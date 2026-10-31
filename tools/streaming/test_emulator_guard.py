#!/usr/bin/env python3
"""runtime-env.json reaches the native program the guard starts, after the GUI stripped KYTY_*.

A copied guard runs a fake native program that prints its environment; no game is started.
"""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
FAKE_NATIVE = '''#!/usr/bin/env python3
import os
for name in sorted(os.environ):
    if name.startswith('KYTY_'):
        print(f'ENV {name}={os.environ[name]}')
'''


class GuardRuntimeEnvTests(unittest.TestCase):
    def run_guard(self, runtime_env, diagnostics=True):
        """runtime_env: a dict written as JSON, raw text, or None for no file."""
        tmp = Path(self.enterContext(tempfile.TemporaryDirectory()))
        prefix = tmp/'runtime'
        (prefix/'libexec').mkdir(parents=True)
        shutil.copy(HERE/'emulator_guard.py', prefix/'kyty_emulator')
        native = prefix/'libexec'/'kyty_emulator'
        native.write_text(FAKE_NATIVE)
        native.chmod(0o755)
        if diagnostics:
            shutil.copy(HERE.parent/'diagnostics'/'hang_stacks.py', prefix/'libexec')
        if runtime_env is not None:
            text = runtime_env if isinstance(runtime_env, str) else json.dumps(runtime_env)
            (prefix/'runtime-env.json').write_text(text)
        env = {k: v for k, v in os.environ.items() if not k.startswith('KYTY_')}
        # What the launcher's preset leaves: the shared settings, no runtime experiments.
        env.update({'KYTY_CPU_RESERVE': 'cp', 'KYTY_DROP_ME': 'preset', 'XDG_RUNTIME_DIR': str(tmp)})
        result = subprocess.run([sys.executable, str(prefix/'kyty_emulator'), '--game', 'none'],
                                env=env, capture_output=True, text=True, timeout=60)
        if result.returncode == 1 and 'running' in result.stderr:
            self.skipTest('an emulator is running')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        capture = Path((prefix/'_CrashCapture'/'last-run.txt').read_text().strip())
        child = dict(line[4:].split('=', 1) for line in (capture/'terminal.log').read_text().splitlines()
                     if line.startswith('ENV '))
        return child, json.loads((capture/'manifest.json').read_text()), result.stdout, capture

    def test_settings_reach_the_child_and_the_manifest(self):
        child, manifest, stdout, _ = self.run_guard({
            '_why': 'Performance runtime experiment', 'KYTY_UFFD_WP': '1',
            'KYTY_FAULT_MAP': '1', 'KYTY_DROP_ME': None, 'LD_PRELOAD': '/tmp/x.so',
            'KYTY_NUMBER': 1})
        self.assertEqual(child.get('KYTY_UFFD_WP'), '1')
        self.assertEqual(child.get('KYTY_FAULT_MAP'), '1')
        self.assertEqual(child.get('KYTY_CPU_RESERVE'), 'cp')
        self.assertNotIn('KYTY_DROP_ME', child)
        self.assertNotIn('KYTY_NUMBER', child)
        self.assertIn('KYTY_HANG_TRACE_DIR', child)
        effective = manifest['effective_settings']
        self.assertEqual(effective['KYTY_UFFD_WP'], '1')
        self.assertNotIn('KYTY_DROP_ME', effective)
        report = manifest['runtime_env']
        self.assertEqual(report['set'], {'KYTY_UFFD_WP': '1', 'KYTY_FAULT_MAP': '1'})
        self.assertEqual(report['removed'], ['KYTY_DROP_ME'])
        self.assertEqual(sorted(report['ignored']), ['KYTY_NUMBER', 'LD_PRELOAD'])
        self.assertIn('Runtime settings (runtime-env.json): KYTY_UFFD_WP=1', stdout)

    def test_without_diagnostics_or_without_file(self):
        child, manifest, _, _ = self.run_guard({'KYTY_UFFD_WP': '1'}, diagnostics=False)
        self.assertEqual(child.get('KYTY_UFFD_WP'), '1')
        self.assertEqual(manifest['effective_settings']['KYTY_UFFD_WP'], '1')
        child, manifest, _, _ = self.run_guard(None)
        self.assertNotIn('KYTY_UFFD_WP', child)
        self.assertIsNone(manifest['runtime_env'])

    def test_broken_file_never_blocks_the_launch(self):
        child, manifest, _, capture = self.run_guard('{not json')
        self.assertEqual(child.get('KYTY_CPU_RESERVE'), 'cp')
        self.assertNotIn('runtime_env', manifest)
        self.assertTrue((capture/'runtime-env-capture-error.txt').is_file())


if __name__ == '__main__':
    unittest.main()
