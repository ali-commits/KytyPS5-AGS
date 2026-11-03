#!/usr/bin/env python3
"""Package the native AGS launcher without replacing an existing installation."""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess

TOOLS = Path(__file__).resolve().parent
SOURCE = TOOLS.parent.parent
UNIT_NAME = 'kyty-host.service'
STATE_DIRS = ('_SaveData', '_PipelineCache', '_Trophies', '_DownloadData', '_TempData', '_Textures')


def sha256(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def revision(directory):
    result = subprocess.run(['git', '-C', str(directory), 'rev-parse', 'HEAD'], capture_output=True, text=True)
    status = subprocess.run(['git', '-C', str(directory), 'status', '--porcelain'], capture_output=True, text=True)
    return {'commit': result.stdout.strip() if result.returncode == 0 else None,
            'changes': status.stdout.splitlines() if status.returncode == 0 else []}


def systemd_word(path):
    text = str(path).replace('%', '%%').replace('$', '$$')
    if not any(char.isspace() or char in '"\\%$' for char in str(path)):
        return text
    return '"' + text.replace('\\', '\\\\').replace('"', '\\"') + '"'


def install_unit(args, command, config, unit_path, old_unit):
    if old_unit is not None:
        (config/'rollback').mkdir(mode=0o700, exist_ok=True)
        (config/'rollback'/UNIT_NAME).write_text(old_unit)
    unit_path.parent.mkdir(parents=True, exist_ok=True)
    unit_path.write_text((TOOLS/(UNIT_NAME+'.in')).read_text().replace('@COMMAND@', systemd_word(command)))
    if args.enable:
        subprocess.run(['systemctl', '--user', 'daemon-reload'], check=True)
        subprocess.run(['systemctl', '--user', 'enable', UNIT_NAME], check=True)


def package(args):
    stage, backend, base, runtime, prefix, config, link_map = [path.expanduser().resolve() for path in
        (args.stage, args.backend, args.base_install, args.runtime, args.prefix, args.config, args.map)]
    for path in (stage/'kyty_launcher', stage/'kyty_emulator', backend, backend.parent/'ags-guardian',
                 backend.parent/'ags-hid-guard', backend.parent/'ags-capture-worker', backend.parent/'ags-client',
                 base/'Kyty.ini', base/'u59-preset.json', link_map):
        if not path.is_file():
            raise SystemExit(f'Missing package input: {path}')
    if not (runtime/'_SaveData').is_dir():
        raise SystemExit('Runtime must contain current _SaveData')
    for path in (prefix, config):
        if path.exists() or path.is_symlink():
            raise SystemExit(f'Fresh destination required; {path} already exists')
        if any(path == old or old in path.parents or path in old.parents for old in (base, runtime, stage)):
            raise SystemExit('Package destinations must be separate from working source/install/runtime')
    if prefix == config or prefix in config.parents or config in prefix.parents:
        raise SystemExit('Config and installation must have separate destinations')
    if not args.skip_running_check:
        check = subprocess.run(['pgrep', '-x', 'kyty_emulator'], capture_output=True)
        if check.returncode != 1:
            raise SystemExit('Close the emulator before taking an independent save snapshot')
    if args.command:
        command = args.command.expanduser().absolute()
        if command.name != 'kytyps5-streaming':
            raise SystemExit('Only the dedicated kytyps5-streaming command may be installed')
        if command.exists() or command.is_symlink():
            raise SystemExit(f'Command already exists: {command}')
    unit_path = args.systemd_dir.expanduser().absolute()/UNIT_NAME
    if args.command:
        old_unit = unit_path.read_text() if unit_path.is_file() else None
        if old_unit is not None and f'ExecStart={systemd_word(command)} --host\n' not in old_unit and not args.replace_unit:
            raise SystemExit(f'{unit_path} points to a different installation; pass --replace-unit to back it up and replace it')
    base_hash = sha256(base/'Kyty.ini')
    prefix.parent.mkdir(parents=True, exist_ok=True)
    shutil.copytree(stage, prefix, symlinks=False)
    # Old stages may contain a generic ELF alias that external scheduler rules
    # classify as an unrelated background application. Publish only the canonical name.
    (prefix/'launcher').unlink(missing_ok=True)
    config.mkdir(mode=0o700, parents=True)
    os.chmod(config, 0o700)
    (prefix/'libexec').mkdir(exist_ok=True)
    (prefix/'kyty_emulator').rename(prefix/'libexec'/'kyty_emulator')
    shutil.copy2(TOOLS/'emulator_guard.py', prefix/'kyty_emulator')
    (prefix/'kyty_emulator').chmod(0o755)
    for name in ('runtime_resources.py', 'hang_stacks.py'):
        shutil.copy2(TOOLS.parent/'diagnostics'/name, prefix/'libexec'/name)
    shutil.copy2(backend, prefix/'libexec'/'ags-host')
    (prefix/'libexec'/'ags-host').chmod(0o755)
    for name in ('ags-guardian', 'ags-hid-guard', 'ags-capture-worker', 'ags-client'):
        shutil.copy2(backend.parent/name, prefix/'libexec'/name)
        (prefix/'libexec'/name).chmod(0o755)
    compositor = args.backend_source.expanduser().resolve()/'local/runtime/gamescope-pts'
    if (compositor/'bin/gamescope-ags').is_file():
        boundary = compositor.resolve()
        for path in compositor.rglob('*'):
            if path.is_symlink() and not path.resolve().is_relative_to(boundary):
                raise SystemExit('Bundled compositor contains an external symlink')
        if not (compositor/'UPSTREAM_LICENSE').is_file():
            raise SystemExit('Bundled compositor license is missing')
        shutil.copytree(compositor, prefix/'libexec/gamescope-pts')
    shutil.copy2(link_map, prefix/'kyty_emulator_clang_lld.map')
    for name in ('lib', 'assets', '3rdparty'):
        if (prefix/name).exists() and not (prefix/'libexec'/name).exists():
            (prefix/'libexec'/name).symlink_to(Path('..')/name, target_is_directory=True)
    settings = (base/'Kyty.ini').read_text().replace(str(base), str(prefix))
    (config/'Kyty.ini').write_text(settings)
    (config/'Kyty.ini').chmod(0o600)
    (prefix/'Kyty.ini').symlink_to(config/'Kyty.ini')
    shutil.copy2(base/'u59-preset.json', prefix/'u59-preset.json')
    if (base/'_Patches').is_dir():
        shutil.copytree(base/'_Patches', prefix/'_Patches', dirs_exist_ok=True)
    for name in STATE_DIRS:
        source = runtime/name
        if source.is_dir():
            shutil.copytree(source, prefix/name, symlinks=False)
        else:
            (prefix/name).mkdir()
    (prefix/'_CrashCapture').mkdir(exist_ok=True)
    shutil.copy2(SOURCE/'LICENSE', prefix/'LICENSE')
    shutil.copy2(SOURCE/'docs'/'streaming'/'README.md', prefix/'STREAMING-README.md')
    backend_source = args.backend_source.expanduser().resolve()
    licenses = prefix/'licenses'/'ags-native'
    licenses.mkdir(parents=True, exist_ok=True)
    for name in ('LICENSE', 'NOTICE', 'THIRD-PARTY-LICENSES'):
        if (backend_source/name).is_file():
            shutil.copy2(backend_source/name, licenses/name)
    result = {'created_at_utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
        'prefix': str(prefix), 'config': str(config), 'base_install': str(base), 'source_runtime': str(runtime),
        'runtime_policy': 'independent copy; no writeback', 'kyty': revision(SOURCE),
        'stream_host': revision(backend_source), 'native_sha256': sha256(prefix/'libexec'/'kyty_emulator'),
        'map_sha256': sha256(prefix/'kyty_emulator_clang_lld.map'),
        'launcher_sha256': sha256(prefix/'kyty_launcher'), 'host_sha256': sha256(prefix/'libexec'/'ags-host'),
        'receiver_sha256': sha256(prefix/'libexec'/'ags-client'),
        'base_config_sha256': base_hash, 'crash_capture': True}
    # Retired companion service is not a native receiver capability. A reused
    # staging directory must not accidentally publish its old executable.
    companion = prefix/'bin'/'controller-audio-receiver'
    if companion.is_file():
        companion.unlink()
    (prefix/'installation-provenance.json').write_text(json.dumps(result, indent=2)+'\n')
    if sha256(base/'Kyty.ini') != base_hash:
        raise SystemExit('Working configuration changed during package creation; inspect before testing')
    wrapper = ('#!/bin/sh\nset -eu\nexport __NV_PRIME_RENDER_OFFLOAD=1\n'
        'export __VK_LAYER_NV_optimus=NVIDIA_only\n'
        f'export KYTY_STREAMING_CONFIG_DIR={shlex.quote(str(config/"streaming"))}\n'
        f'cd {shlex.quote(str(prefix))}\nexec ./kyty_launcher "$@"\n')
    (prefix/'start-streaming.sh').write_text(wrapper)
    (prefix/'start-streaming.sh').chmod(0o755)
    share = prefix/'share'/'kyty'
    share.mkdir(parents=True, exist_ok=True)
    for name in ('71-kyty-uhid.rules', 'uhid.conf'):
        shutil.copy2(TOOLS/name, share/name)
    print(json.dumps(result, indent=2))
    print('Run once to allow the virtual DualSense (not executed by this packager):\n'
          f'sudo install -m 644 {share}/71-kyty-uhid.rules /etc/udev/rules.d/\n'
          f'sudo install -m 644 {share}/uhid.conf /etc/modules-load.d/\n'
          'sudo modprobe uhid && sudo udevadm control --reload-rules && sudo udevadm trigger', flush=True)
    # Last: a failing systemctl then leaves a complete prefix and the commands above printed.
    if args.command:
        command.parent.mkdir(parents=True, exist_ok=True)
        command.write_text(wrapper)
        command.chmod(0o755)
        install_unit(args, command, config, unit_path, old_unit)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for option in ('stage', 'backend', 'backend-source', 'base-install', 'runtime', 'prefix', 'config', 'map'):
        parser.add_argument('--'+option, type=Path, required=True)
    parser.add_argument('--command', type=Path, help='Optional dedicated kytyps5-streaming command to create')
    parser.add_argument('--systemd-dir', type=Path, default=Path('~/.config/systemd/user'),
                        help='User unit directory; kyty-host.service is written only with --command')
    parser.add_argument('--replace-unit', action='store_true',
                        help='Replace a kyty-host.service that points at another installation (old one is backed up)')
    parser.add_argument('--enable', action='store_true', help='Run systemctl --user daemon-reload and enable the unit')
    parser.add_argument('--skip-running-check', action='store_true', help='For synthetic packaging tests only')
    package(parser.parse_args())


if __name__ == '__main__':
    main()
