import contextlib
import copy
import hashlib
import io
import json
import os
import subprocess
import sys
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import launch
import validate_profile as v


class ProfileTests(unittest.TestCase):
    def setUp(self):
        self.profile = json.loads(v.PATCH.read_text())

    def test_pinned_profile_and_selected_modes(self):
        self.assertEqual(hashlib.sha256(v.PATCH.read_bytes()).hexdigest(), v.PATCH_HASH)
        enabled = {i for i,m in enumerate(self.profile['mods']) if m['enabled']}
        self.assertEqual(enabled, v.SELECTED)
        self.assertEqual(self.profile['mods'][5]['memory'][0]['on'], '111111111111913f')
        self.assertEqual(self.profile['mods'][9]['memory'][0]['on'], 'b80000000090')
        self.assertTrue(all(not self.profile['mods'][i]['enabled'] for i in (0,1,6,7,8,10,11,12,21,22,23,24,25)))

    def test_state_is_separate_from_import_metadata(self):
        for mod in self.profile['mods']:
            for w in mod['memory']:
                a = int(w['offset'], 0)
                self.assertFalse(0xf4e5440 <= a < 0xf4e54c8)
        self.assertEqual(self.profile['mods'][2]['memory'][1]['offset'], '0x12004000')
        self.assertEqual(self.profile['mods'][5]['memory'][0]['offset'], '0x12004078')

    def test_mismatched_original_bytes_are_rejected(self):
        with self.assertRaisesRegex(ValueError, 'Original bytes mismatch'):
            v.check_writes(self.profile, lambda a,n: b'\xff'*n, 0x10215410)

    def test_unexpected_overlap_is_rejected(self):
        p = {'mods':[{'enabled':True,'memory':[
            {'offset':'0x100','off':'00','on':'90'},
            {'offset':'0x100','off':'00','on':'90'}]}]}
        with self.assertRaisesRegex(ValueError,'Unexpected overlapping edits'):
            v.check_writes(p,lambda a,n:bytes(n),0x1000)

    def test_validation_remains_enabled_with_python_optimization(self):
        result = subprocess.run([sys.executable, '-O', '-c',
            "import validate_profile as v; v.require(False, 'guard active')"],
            cwd=Path(__file__).parent, capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('guard active', result.stderr)

    def test_dry_run_cleans_inherited_flags_and_uses_requested_paths(self):
        with tempfile.TemporaryDirectory(prefix='astro test ') as d:
            root = Path(d)
            (root/'emulator').touch()
            (root/'runtime').mkdir()
            (root/'preset.json').write_text('{"KYTY_DEFER_PROTECTS":"all"}')
            c = {'game':str(root/'game'), 'emulator':str(root/'emulator'),
                 'runtime':str(root/'runtime'), 'preset':str(root/'preset.json'),
                 'profile':str(root/'profile.json')}
            output = io.StringIO()
            with patch.object(launch,'validate',return_value={'ok':True}), \
                 patch.dict(os.environ,{'KYTY_UNKNOWN':'bad','KYTY_AMD_CPU':'0'}), \
                 contextlib.redirect_stdout(output):
                self.assertEqual(launch.launch(c,dry_run=True),0)
            result = json.loads(output.getvalue())
            self.assertEqual(result['effective_settings']['KYTY_DEFER_PROTECTS'],'0')
            self.assertEqual(result['effective_settings']['KYTY_AMD_CPU'],'1')
            self.assertNotIn('KYTY_UNKNOWN',result['effective_settings'])
            self.assertEqual(result['command'][2],str(root/'game'))
            self.assertEqual(result['command'][4],str(root/'profile.json'))
            self.assertFalse((root/'runtime/last-launch.json').exists())


if __name__ == '__main__':
    unittest.main()
