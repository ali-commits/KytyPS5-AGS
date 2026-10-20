import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from unittest import mock


MODULE = Path(__file__).resolve().parents[1] / "tools/diagnostics/runtime_resources.py"
SPEC = importlib.util.spec_from_file_location("runtime_resources", MODULE)
resources = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(resources)


def wait_for(predicate, seconds=3):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(0.02)
    raise AssertionError("timed out waiting for capture")


def rows(path):
    try:
        return [json.loads(line) for line in path.read_text().splitlines() if line.endswith("}")]
    except (OSError, ValueError):
        return []


class RuntimeResourcesTests(unittest.TestCase):
    def test_stat_parser_handles_spaces_and_parentheses_in_thread_names(self):
        fields = ["0"] * 50
        for index, value in {0: "R", 7: 17, 9: 2, 11: 120, 12: 30, 17: 7,
                             19: 98765, 21: 4096, 36: 19}.items():
            fields[index] = str(value)
        result = resources._parse_stat("412 (CP (submit) worker) " + " ".join(fields))
        self.assertEqual(result, {"pid": 412, "name": "CP (submit) worker", "state": "R",
                                 "minor_faults": 17, "major_faults": 2, "cpu_ticks": 150,
                                 "thread_count": 7, "start_ticks": 98765,
                                 "rss_pages": 4096, "last_cpu": 19})
        with self.assertRaises(ValueError):
            resources._parse_stat("412 (truncated) R 1 2")

    def test_cpu_delta_uses_actual_elapsed_time_and_one_core_is_100_percent(self):
        self.assertEqual(resources._cpu_percent(250, 150, 0.5, 100), 200.0)
        self.assertIsNone(resources._cpu_percent(10, 20, 1, 100))

    def test_topology_keeps_both_performance_frequencies_in_same_hint(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for cpu, frequency, siblings in [(0, 5100000, "0-1"), (1, 5200000, "0-1"),
                                               (2, 4100000, "2")]:
                path = root / f"cpu{cpu}"
                (path / "topology").mkdir(parents=True)
                (path / "cpufreq").mkdir()
                (path / "topology/thread_siblings_list").write_text(siblings)
                (path / "cpufreq/cpuinfo_max_freq").write_text(str(frequency))
            result = resources._cpu_topology(root)
            self.assertEqual([cpu["max_frequency_khz"] for cpu in result],
                             [5100000, 5200000, 4100000])
            self.assertEqual([cpu["core_class_hint"] for cpu in result],
                             ["performance-like", "performance-like", "efficiency-like"])

    def make_gpu(self, directory):
        pid_file = directory / "gpu-pid"
        helper = directory / "fake-nvidia-smi"
        helper.write_text(
            f"#!{sys.executable}\nimport os,time\nfrom pathlib import Path\n"
            f"Path({str(pid_file)!r}).write_text(str(os.getpid()))\n"
            "while True:\n print('fake,0,uuid,50,20,100,80,60,1800,9000,P0,0,0,0,0',flush=True)\n"
            " time.sleep(.03)\n")
        helper.chmod(0o700)
        return helper, pid_file

    def test_stop_event_reaps_gpu_and_records_owned_cpu_child(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            helper, pid_file = self.make_gpu(root)
            child = subprocess.Popen([sys.executable, "-c",
                                      "import time\nend=time.monotonic()+5\n"
                                      "while time.monotonic()<end: pass"],
                                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            stop = threading.Event()
            thread = resources.start_capture(child.pid, root / "capture", stop,
                                              interval_s=0.05, nvidia_smi=str(helper))
            try:
                self.assertTrue(thread.daemon)
                wait_for(lambda: pid_file.exists() and any(
                    (row.get("cpu_percent") or 0) > 0 for row in rows(root / "capture/resources.jsonl")))
                stop.set()
                thread.join(3)
                self.assertFalse(thread.is_alive())
                captured = rows(root / "capture/resources.jsonl")
                active = next(row for row in captured if (row.get("cpu_percent") or 0) > 0)
                self.assertGreater(active["rss_bytes"], 0)
                self.assertGreater(active["peak_rss_bytes"], 0)
                self.assertIn("read_bytes", active["io"])
                self.assertTrue(active["top_threads"])
                self.assertIsInstance(active["top_threads"][0]["last_cpu"], int)
                self.assertGreater((root / "capture/gpu.csv").stat().st_size, 0)
                metadata = json.loads((root / "capture/resource-metadata.json").read_text())
                self.assertEqual(metadata["end_reason"], "stop-event")
                self.assertEqual(metadata["gpu_pid"], int(pid_file.read_text()))
                with self.assertRaises(ChildProcessError):
                    os.waitpid(int(pid_file.read_text()), os.WNOHANG)
            finally:
                stop.set()
                if thread:
                    thread.join(3)
                if child.poll() is None:
                    child.terminate()
                child.wait(timeout=3)

    def test_target_exit_stops_capture_and_reaps_gpu_without_stop_event(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            helper, pid_file = self.make_gpu(root)
            child = subprocess.Popen([sys.executable, "-c", "import time;time.sleep(.3)"])
            stop = threading.Event()
            thread = resources.start_capture(child.pid, root / "capture", stop,
                                              interval_s=0.03, nvidia_smi=str(helper))
            try:
                wait_for(pid_file.exists)
                child.wait(timeout=3)
                thread.join(3)
                self.assertFalse(thread.is_alive())
                self.assertFalse(stop.is_set())
                metadata = json.loads((root / "capture/resource-metadata.json").read_text())
                self.assertEqual(metadata["end_reason"], "process-exited")
                with self.assertRaises(ChildProcessError):
                    os.waitpid(int(pid_file.read_text()), os.WNOHANG)
            finally:
                stop.set()
                if thread:
                    thread.join(3)
                if child.poll() is None:
                    child.terminate()
                child.wait(timeout=3)

    def test_missing_gpu_and_unwritable_destination_do_not_escape(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            stop = threading.Event()
            thread = resources.start_capture(os.getpid(), root / "capture", stop,
                                              interval_s=0.03, nvidia_smi=str(root / "missing-gpu"))
            try:
                wait_for(lambda: bool(rows(root / "capture/resources.jsonl")))
            finally:
                stop.set()
                thread.join(3)
            metadata = json.loads((root / "capture/resource-metadata.json").read_text())
            self.assertIn("gpu_error", metadata)
            blocked = root / "file"
            blocked.write_text("not a directory")
            failed = resources.start_capture(os.getpid(), blocked / "capture", threading.Event())
            failed.join(3)
            self.assertFalse(failed.is_alive())

    def test_missing_status_reports_unknown_peak_rss(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            stop = threading.Event()
            original = resources._numbers
            with mock.patch.object(resources, "_numbers", side_effect=lambda path:
                                   {} if path.name == "status" else original(path)):
                thread = resources.start_capture(os.getpid(), root, stop, interval_s=0.03,
                                                  nvidia_smi=str(root / "missing-gpu"))
                try:
                    wait_for(lambda: bool(rows(root / "resources.jsonl")))
                finally:
                    stop.set()
                    thread.join(3)
            self.assertIsNone(rows(root / "resources.jsonl")[0]["peak_rss_bytes"])


if __name__ == "__main__":
    unittest.main()
