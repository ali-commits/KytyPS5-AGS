#!/usr/bin/env python3
"""Exercise save/config isolation and refusal to overwrite a working install."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

SCRIPT = Path(__file__).with_name('package.py')


class PackageTests(unittest.TestCase):
    def test_isolated_copy_and_existing_prefix_refusal(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            stage, base, runtime = (root/name for name in ('stage', 'base', 'runtime'))
            for path in (stage, base, runtime):
                path.mkdir()
            for name in ('launcher', 'kyty_emulator'):
                (stage/name).write_text('#!/bin/sh\nexit 0\n')
                (stage/name).chmod(0o755)
            (stage/'bin').mkdir()
            (stage/'bin'/'controller-audio-receiver').write_bytes(b'companion')
            (base/'Kyty.ini').write_text('[Launcher]\ngame_dirs=/example/game\n')
            (base/'u59-preset.json').write_text('{"KYTY_PIPELINE_PREFETCH_FAST_FIRST":"1"}\n')
            (base/'_Patches').mkdir()
            (base/'_Patches'/'game.json').write_text('{}')
            (runtime/'_SaveData').mkdir()
            (runtime/'_SaveData'/'progress').write_bytes(b'latest normal save')
            backend = root/'host'
            backend.write_bytes(b'backend')
            link_map = root/'emulator.map'
            link_map.write_bytes(b'exact matching map')
            prefix, config = root/'experimental', root/'settings'
            args = [sys.executable, str(SCRIPT), '--stage', str(stage), '--backend', str(backend),
                    '--backend-source', str(root), '--base-install', str(base), '--runtime', str(runtime),
                    '--prefix', str(prefix), '--config', str(config), '--map', str(link_map), '--skip-running-check']
            result = subprocess.run(args, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual((prefix/'_SaveData'/'progress').read_bytes(), b'latest normal save')
            (prefix/'_SaveData'/'progress').write_bytes(b'experimental progress')
            self.assertEqual((runtime/'_SaveData'/'progress').read_bytes(), b'latest normal save')
            self.assertEqual((base/'Kyty.ini').read_text(), '[Launcher]\ngame_dirs=/example/game\n')
            self.assertEqual((config/'Kyty.ini').read_text(), (base/'Kyty.ini').read_text())
            self.assertEqual((prefix/'libexec'/'kyty_stream_host').read_bytes(), b'backend')
            self.assertTrue((prefix/'kyty_emulator').stat().st_mode & 0o100)
            for name in ('runtime_resources.py', 'hang_stacks.py'):
                self.assertTrue((prefix/'libexec'/name).is_file())
            self.assertEqual((prefix/'kyty_emulator_clang_lld.map').read_bytes(), b'exact matching map')
            provenance = json.loads((prefix/'installation-provenance.json').read_text())
            self.assertEqual((prefix/'bin'/'controller-audio-receiver').read_bytes(), b'companion')
            self.assertEqual(provenance['companion_sha256'], 'c989edf18246e9c8cdd647369a4835f7dd9a0f47b8e755d37b16431fc5035981')
            self.assertEqual(provenance['runtime_policy'], 'independent copy; no writeback')
            again = subprocess.run(args, capture_output=True, text=True)
            self.assertNotEqual(again.returncode, 0)
            self.assertIn('already exists', again.stderr)
            self.assertEqual((prefix/'_SaveData'/'progress').read_bytes(), b'experimental progress')

    def make_inputs(self, root):
        stage, base, runtime = (root/name for name in ('stage', 'base', 'runtime'))
        for path in (stage, base, runtime):
            path.mkdir()
        for name in ('launcher', 'kyty_emulator'):
            (stage/name).write_text('#!/bin/sh\nexit 0\n')
            (stage/name).chmod(0o755)
        (base/'Kyty.ini').write_text('[Launcher]\n')
        (base/'u59-preset.json').write_text('{}\n')
        (runtime/'_SaveData').mkdir()
        (root/'host').write_bytes(b'backend')
        (root/'emulator.map').write_bytes(b'map')
        return ['--stage', str(stage), '--backend', str(root/'host'), '--backend-source', str(root),
                '--base-install', str(base), '--runtime', str(runtime), '--map', str(root/'emulator.map'),
                '--skip-running-check']

    def run_package(self, inputs, prefix, config, command, systemd, *extra):
        return subprocess.run([sys.executable, str(SCRIPT), *inputs, '--prefix', str(prefix), '--config', str(config),
                               '--command', str(command), '--systemd-dir', str(systemd), *extra],
                              capture_output=True, text=True)

    def test_user_service_and_udev_files(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            inputs = self.make_inputs(root)
            prefix, config, systemd = root/'prefix', root/'config', root/'systemd'
            command = root/'bin'/'kytyps5-streaming'
            result = self.run_package(inputs, prefix, config, command, systemd)
            self.assertEqual(result.returncode, 0, result.stderr)
            unit = (systemd/'kyty-host.service').read_text()
            self.assertIn(f'ExecStart={command} --host\n', unit)
            self.assertIn('Restart=on-failure\n', unit)
            self.assertIn('WantedBy=graphical-session.target\n', unit)
            self.assertNotIn('@COMMAND@', unit)
            share = prefix/'share'/'kyty'
            self.assertIn('KERNEL=="uhid"', (share/'71-kyty-uhid.rules').read_text())
            self.assertEqual((share/'uhid.conf').read_text(), 'uhid\n')
            for line in (f'sudo install -m 644 {share}/71-kyty-uhid.rules /etc/udev/rules.d/',
                         f'sudo install -m 644 {share}/uhid.conf /etc/modules-load.d/',
                         'sudo modprobe uhid && sudo udevadm control --reload-rules && sudo udevadm trigger'):
                self.assertIn(line, result.stdout)

    def test_exec_start_is_quoted_for_systemd(self):
        for name, expected in (('a b', '"{}" --host'), ('p%d$x', '"{}" --host'), ('q"u\\o t', '"{}" --host')):
            with tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                inputs = self.make_inputs(root)
                command = root/name/'kytyps5-streaming'
                result = self.run_package(inputs, root/'prefix', root/'config', command, root/'systemd')
                self.assertEqual(result.returncode, 0, result.stderr)
                rendered = str(command).replace('%', '%%').replace('$', '$$')
                if '"' in name:
                    rendered = rendered.replace('\\', '\\\\').replace('"', '\\"')
                self.assertIn('ExecStart=' + expected.format(rendered) + '\n', (root/'systemd'/'kyty-host.service').read_text())
                command.unlink()
                again = self.run_package(inputs, root/'prefix2', root/'config2', command, root/'systemd')
                self.assertEqual(again.returncode, 0, again.stderr)

    def test_foreign_unit_refused_unless_replaced(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            inputs = self.make_inputs(root)
            systemd = root/'systemd'
            systemd.mkdir()
            old = '[Service]\nExecStart=/old/prefix/bin/kytyps5-streaming --host\n'
            (systemd/'kyty-host.service').write_text(old)
            prefix, config = root/'prefix', root/'config'
            command = root/'bin'/'kytyps5-streaming'
            refused = self.run_package(inputs, prefix, config, command, systemd)
            self.assertNotEqual(refused.returncode, 0)
            self.assertIn('--replace-unit', refused.stderr)
            self.assertEqual((systemd/'kyty-host.service').read_text(), old)
            self.assertFalse(prefix.exists() or config.exists() or command.exists())
            replaced = self.run_package(inputs, prefix, config, command, systemd, '--replace-unit')
            self.assertEqual(replaced.returncode, 0, replaced.stderr)
            self.assertEqual((config/'rollback'/'kyty-host.service').read_text(), old)
            self.assertIn(f'ExecStart={command} --host\n', (systemd/'kyty-host.service').read_text())

    def test_failing_systemctl_leaves_complete_prefix(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            inputs = self.make_inputs(root)
            fake = root/'fakebin'
            fake.mkdir()
            (fake/'systemctl').write_text('#!/bin/sh\nexit 1\n')
            (fake/'systemctl').chmod(0o755)
            prefix, config = root/'prefix', root/'config'
            result = subprocess.run([sys.executable, str(SCRIPT), *inputs, '--prefix', str(prefix), '--config', str(config),
                                     '--command', str(root/'bin'/'kytyps5-streaming'), '--systemd-dir', str(root/'systemd'),
                                     '--enable'], capture_output=True, text=True,
                                    env={**os.environ, 'PATH': f'{fake}:{os.environ["PATH"]}'})
            self.assertNotEqual(result.returncode, 0)
            self.assertTrue((prefix/'share'/'kyty'/'71-kyty-uhid.rules').is_file())
            self.assertIn('sudo udevadm control --reload-rules', result.stdout)


if __name__ == '__main__':
    unittest.main()
