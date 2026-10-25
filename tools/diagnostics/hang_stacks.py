"""Capture native stacks once a game watchdog reports a sustained stall.

GDB attaches only after the report, then detaches without terminating the game.
No privileged system settings or unrestricted ptrace permissions are required.
"""
import ctypes
import os
from pathlib import Path
import shutil
import subprocess
import sys
import threading


def launch_options(output):
    env = dict(os.environ)
    env.setdefault('KYTY_HANG_TRACE_DIR', str(output / 'hang'))
    env.setdefault('KYTY_FRAME_TIMES_LOG', str(output / 'frame-times.csv'))
    preexec = None
    if sys.platform == 'linux' and shutil.which('gdb'):
        # Yama permits the designated debugger's descendants as well. Authorize this
        # wrapper, whose GDB child will inspect only its own emulator subprocess.
        parent = os.getpid()
        libc = ctypes.CDLL(None)
        libc.prctl.argtypes = [ctypes.c_int, ctypes.c_ulong, ctypes.c_ulong,
                               ctypes.c_ulong, ctypes.c_ulong]
        libc.prctl.restype = ctypes.c_int

        def permit_wrapper_debugger():
            libc.prctl(0x59616d61, parent, 0, 0, 0)  # PR_SET_PTRACER

        preexec = permit_wrapper_debugger
    return env, preexec


def start_capture(child, output, watchdog_dir, stop):
    def watch():
        marker = Path(watchdog_dir) / 'watchdog.txt'
        while not stop.wait(0.25) and child.poll() is None:
            if not marker.is_file():
                continue
            # An explicitly reused trace directory can contain an older game's report.
            try:
                with marker.open() as report:
                    if f'pid={child.pid} ' not in report.read(512):
                        continue
            except OSError:
                continue
            with (output / 'native-stacks.txt').open('w') as log:
                debugger = shutil.which('gdb')
                if not debugger:
                    log.write('GDB unavailable; watchdog state is preserved.\n')
                    return
                try:
                    result = subprocess.run([debugger, '--batch', '-nx',
                                    '-iex', 'set auto-load off',
                                    '-iex', 'set debuginfod enabled off',
                                    '-ex', 'set pagination off',
                                    '-ex', 'set print frame-arguments none',
                                    '-ex', 'thread apply all bt 24',
                                    '-ex', 'info proc mappings', '-ex', 'detach',
                                    '--pid', str(child.pid)],
                                   stdin=subprocess.DEVNULL, stdout=log,
                                   stderr=subprocess.STDOUT, timeout=15)
                    log.write(f'\nDebugger exit code: {result.returncode}\n')
                except (OSError, subprocess.TimeoutExpired) as error:
                    log.write(f'Native stack capture failed: {error}\n')
            return

    thread = threading.Thread(target=watch, name='HangStackCapture', daemon=True)
    thread.start()
    return thread
