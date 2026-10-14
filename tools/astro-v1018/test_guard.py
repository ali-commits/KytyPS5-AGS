import fcntl
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


class GuardTests(unittest.TestCase):
    def test_gui_probe_works_but_game_launch_is_blocked_by_shared_lock(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            (root/'libexec').mkdir()
            native = root/'libexec/kyty_emulator'
            native.write_text('#!/bin/sh\nprintf "native probe\\n"\n')
            native.chmod(0o755)
            guard = root/'kyty_emulator'
            shutil.copy2(Path(__file__).parent/'emulator_guard.py',guard)
            env = dict(os.environ, XDG_RUNTIME_DIR=str(root/'run'))
            state = root/'run/kytyps5'
            state.mkdir(parents=True)
            with (state/'emulator.lock').open('a') as lock:
                fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
                probe = subprocess.run([sys.executable,str(guard),'--help'],env=env,
                                       capture_output=True,text=True)
                self.assertEqual(probe.returncode,0)
                self.assertIn('native probe',probe.stdout)
                blocked = subprocess.run([sys.executable,str(guard),'--game','dummy'],env=env,
                                         capture_output=True,text=True)
                self.assertEqual(blocked.returncode,1)
                self.assertIn('already running',blocked.stderr)
                self.assertEqual(blocked.stdout,'')


if __name__ == '__main__':
    unittest.main()
