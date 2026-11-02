#!/usr/bin/env python3
"""Shared Linux entry point for GUI and direct emulator launches."""
import fcntl
import os
from pathlib import Path
import re
import subprocess
import stat
import sys


def settings_of(env):
    return {k: v for k, v in env.items()
            if k.upper().startswith(('KYTY_', 'TRACY_', '__NV_PRIME', '__VK_LAYER'))}


def runtime_settings(prefix, env):
    """Applies runtime-env.json (next to this file) to `env` and returns what it did.

    The launcher keeps only the KYTY_* variables of the shared u59-preset.json, so a runtime's own
    experiments and diagnostics are set here, after the GUI: {"KYTY_NAME": "value"}; null removes
    a variable; keys starting with "_" are notes. Other names and values are ignored and reported.
    """
    import hashlib
    import json

    path = prefix/'runtime-env.json'
    try:
        raw = path.read_bytes()
    except FileNotFoundError:
        return None
    report = {'file': str(path), 'sha256': hashlib.sha256(raw).hexdigest(),
              'set': {}, 'removed': [], 'ignored': {}}
    data = json.loads(raw)
    if not isinstance(data, dict):
        report['ignored']['(file)'] = 'not a JSON object'
        return report
    for name, value in data.items():
        if name.startswith('_'):
            continue
        if not re.fullmatch(r'KYTY_[A-Z0-9_]+', name):
            report['ignored'][name] = 'only KYTY_* names'
        elif value is None:
            env.pop(name, None)
            report['removed'].append(name)
        elif isinstance(value, str):
            env[name] = value
            report['set'][name] = value
        else:
            report['ignored'][name] = 'value must be a string or null'
    changes = [f'{k}={v}' for k, v in report['set'].items()] + [f'-{k}' for k in report['removed']]
    print('Runtime settings (runtime-env.json):', ', '.join(changes) or 'none',
          *(f'(ignored {k}: {v})' for k, v in report['ignored'].items()), flush=True)
    return report


def capture(native, args, lock_fd):
    import datetime
    import hashlib
    import json
    import signal
    import threading
    import time

    prefix = Path(__file__).resolve().parent
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%S.%fZ')
    mutable = Path(os.environ.get('AGS_SESSION_RUNTIME', str(prefix)))
    output = mutable/'_CrashCapture'/stamp
    output.mkdir(parents=True)
    metadata = {'started_at_utc': stamp, 'command': [str(native), *args],
                'native_sha256': hashlib.sha256(native.read_bytes()).hexdigest(),
                'cwd': os.getcwd(), 'capture_only': True,
                'effective_settings': settings_of(os.environ)}
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
    except Exception as error:
        diagnostic_error('hang', error)
    try:
        metadata['runtime_env'] = runtime_settings(prefix, options.setdefault('env', dict(os.environ)))
    except Exception as error:
        diagnostic_error('runtime-env', error)
    if 'AGS_SESSION_RUNTIME' in os.environ:
        # Session ownership wins over inherited or installation-level diagnostic paths.
        options.setdefault('env', dict(os.environ))['KYTY_HANG_TRACE_DIR'] = str(output/'hang')
        options['env']['KYTY_FRAME_TIMES_LOG'] = str(output/'frame-times.csv')
    if 'AGS_EMULATOR_LOCK_FD' in os.environ:
        # If the native AGS supervisor is killed abruptly it kills this wrapper via
        # PDEATHSIG; propagate that lifetime to the wrapped native child as well.
        import ctypes
        previous_preexec = options.get('preexec_fn')
        wrapper_pid = os.getpid()
        def owned_preexec():
            libc = ctypes.CDLL(None, use_errno=True)
            if libc.prctl(1, signal.SIGKILL, 0, 0, 0) != 0 or os.getppid() != wrapper_pid:
                os._exit(127)
            if previous_preexec is not None:
                previous_preexec()
        options['preexec_fn'] = owned_preexec
    # What the native program actually receives (KYTY_FAULT_MAP is read at its static init).
    metadata['effective_settings'] = settings_of(options.get('env', os.environ))
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
    (mutable/'_CrashCapture'/'last-run.txt').write_text(str(output)+'\n')
    print('Saved crash capture:', output, 'exit', status, flush=True)
    return status if status >= 0 else 128-status


