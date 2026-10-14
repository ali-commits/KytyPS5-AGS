#!/usr/bin/env python3
"""Install a staged U59 build as this user's default, retaining the system package."""
import argparse
import configparser
import datetime
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess

from validate_profile import validate, PATCH

TOOLS = Path(__file__).resolve().parent


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--stage', type=Path, required=True)
    p.add_argument('--prefix', type=Path, required=True)
    p.add_argument('--game', type=Path, required=True)
    p.add_argument('--runtime', type=Path, required=True)
    args = p.parse_args()
    stage, prefix, game, runtime = [x.expanduser().resolve() for x in
                                    (args.stage, args.prefix, args.game, args.runtime)]
    validate(game)
    for name in ('launcher', 'kyty_emulator', 'u59-preset.json'):
        if not (stage/name).is_file():
            raise SystemExit(f'Staged build missing {name}')
    if not (runtime/'_SaveData').is_dir():
        raise SystemExit('Specify the tested runtime containing the current saves')
    running = subprocess.run(['pgrep', '-x', 'kyty_emulator'], capture_output=True)
    if running.returncode != 1:
        raise SystemExit('Close the emulator before installing')
    if prefix.exists():
        raise SystemExit('Use a fresh versioned prefix; an existing install is never overwritten')
    home = Path.home()
    state = home/'.config/kytyps5-local'
    backup = state/'backups'/datetime.datetime.now().strftime('%Y%m%d-%H%M%S-%f')
    backup.mkdir(parents=True)
    for path in [home/'.local/bin/kytyps5', home/'.local/bin/astro-bot',
                 home/'.local/share/applications/kytyps5.desktop',
                 home/'.local/share/applications/astro-bot-kytyps5-local.desktop',
                 state/'astro.json', state/'Kyty.ini']:
        if path.exists() or path.is_symlink():
            key = str(path.relative_to(home)).replace('/', '__')
            shutil.copy2(path, backup/key, follow_symlinks=False)
    shutil.copytree(stage, prefix, symlinks=True)
    (prefix/'libexec').mkdir()
    (prefix/'kyty_emulator').rename(prefix/'libexec/kyty_emulator')
    shutil.copy2(TOOLS/'emulator_guard.py', prefix/'kyty_emulator')
    (prefix/'kyty_emulator').chmod(0o755)
    for name in ('lib', 'assets', '3rdparty'):
        if (prefix/name).exists():
            (prefix/'libexec'/name).symlink_to(Path('..')/name, target_is_directory=True)
    installed_tools = prefix/'astro-tools'
    installed_tools.mkdir()
    for name in ('launch.py', 'validate_profile.py', 'profile.json'):
        shutil.copy2(TOOLS/name, installed_tools/name)
    (prefix/'_Patches').mkdir(exist_ok=True)
    shutil.copy2(PATCH, prefix/'_Patches/PPSA21567.json')
    # Reuse current save/cache directories without copying them back and forth.
    for name in ('_SaveData', '_PipelineCache', '_Trophies', '_DownloadData', '_TempData', '_Textures'):
        (runtime/name).mkdir(exist_ok=True)
        (prefix/name).symlink_to(runtime/name, target_is_directory=True)
    preset = json.loads((prefix/'u59-preset.json').read_text())
    preset.update(KYTY_AMD_CPU='1', KYTY_DEFER_PROTECTS='0', KYTY_HANG_WATCHDOG='1',
                  KYTY_FAULT_COST_LOG='5', KYTY_PIPELINE_STATS='1')
    (prefix/'u59-preset.json').write_text(json.dumps(preset, indent=2)+'\n')
    # A local Kyty.ini keeps old-package preferences available for rollback.
    settings = configparser.ConfigParser(interpolation=None, strict=False)
    settings.optionxform = str
    old_settings = state/'Kyty.ini' if (state/'Kyty.ini').exists() else home/'.config/Kyty/Kyty.ini'
    settings.read(old_settings)
    for section in ('GlobalConfiguration', 'MainDialog', 'Launcher'):
        if not settings.has_section(section):
            settings.add_section(section)
    settings['GlobalConfiguration'].update(
        screen_resolution='R1920X1080', amd_cpu_mode='on', gpu_index='-1',
        vblank_frequency='60', user_id='1000', user_name='Kyty',
        gpu_occlusion_accurate='false', shader_validation_enabled='false',
        vulkan_validation_enabled='false', shader_optimization_type='Performance',
        present_mode='Mailbox', printf_direction='File', printf_output_file='_kyty.txt')
    settings['GlobalConfiguration'].pop('amd_cpu_enabled', None)
    settings['MainDialog']['check_updates_on_startup'] = 'false'
    settings['Launcher']['game_dirs'] = str(game)
    with (state/'Kyty.ini').open('w') as f:
        settings.write(f, space_around_delimiters=False)
    (prefix/'Kyty.ini').symlink_to(state/'Kyty.ini')
    config = {'game': str(game), 'emulator': str(prefix/'kyty_emulator'),
              'runtime': str(runtime), 'preset': str(prefix/'u59-preset.json'),
              'profile': str(prefix/'_Patches/PPSA21567.json')}
    (state/'astro.json').write_text(json.dumps(config, indent=2)+'\n')
    bindir = home/'.local/bin'
    apps = home/'.local/share/applications'
    bindir.mkdir(parents=True, exist_ok=True)
    apps.mkdir(parents=True, exist_ok=True)
    wrappers = {
        'kytyps5': '#!/bin/sh\nset -eu\nexport __NV_PRIME_RENDER_OFFLOAD=1\n'
                    'export __VK_LAYER_NV_optimus=NVIDIA_only\n'
                    f'cd {shlex.quote(str(prefix))}\nexec ./launcher "$@"\n',
        'astro-bot': '#!/bin/sh\nset -eu\nexec python3 '+
                     shlex.quote(str(installed_tools/'launch.py'))+' "$@"\n'}
    for name, text in wrappers.items():
        dest = bindir/name
        if dest.is_symlink():
            dest.unlink()
        dest.write_text(text)
        dest.chmod(0o755)
    def desktop(name, executable, terminal, comment):
        # Desktop Entry Exec quoting, not shell quoting.
        quoted = str(executable).replace('\\', '\\\\').replace('"', '\\"')
        return ('[Desktop Entry]\nType=Application\nName='+name+'\nComment='+comment+
                '\nExec="'+quoted+'"\nTerminal='+terminal+
                '\nIcon=applications-games\nCategories=Game;Emulator;\nStartupWMClass=launcher\n')
    (apps/'kytyps5.desktop').write_text(desktop('KytyPS5', bindir/'kytyps5', 'false',
        'Local Jetsku U59 build with the tested Astro Bot profile'))
    (apps/'astro-bot-kytyps5-local.desktop').write_text(desktop('Astro Bot (1080p / 60 FPS)',
        bindir/'astro-bot', 'true', 'Verified v1.018 timing and physics profile'))
    result = {'prefix': str(prefix), 'runtime': str(runtime), 'backup': str(backup),
              'system_fallback': '/usr/bin/kytyps5', 'config': str(state/'astro.json'),
              'native_binary': str(prefix/'libexec/kyty_emulator'),
              'binary_sha256': hashlib.sha256((prefix/'libexec/kyty_emulator').read_bytes()).hexdigest()}
    (state/'installation.json').write_text(json.dumps(result, indent=2)+'\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
