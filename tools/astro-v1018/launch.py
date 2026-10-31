#!/usr/bin/env python3
"""Launch the verified Astro Bot configuration with a single-instance guard."""
import argparse
import datetime
import fcntl
import hashlib
import json
import os
import shutil
from pathlib import Path
import subprocess
import sys

from validate_profile import validate


def launch(config, dry_run=False):
    required = ('game', 'emulator', 'runtime', 'preset', 'profile')
    paths = {key: Path(config[key]).expanduser().resolve() for key in required}
    checked = validate(paths['game'], paths['profile'])
    for key in ('emulator', 'preset'):
        if not paths[key].is_file():
            raise ValueError(f'Missing {key}: {paths[key]}')
    if not paths['runtime'].is_dir():
        raise ValueError('Runtime directory must exist; migrate saves before launch')
    preset_bytes = paths['preset'].read_bytes()
    preset = json.loads(preset_bytes)
    if not isinstance(preset, dict) or not all(
        isinstance(k, str) and isinstance(v, str) and k.upper().startswith(('KYTY_', 'TRACY_'))
        for k, v in preset.items()
    ):
        raise ValueError('Invalid U59 preset')
    stamp = datetime.datetime.now().strftime('%Y%m%d-%H%M%S-%f')
    env = {k: v for k, v in os.environ.items() if not k.upper().startswith(('KYTY_', 'TRACY_'))}
    env.update(preset)
    env.update(KYTY_AMD_CPU='1', KYTY_DEFER_PROTECTS='0', KYTY_HANG_WATCHDOG='1',
               KYTY_HANG_TRACE_DIR=str(paths['runtime']/'freeze-diagnostics'/stamp),
               KYTY_FAULT_COST_LOG='5', KYTY_PIPELINE_STATS='1',
               __NV_PRIME_RENDER_OFFLOAD='1', __VK_LAYER_NV_optimus='NVIDIA_only')
    command = [str(paths['emulator']), '--game', str(paths['game']),
               '--game-patch', str(paths['profile']), '--amd-cpu',
               '--screen-width', '1920', '--screen-height', '1080',
               '--vblank-frequency', '60', '--user-id', '1000', '--user-name', 'Kyty',
               '--gpu-occlusion', 'off']
    record = {'started_at': datetime.datetime.now().astimezone().isoformat(),
              'command': command, 'runtime': str(paths['runtime']), 'patch_validation': checked,
              'preset_sha256': hashlib.sha256(preset_bytes).hexdigest(),
              'effective_settings': {k: v for k, v in env.items()
                  if k.upper().startswith(('KYTY_', 'TRACY_', '__NV_PRIME', '__VK_LAYER'))}}
    if dry_run:
        print(json.dumps(record, indent=2))
        return 0
    # Locks serialize this launcher; the process check also catches GUI/release launches.
    lock_path = paths['runtime'].parent / '.astro-run.lock'
    with lock_path.open('a') as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise RuntimeError('An Astro launch is already active') from None
        running = subprocess.run(['pgrep', '-x', 'kyty_emulator'], capture_output=True, text=True)
        if running.returncode == 0:
            raise RuntimeError(f'An emulator is already running: {running.stdout.strip()}')
        if running.returncode != 1:
            raise RuntimeError('Could not verify emulator process count')
        history = paths['runtime']/'launch-history'
        history.mkdir(exist_ok=True)
        log_path = paths['runtime']/'Astro-Bot-local-last-run.log'
        manifest = paths['runtime']/'last-launch.json'
        for old in (log_path, manifest):
            if old.exists():
                old.rename(history/f'{stamp}-{old.name}')
        manifest.write_text(json.dumps(record, indent=2)+'\n')
        print('Astro Bot v1.018 — 1080p, 60 FPS cap, timing/physics profile', flush=True)
        with log_path.open('w') as log:
            stdbuf = shutil.which('stdbuf')
            run_command = [stdbuf, '-oL', '-eL', *command] if stdbuf else command
            child = subprocess.Popen(run_command, cwd=paths['runtime'], env=env,
                                     stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                     text=True, bufsize=1)
            try:
                for line in child.stdout:
                    log.write(line)
                    log.flush()
                    try:
                        print(line, end='', flush=True)
                    except BrokenPipeError:
                        pass
                code = child.wait()
            finally:
                if child.poll() is None:
                    child.terminate()
                    try:
                        child.wait(timeout=10)
                    except subprocess.TimeoutExpired:
                        child.kill()
                        child.wait(timeout=5)
                child.stdout.close()
        record.update(exit_code=code, ended_at=datetime.datetime.now().astimezone().isoformat())
        manifest.write_text(json.dumps(record, indent=2)+'\n')
        return code if code >= 0 else 128-code


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--config', type=Path, default=Path.home()/'.config/kytyps5-local/astro.json')
    parser.add_argument('--dry-run', action='store_true')
    args = parser.parse_args()
    try:
        return launch(json.loads(args.config.read_text()), args.dry_run)
    except (ValueError, KeyError, AssertionError, RuntimeError, OSError) as exc:
        print(f'Launch refused: {exc}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
