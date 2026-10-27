#!/usr/bin/env python3
"""Same-process A/B for KytyPS5 live switches (src/common/liveSwitch.h).

Cycles one live KYTY_* switch through VALUES, one slot of --slot seconds each, while the emulator
runs, by rewriting the file KYTY_LIVE_FILE names. The emulator applies each change at its next
guest flip and logs "Live: seq N applied at CP flip F (t_ms ..., t_us ...)", which places every
slot exactly in frame-times.csv (same clock). Waits for one emulator run, then writes KEY= (the
switch's default) and exits, so later runs are unaffected.

  live_ab.py --file ~/.local/state/kytyps5-live/switches.txt \
             --key KYTY_FAULT_AHEAD_ADAPT --values 1024,256 --slot 60
"""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import time


def running(pattern):
    return subprocess.run(['pgrep', '-f', pattern], capture_output=True).returncode == 0


def write(path, seq, key, value):
    # Atomic: the emulator polls size and time every 250 ms and must never read half a file.
    temporary = path.with_suffix('.tmp')
    temporary.write_text(f'KYTY_LIVE_SEQ={seq}\n{key}={value}\n')
    temporary.replace(path)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--file', type=Path, required=True)
    parser.add_argument('--key', required=True)
    parser.add_argument('--values', required=True, help='comma-separated, cycled in order')
    parser.add_argument('--slot', type=float, default=60.0, help='seconds per value')
    parser.add_argument('--pattern', default='/libexec/kyty_emulator', help='pgrep -f pattern of the run')
    parser.add_argument('--wait', type=float, default=36 * 3600, help='seconds to wait for the run to start')
    parser.add_argument('--wait-poll', type=float, default=5.0, help='seconds between checks while waiting')
    parser.add_argument('--poll', type=float, default=1.0)
    parser.add_argument('--min-run', type=float, default=600.0,
                        help='a run shorter than this does not count: wait for the next one')
    args = parser.parse_args(argv)
    values = [value.strip() for value in args.values.split(',') if value.strip()]
    if not args.key.startswith('KYTY_') or len(values) < 2 or args.slot <= 0:
        parser.error('need a KYTY_ key, at least two values and a positive slot')
    args.file.parent.mkdir(parents=True, exist_ok=True)
    deadline = time.monotonic() + args.wait
    seq = 0
    while True:
        while not running(args.pattern):
            if time.monotonic() > deadline:
                print('no run started', flush=True)
                return 1
            time.sleep(args.wait_poll)
        began = time.monotonic()
        try:
            slot = 0
            while running(args.pattern):
                value = values[slot % len(values)]
                slot += 1
                seq += 1
                write(args.file, seq, args.key, value)
                print(f'{time.time():.3f} seq {seq} {args.key}={value}', flush=True)
                slot_end = time.monotonic() + args.slot
                while time.monotonic() < slot_end and running(args.pattern):
                    time.sleep(min(args.poll, max(0.0, slot_end - time.monotonic())))
        finally:
            seq += 1
            write(args.file, seq, args.key, '')
            print(f'{time.time():.3f} seq {seq} {args.key}= (default restored)', flush=True)
        if time.monotonic() - began >= args.min_run:
            return 0
        print(f'run lasted {time.monotonic() - began:.0f} s, waiting for a longer one', flush=True)


if __name__ == '__main__':
    sys.exit(main())
