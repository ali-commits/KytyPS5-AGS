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

    def test_cpu_mhz_reads_each_cpu_clock_and_tolerates_missing_or_bad_files(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for cpu, text in [(0, "5200000\n"), (2, "garbage")]:
                (root / f"cpu{cpu}" / "cpufreq").mkdir(parents=True)
                (root / f"cpu{cpu}" / "cpufreq" / "scaling_cur_freq").write_text(text)
            self.assertEqual(resources._cpu_mhz([0, 1, 2], root), [5200, None, None])

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

    def capture_with_proc_inputs(self, reader, count=1):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            stop = threading.Event()
            with mock.patch.object(resources, "_text", side_effect=reader):
                thread = resources.start_capture(os.getpid(), root, stop, interval_s=0.03,
                                                  nvidia_smi=str(root / "missing-gpu"))
                try:
                    wait_for(lambda: len(rows(root / "resources.jsonl")) >= count)
                finally:
                    stop.set()
                    thread.join(3)
            self.assertFalse(thread.is_alive())
            return rows(root / "resources.jsonl"), json.loads(
                (root / "resource-metadata.json").read_text())

    def test_memory_samples_distinguish_process_swap_from_host_pressure_and_page_io(self):
        original = resources._text
        vmstat_reads = 0
        taint_reads = 0

        def read(path):
            nonlocal vmstat_reads, taint_reads
            if path == Path(f"/proc/{os.getpid()}/status"):
                return "\n".join("VmSwap:\t13 kB" if line.startswith("VmSwap:") else line
                                 for line in original(path).splitlines())
            if path == Path("/proc/meminfo"):
                return ("MemTotal: 32000 kB\nMemAvailable: 1234 kB\nSwapTotal: 8192 kB\n"
                        "SwapFree: 0 kB\nCached: 300 kB\nShmem: 7 kB\n")
            if path == Path("/proc/vmstat"):
                values = [(100, 200), (103, 205), (2, 1)]
                incoming, outgoing = values[min(vmstat_reads, 2)]
                vmstat_reads += 1
                return f"pswpin {incoming}\npswpout {outgoing}\npgfault 90000\n"
            if path == Path("/proc/pressure/memory"):
                return ("some avg10=1.25 avg60=0.50 avg300=0.10 total=12345\n"
                        "full avg10=0.25 avg60=0.00 avg300=0.00 total=67\n")
            if path == Path("/proc/sys/kernel/random/boot_id"):
                return "12345678-1234-1234-1234-123456789abc\n"
            if path == Path("/proc/sys/kernel/osrelease"):
                return "test-kernel\n"
            if path == Path("/proc/sys/kernel/tainted"):
                taint_reads += 1
                return "0\n" if taint_reads == 1 else "128\n"
            return original(path)

        captured, metadata = self.capture_with_proc_inputs(read, count=3)
        first = captured[0]
        self.assertIn("swap_bytes", first)
        self.assertEqual(first["swap_bytes"], 13312)
        self.assertEqual(first["host_memory"], {
            "total_bytes": 32768000, "available_bytes": 1263616,
            "swap_total_bytes": 8388608, "swap_free_bytes": 0,
            "cached_bytes": 307200, "shmem_bytes": 7168})
        self.assertEqual(first["host_memory_pressure"], {
            "some": {"avg10_percent": 1.25, "total_us": 12345},
            "full": {"avg10_percent": 0.25, "total_us": 67}})
        self.assertEqual(first["host_swap_pages"], {"in": 100, "out": 200})
        self.assertEqual(first["host_swap_pages_delta"], {"in": None, "out": None})
        self.assertEqual(captured[1]["host_swap_pages_delta"], {"in": 3, "out": 5})
        self.assertEqual(captured[2]["host_swap_pages"], {"in": 2, "out": 1})
        self.assertEqual(captured[2]["host_swap_pages_delta"], {"in": None, "out": None})
        self.assertEqual(metadata["boot_id"], "12345678-1234-1234-1234-123456789abc")
        self.assertEqual(metadata["kernel_release"], "test-kernel")
        self.assertEqual(metadata["kernel_taint_start"], 0)
        self.assertEqual(metadata["kernel_taint_end"], 128)
        self.assertNotIn("capture_error", metadata)

    def test_missing_or_malformed_optional_memory_inputs_remain_unknown(self):
        original = resources._text
        optional = {Path("/proc/meminfo"), Path("/proc/vmstat"),
                    Path("/proc/pressure/memory"), Path("/proc/sys/kernel/tainted"),
                    Path("/proc/sys/kernel/random/boot_id"), Path("/proc/sys/kernel/osrelease")}
        for malformed in (False, True):
            with self.subTest(malformed=malformed):
                def read(path):
                    if path == Path(f"/proc/{os.getpid()}/status"):
                        return "\n".join(line for line in original(path).splitlines()
                                         if not line.startswith("VmSwap:"))
                    if path in optional:
                        if not malformed:
                            return ""
                        if path == Path("/proc/meminfo"):
                            return "MemAvailable: not-a-number kB\nSwapFree: -1 kB\n"
                        if path == Path("/proc/vmstat"):
                            return "pswpin -1\npswpout nope\n"
                        if path == Path("/proc/pressure/memory"):
                            return "some avg10=nan total=-1\nfull avg10=101 total=nope\n"
                        if path == Path("/proc/sys/kernel/tainted"):
                            return "not-a-number\n"
                        return ""
                    return original(path)

                captured, metadata = self.capture_with_proc_inputs(read)
                self.assertIn("swap_bytes", captured[0])
                self.assertIsNone(captured[0]["swap_bytes"])
                self.assertTrue(all(value is None for value in captured[0]["host_memory"].values()))
                self.assertEqual(captured[0]["host_swap_pages"], {"in": None, "out": None})
                self.assertEqual(captured[0]["host_swap_pages_delta"], {"in": None, "out": None})
                self.assertEqual(captured[0]["host_memory_pressure"], {"some": None, "full": None})
                for field in ("boot_id", "kernel_release", "kernel_taint_start", "kernel_taint_end"):
                    self.assertIsNone(metadata[field])
                self.assertNotIn("capture_error", metadata)

    def test_permission_denied_optional_proc_files_do_not_stop_capture(self):
        original = Path.read_text
        optional = {Path("/proc/meminfo"), Path("/proc/vmstat"),
                    Path("/proc/pressure/memory"), Path("/proc/sys/kernel/tainted"),
                    Path("/proc/sys/kernel/random/boot_id"), Path("/proc/sys/kernel/osrelease")}

        def read(path, *args, **kwargs):
            if path in optional:
                raise PermissionError("test proc access denied")
            return original(path, *args, **kwargs)

        with mock.patch.object(Path, "read_text", autospec=True, side_effect=read):
            captured, metadata = self.capture_with_proc_inputs(resources._text)
        self.assertIsNone(captured[0]["host_memory"]["available_bytes"])
        self.assertEqual(captured[0]["host_swap_pages"], {"in": None, "out": None})
        self.assertEqual(captured[0]["host_memory_pressure"], {"some": None, "full": None})
        self.assertIsNone(metadata["kernel_taint_start"])
        self.assertIsNone(metadata["kernel_taint_end"])
        self.assertNotIn("capture_error", metadata)

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
