#!/usr/bin/env python3
"""Exercise save/config isolation and refusal to overwrite a working install."""
import json
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


if __name__ == '__main__':
    unittest.main()
