#!/usr/bin/env python3
"""Shared Linux entry point for GUI and direct emulator launches."""
import fcntl
import os
from pathlib import Path
import subprocess
import sys


def main():
    native = Path(__file__).resolve().parent/'libexec/kyty_emulator'
    args = sys.argv[1:]
    # Version/help probes made by the GUI must work while a game is running.
    if not any(a == '--game' or a.startswith('--game=') for a in args):
        os.execv(native, [str(native), *args])
    state = Path(os.environ.get('XDG_RUNTIME_DIR', str(Path.home()/'.cache')))/'kytyps5'
    state.mkdir(mode=0o700, parents=True, exist_ok=True)
    with (state/'emulator.lock').open('a') as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            print('An emulator is already running. Close it before launching another.', file=sys.stderr)
            return 1
        check = subprocess.run(['pgrep', '-x', 'kyty_emulator'], capture_output=True, text=True)
        if check.returncode != 1:
            print('Another emulator is running, or its process count could not be checked.', file=sys.stderr)
            return 1
        os.set_inheritable(lock.fileno(), True)
        os.execv(native, [str(native), *args])


if __name__ == '__main__':
    raise SystemExit(main())