def main():
    native = Path(__file__).resolve().parent/'libexec/kyty_emulator'
    args = sys.argv[1:]
    # Version/help probes made by the GUI must work while a game is running.
    if not any(a == '--game' or a.startswith('--game=') for a in args):
        os.execv(native, [str(native), *args])
    try:
        inherited = os.environ.get('AGS_EMULATOR_LOCK_FD')
        if inherited is not None:
            # Native AGS has already claimed the one-use owner reservation and acquired
            # the lifetime lock. Never reopen that lock as a competing owner.
            fd = int(inherited)
            path = Path(os.environ['AGS_EMULATOR_LOCK_PATH'])
            expected = path.lstat()
            actual = os.fstat(fd)
            if (not path.is_absolute() or not stat.S_ISREG(expected.st_mode)
                    or expected.st_uid != os.geteuid() or expected.st_mode & 0o077
                    or (expected.st_dev, expected.st_ino) != (actual.st_dev, actual.st_ino)):
                raise ValueError('invalid inherited lifetime lock')
            with private_lock(path) as probe:
                try:
                    fcntl.flock(probe, fcntl.LOCK_EX | fcntl.LOCK_NB)
                except BlockingIOError:
                    pass
                else:
                    raise ValueError('inherited lifetime lock is not held')
            return capture(native, args, fd)
        state = Path(os.environ.get('XDG_RUNTIME_DIR', str(Path.home()/'.cache')))/'kytyps5'
        state.mkdir(mode=0o700, parents=True, exist_ok=True)
        with private_lock(state/'emulator.lock.launch') as admission:
            fcntl.flock(admission, fcntl.LOCK_EX | fcntl.LOCK_NB)
            marker = state/'emulator.lock.host-steam-owner'
            if marker.exists() or marker.is_symlink():
                raise ValueError('Steam ownership recovery required')
            with private_lock(state/'emulator.lock') as lock:
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                if legacy_emulator_running():
                    raise ValueError('another emulator is running')
                # The child now owns exclusion; release admission without ever closing or
                # unlocking the lifetime descriptor inherited by the intended child only.
                fcntl.flock(admission, fcntl.LOCK_UN)
                return capture(native, args, lock.fileno())
    except (OSError, ValueError, KeyError):
        print('host-busy: emulator launch reservation unavailable', file=sys.stderr)
        return 1


def private_lock(path):
    parent = path.parent
    if not path.is_absolute() or any(part.is_symlink() for part in [parent, *parent.parents]):
        raise ValueError('unsafe lock parent')
    info = parent.stat()
    if info.st_uid != os.geteuid() or info.st_mode & 0o077:
        raise ValueError('unsafe lock directory')
    fd = os.open(path, os.O_RDWR | os.O_CREAT | os.O_CLOEXEC | os.O_NOFOLLOW, 0o600)
    info = os.fstat(fd)
    if not stat.S_ISREG(info.st_mode) or info.st_uid != os.geteuid() or info.st_mode & 0o077:
        os.close(fd)
        raise ValueError('unsafe lock file')
    return os.fdopen(fd, 'a')


def legacy_emulator_running():
    # Advisory detection for an already running legacy direct ELF. Atomic exclusion
    # requires every ordinary launcher to install/use this guard.
    for process in Path('/proc').iterdir():
        if not process.name.isdigit() or int(process.name) == os.getpid():
            continue
        try:
            if process.stat().st_uid == os.geteuid() and (process/'comm').read_text().strip() == 'kyty_emulator':
                return True
        except (OSError, ProcessLookupError):
            pass
    return False


if __name__ == '__main__':
    raise SystemExit(main())
