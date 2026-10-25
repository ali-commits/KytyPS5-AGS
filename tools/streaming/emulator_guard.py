#!/usr/bin/env python3
"""Shared Linux entry point for GUI and direct emulator launches."""
import fcntl
import os
from pathlib import Path
import subprocess
import sys


def capture(native, args, lock_fd):
    import datetime
    import hashlib
    import json
    import signal
    import threading
    import time

    prefix = Path(__file__).resolve().parent
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%S.%fZ')
    output = prefix/'_CrashCapture'/stamp
    output.mkdir(parents=True)
    metadata = {'started_at_utc': stamp, 'command': [str(native), *args],
                'native_sha256': hashlib.sha256(native.read_bytes()).hexdigest(),
                'cwd': os.getcwd(), 'capture_only': True,
                'effective_settings': {k: v for k, v in os.environ.items()
                    if k.upper().startswith(('KYTY_', 'TRACY_', '__NV_PRIME', '__VK_LAYER'))}}
    (output/'manifest.json').write_text(json.dumps(metadata, indent=2)+'\n')
    for name in ('kyty_run.sh', 'Kyty.ini', 'u59-preset.json', 'installation-provenance.json'):
        source = prefix/name
        if source.is_file():
            (output/name).write_bytes(source.read_bytes())
    print('Crash capture:', output, flush=True)
    done = threading.Event()
    def diagnostic_error(name, error):
        try:
            (output/(name+'-capture-error.txt')).write_text(str(error)+'\n')
        except OSError:
            pass
    # Optional diagnostics are isolated from launching and never prevent gameplay.
    import importlib.util
    def diagnostic(name):
        spec = importlib.util.spec_from_file_location(name, prefix/'libexec'/(name+'.py'))
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        return module
    stacks = None
    options = {}
    try:
        stacks = diagnostic('hang_stacks')
        env, preexec = stacks.launch_options(output)
        options = {'env': env, 'preexec_fn': preexec}
        metadata['effective_settings'] = {k: v for k, v in env.items()
            if k.upper().startswith(('KYTY_', 'TRACY_', '__NV_PRIME', '__VK_LAYER'))}
    except Exception as error:
        diagnostic_error('hang', error)
    child = subprocess.Popen([str(native), *args], stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, bufsize=0, pass_fds=(lock_fd,), **options)
    diagnostic_threads = []
    try:
        sampler = diagnostic('runtime_resources')
        thread = sampler.start_capture(child.pid, output, done)
        if thread is not None:
            diagnostic_threads.append(thread)
    except Exception as error:
        diagnostic_error('resource', error)
    if stacks is not None and options:
        try:
            diagnostic_threads.append(stacks.start_capture(child, output,
                options['env']['KYTY_HANG_TRACE_DIR'], done))
        except Exception as error:
            diagnostic_error('hang', error)
    metadata['pid'] = child.pid
    (output/'manifest.json').write_text(json.dumps(metadata, indent=2)+'\n')
    old_handlers = {}
    def forward(sig, _frame):
        if child.poll() is None:
            child.send_signal(sig)
    for sig in (signal.SIGINT, signal.SIGTERM):
        old_handlers[sig] = signal.signal(sig, forward)
    def mappings():
        proc = Path('/proc')/str(child.pid)
        for delay, name in ((0, 'startup'), (10, 'loaded')):
            if done.wait(delay):
                return
            for _ in range(100):
                try:
                    if (proc/'exe').resolve(strict=True) == native.resolve():
                        (output/(name+'-maps.txt')).write_bytes((proc/'maps').read_bytes())
                        (output/(name+'-stat.txt')).write_bytes((proc/'stat').read_bytes())
                        return_if_loaded = name == 'loaded'
                        if return_if_loaded:
                            return
                        break
                except (OSError, RuntimeError):
                    pass
                if done.wait(0.02):
                    return
    reader = threading.Thread(target=mappings, name='crash-maps', daemon=True)
    reader.start()
    try:
        console_open = True
        with (output/'terminal.log').open('wb', buffering=0) as log:
            while chunk := os.read(child.stdout.fileno(), 65536):
                log.write(chunk)
                if console_open:
                    try:
                        sys.stdout.buffer.write(chunk)
                        sys.stdout.buffer.flush()
                    except (BrokenPipeError, OSError):
                        console_open = False
        status = child.wait()
    finally:
        done.set()
        reader.join(timeout=1)
        for thread in diagnostic_threads:
            thread.join(timeout=5)
        if child.poll() is None:
            child.terminate()
            try:
                child.wait(timeout=5)
            except subprocess.TimeoutExpired:
                child.kill()
                child.wait()
        for sig, handler in old_handlers.items():
            signal.signal(sig, handler)
    metadata['exit_code'] = status
    metadata['ended_at_utc'] = datetime.datetime.now(datetime.timezone.utc).isoformat()
    (output/'manifest.json').write_text(json.dumps(metadata, indent=2)+'\n')
    (prefix/'_CrashCapture'/'last-run.txt').write_text(str(output)+'\n')
    print('Saved crash capture:', output, 'exit', status, flush=True)
    return status if status >= 0 else 128-status


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
        return capture(native, args, lock.fileno())


if __name__ == '__main__':
    raise SystemExit(main())
