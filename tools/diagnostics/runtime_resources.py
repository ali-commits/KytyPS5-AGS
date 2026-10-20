"""Low-rate Linux diagnostics for one game process; no external Python dependencies.

Call start_capture(pid, output_dir, stop_event), then set the event and join the
returned daemon thread during wrapper cleanup. CPU percent uses 100% per logical
CPU; NVIDIA telemetry is device-wide. No smaps/PSS reads or profiler attachment.
"""
from datetime import datetime, timezone
import json
import math
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import threading
import time


GPU_FIELDS = (
    "timestamp,index,uuid,utilization.gpu,utilization.memory,memory.used,power.draw,"
    "temperature.gpu,clocks.current.graphics,clocks.current.memory,pstate,"
    "clocks_event_reasons.active,clocks_event_reasons.sw_thermal_slowdown,"
    "clocks_event_reasons.hw_thermal_slowdown,clocks_event_reasons.sw_power_cap,"
    "clocks_event_reasons.hw_slowdown,clocks_event_reasons.hw_power_brake_slowdown"
)


def _text(path):
    try:
        return path.read_text(errors="replace")
    except OSError:
        return ""


def _parse_stat(text):
    left, right = text.find("("), text.rfind(")")
    if left < 1 or right <= left:
        raise ValueError("invalid proc stat name")
    fields = text[right + 1:].split()
    if len(fields) < 37:
        raise ValueError("truncated proc stat")
    return {"pid": int(text[:left]), "name": text[left + 1:right], "state": fields[0],
            "minor_faults": int(fields[7]), "major_faults": int(fields[9]),
            "cpu_ticks": int(fields[11]) + int(fields[12]), "thread_count": int(fields[17]),
            "start_ticks": int(fields[19]), "rss_pages": int(fields[21]),
            "last_cpu": int(fields[36])}


def _stat(path):
    try:
        return _parse_stat(_text(path))
    except ValueError:
        return None


def _cpu_percent(current, previous, elapsed, ticks_per_second):
    if previous is None or current < previous or elapsed <= 0:
        return None
    return round(100.0 * (current - previous) / ticks_per_second / elapsed, 2)


def _cpu_topology(root=Path("/sys/devices/system/cpu")):
    cpus = []
    for path in root.glob("cpu[0-9]*"):
        if not path.name[3:].isdigit():
            continue
        cpu = {"cpu": int(path.name[3:]), "core_class_hint": "unknown"}
        for key, relative in (("core_id", "topology/core_id"),
                              ("package_id", "topology/physical_package_id"),
                              ("core_type_raw", "topology/core_type"),
                              ("max_frequency_khz", "cpufreq/cpuinfo_max_freq")):
            try:
                cpu[key] = int(_text(path / relative).strip())
            except ValueError:
                pass
        siblings = _text(path / "topology/thread_siblings_list").strip()
        cpu["thread_siblings"] = siblings
        count = 0
        try:
            for part in siblings.split(","):
                ends = [int(value) for value in part.split("-")]
                count += ends[-1] - ends[0] + 1
        except ValueError:
            count = 0
        cpu["smt_threads"] = count
        cpus.append(cpu)
    # Label only the clear mixed-SMT/frequency pattern; retain raw facts for interpretation.
    smt = [cpu.get("max_frequency_khz", 0) for cpu in cpus if cpu["smt_threads"] > 1]
    single = [cpu.get("max_frequency_khz", 0) for cpu in cpus if cpu["smt_threads"] == 1]
    if smt and single and min(single) > 0 and max(single) < min(smt):
        for cpu in cpus:
            if cpu["smt_threads"]:
                cpu["core_class_hint"] = "performance-like" if cpu["smt_threads"] > 1 else "efficiency-like"
    return sorted(cpus, key=lambda cpu: cpu["cpu"])


def _numbers(path):
    result = {}
    for line in _text(path).splitlines():
        key, separator, value = line.partition(":")
        if separator:
            try:
                result[key] = int(value.split()[0])
            except (ValueError, IndexError):
                pass
    return result


def _threads(process, previous, elapsed, hz):
    current = {}
    try:
        paths = list((process / "task").iterdir())
    except OSError:
        return current, []
    for path in paths:
        thread = _stat(path / "stat")
        if thread is None:
            continue
        tid = thread["pid"]
        old = previous.get(tid)
        before = old["cpu_ticks"] if old and old["start_ticks"] == thread["start_ticks"] else None
        thread["cpu_percent"] = _cpu_percent(thread["cpu_ticks"], before, elapsed, hz)
        current[tid] = thread
    selected = [{"tid": thread["pid"], "name": thread["name"], "state": thread["state"],
                 "cpu_percent": thread["cpu_percent"], "last_cpu": thread["last_cpu"]}
                for thread in current.values()]
    selected.sort(key=lambda thread: (-(thread["cpu_percent"] or 0), thread["tid"]))
    return current, selected


def _save_metadata(path, metadata):
    try:
        temporary = path.with_suffix(".tmp")
        temporary.write_text(json.dumps(metadata, indent=2) + "\n")
        temporary.replace(path)
    except OSError:
        pass


