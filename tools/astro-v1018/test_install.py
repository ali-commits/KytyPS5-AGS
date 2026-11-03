import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import contextlib
import io
import subprocess

import install_default


class InstallTests(unittest.TestCase):
    def test_install_preserves_system_fallback_and_save_directory(self):
        with tempfile.TemporaryDirectory(prefix='astro install ') as d:
            home = Path(d)
            stage, runtime, game, prefix = [home/n for n in ('stage','runtime','game','version')]
            for p in (stage, runtime/'_SaveData', game):
                p.mkdir(parents=True)
            (stage/'kyty_launcher').write_text('#!/bin/sh\nprintf \'%s\\n\' "$PWD" "$@"\n')
            (stage/'kyty_launcher').chmod(0o755)
            (stage/'launcher').write_text('#!/bin/sh\nexit 99\n')
            (stage/'launcher').chmod(0o755)
            (stage/'kyty_emulator').write_text('emulator')
            (stage/'u59-preset.json').write_text('{"KYTY_DEFER_PROTECTS":"all"}')
            (runtime/'_SaveData/progress').write_bytes(b'keep this save')
            (home/'.local/bin').mkdir(parents=True)
            previous = home/'previous-wrapper'
            previous.write_text('old wrapper')
            (home/'.local/bin/kytyps5').symlink_to(previous)
            argv = ['install_default.py','--stage',str(stage),'--prefix',str(prefix),
                    '--game',str(game),'--runtime',str(runtime)]
            with patch.object(Path,'home',return_value=home), \
                 patch.object(install_default,'validate',return_value={}), \
                 patch.object(install_default.subprocess,'run',return_value=subprocess.CompletedProcess([],1)), \
                 patch('sys.argv',argv), contextlib.redirect_stdout(io.StringIO()):
                install_default.main()
            self.assertFalse((prefix/'launcher').exists())
            launched = subprocess.run([str(home/'.local/bin/kytyps5'), '--host', 'argument with spaces'],
                                      capture_output=True, text=True)
            self.assertEqual(launched.returncode, 0, launched.stderr)
            self.assertEqual(launched.stdout.splitlines(), [str(prefix), '--host', 'argument with spaces'])
            self.assertEqual(previous.read_text(),'old wrapper')
            self.assertEqual((prefix/'_SaveData/progress').read_bytes(),b'keep this save')
            self.assertEqual((prefix/'_SaveData').resolve(),runtime/'_SaveData')
            state = home/'.config/kytyps5-local'
            report = json.loads((state/'installation.json').read_text())
            self.assertEqual(report['system_fallback'],'/usr/bin/kytyps5')
            self.assertTrue((Path(report['backup'])/'.local__bin__kytyps5').is_symlink())
            c = json.loads((state/'astro.json').read_text())
            self.assertEqual(c['runtime'],str(runtime))
            self.assertIn(str(prefix), (home/'.local/bin/kytyps5').read_text())
            self.assertIn('amd_cpu_mode=on',(state/'Kyty.ini').read_text())
            self.assertIn('shader_validation_enabled=false',(state/'Kyty.ini').read_text())
            self.assertIn('printf_direction=Silent',(state/'Kyty.ini').read_text())


if __name__ == '__main__':
    unittest.main()