def _capture(pid, output_dir, stop, interval_s, nvidia_smi):
    gpu = gpu_output = gpu_error = None
    metadata = {"pid": pid, "interval_s": interval_s, "started_at": datetime.now(timezone.utc).isoformat(),
                "cpu_percent_definition": "100 percent is one logical CPU; process totals may exceed 100",
                "core_class_hint_definition": "Heuristic only: SMT cores with higher maximum frequency versus lower-frequency single-thread cores",
                "gpu_scope": "Device-wide, not per-process", "gpu_fields": GPU_FIELDS.split(","),
                "end_reason": "stop-event"}
    metadata_path = output_dir / "resource-metadata.json"
    try:
        output_dir.mkdir(parents=True, exist_ok=True)
        hz, page_size = os.sysconf("SC_CLK_TCK"), os.sysconf("SC_PAGE_SIZE")
        metadata.update(clock_ticks_per_second=hz, page_size=page_size, cpu_topology=_cpu_topology())
        try:
            metadata["allowed_cpus"] = sorted(os.sched_getaffinity(pid))
        except (AttributeError, OSError):
            pass
        process = Path("/proc") / str(pid)
        initial = _stat(process / "stat")
        if initial is None or initial["state"] in ("Z", "X"):
            metadata["end_reason"] = "process-exited"
            return
        metadata["process_start_ticks"] = initial["start_ticks"]
        command = nvidia_smi if nvidia_smi is not None else shutil.which("nvidia-smi")
        if command and not stop.is_set():
            try:
                gpu_output = (output_dir / "gpu.csv").open("w")
                gpu_error = tempfile.TemporaryFile(dir=output_dir)
                argv = [str(command), "--query-gpu=" + GPU_FIELDS,
                        "--format=csv,noheader,nounits", "--loop-ms=1000"]
                gpu = subprocess.Popen(argv, stdin=subprocess.DEVNULL, stdout=gpu_output,
                                       stderr=gpu_error, close_fds=True)
                metadata.update(gpu_pid=gpu.pid, gpu_command=argv)
            except OSError as error:
                metadata["gpu_error"] = str(error)
        else:
            metadata["gpu_error"] = "nvidia-smi unavailable" if not command else "capture already stopped"
        _save_metadata(metadata_path, metadata)
        previous = previous_io = None
        previous_threads = {}
        began = previous_time = time.monotonic()
        with (output_dir / "resources.jsonl").open("w", buffering=1) as output:
            while not stop.is_set():
                now = time.monotonic()
                current = _stat(process / "stat")
                if current is None or current["state"] in ("Z", "X"):
                    metadata["end_reason"] = "process-exited"
                    break
                if current["start_ticks"] != initial["start_ticks"]:
                    metadata["end_reason"] = "pid-reused"
                    break
                elapsed = now - previous_time
                status, io = _numbers(process / "status"), _numbers(process / "io")
                previous_threads, threads = _threads(process, previous_threads, elapsed, hz)
                row = {"utc": datetime.now(timezone.utc).isoformat(), "monotonic_s": now,
                       "elapsed_s": round(now - began, 6), "pid": pid, "state": current["state"],
                       "cpu_percent": _cpu_percent(current["cpu_ticks"], previous["cpu_ticks"] if previous else None, elapsed, hz),
                       "rss_bytes": max(0, current["rss_pages"]) * page_size,
                       "peak_rss_bytes": status["VmHWM"] * 1024 if "VmHWM" in status else None,
                       "minor_faults": current["minor_faults"], "major_faults": current["major_faults"],
                       "thread_count": current["thread_count"], "io": io, "top_threads": threads[:8],
                       "renderer_threads": [thread for thread in threads if any(
                           token in thread["name"].lower() for token in ("cp", "command", "drawprep", "render", "gpu"))][:32]}
                if previous:
                    row["minor_faults_delta"] = current["minor_faults"] - previous["minor_faults"]
                    row["major_faults_delta"] = current["major_faults"] - previous["major_faults"]
                if previous_io is not None:
                    row["io_delta"] = {key: value - previous_io[key] for key, value in io.items()
                                       if key in previous_io and value >= previous_io[key]}
                output.write(json.dumps(row) + "\n")
                previous, previous_io, previous_time = current, io, now
                if stop.wait(max(0, interval_s - (time.monotonic() - now))):
                    break
    except Exception as error:
        metadata.update(end_reason="capture-error", capture_error=f"{type(error).__name__}: {error}")
    finally:
        if gpu is not None:
            try:
                if gpu.poll() is None:
                    gpu.terminate()
                try:
                    gpu.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    gpu.kill()
                    gpu.wait(timeout=2)
                metadata["gpu_returncode"] = gpu.returncode
            except (OSError, subprocess.TimeoutExpired) as error:
                metadata["gpu_cleanup_error"] = str(error)
        if gpu_error is not None:
            try:
                gpu_error.seek(0)
                message = gpu_error.read(2048).decode(errors="replace").strip()
                if message:
                    metadata["gpu_stderr"] = message
            except OSError:
                pass
            try:
                gpu_error.close()
            except OSError:
                pass
        if gpu_output is not None:
            try:
                gpu_output.close()
            except OSError:
                pass
        metadata["ended_at"] = datetime.now(timezone.utc).isoformat()
        _save_metadata(metadata_path, metadata)


def start_capture(pid, output_dir, stop, *, interval_s=1.0, nvidia_smi=None):
    """Start diagnostics, returning a daemon Thread (None if startup is unavailable).

    The wrapper owns stop: set it in finally and join(timeout=5) the returned
    thread. The sampler also stops on process exit or PID reuse. Optional keyword
    arguments support isolated tests; normal callers need only the first three.
    """
    try:
        if not isinstance(pid, int) or pid <= 0 or not math.isfinite(interval_s) or interval_s < 0.02:
            return None
        thread = threading.Thread(target=_capture, args=(pid, Path(output_dir), stop, interval_s, nvidia_smi),
                                  name="RuntimeResourceCapture", daemon=True)
        thread.start()
        return thread
    except Exception:
        return None
