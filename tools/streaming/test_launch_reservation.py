"""Owned temporary guards and fake children only; never starts an emulator."""
import fcntl
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

HERE = Path(__file__).resolve().parent

class LaunchReservationTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.prefix = self.root/'install'
        (self.prefix/'libexec').mkdir(parents=True)
        self.guard = self.prefix/'kyty_emulator'
        shutil.copy(HERE/'emulator_guard.py', self.guard)
        self.native = self.prefix/'libexec'/'kyty_emulator'
        self.native.write_text('#!/usr/bin/env python3\nimport os\nfrom pathlib import Path\nPath("child-ran").write_text("yes")\n')
        self.native.chmod(0o755)
        self.runtime = self.root/'isolated'; self.runtime.mkdir()
        self.state = self.root/'run'/'kytyps5'; self.state.mkdir(parents=True, mode=0o700)
        self.env = dict(os.environ, XDG_RUNTIME_DIR=str(self.state.parent))
        self.env.pop('AGS_EMULATOR_LOCK_FD', None)
        self.env.pop('AGS_EMULATOR_LOCK_PATH', None)

    def run_guard(self, **kwargs):
        return subprocess.run([sys.executable, str(self.guard), '--game', 'fixture with spaces'],
            cwd=self.runtime, env=self.env, capture_output=True, text=True, timeout=15, **kwargs)

    def test_local_admission_prevents_child_and_diagnostics(self):
        with (self.state/'emulator.lock.launch').open('w') as admission:
            os.chmod(admission.name, 0o600)
            fcntl.flock(admission, fcntl.LOCK_EX | fcntl.LOCK_NB)
            result = self.run_guard()
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse((self.runtime/'child-ran').exists())
        self.assertFalse((self.prefix/'_CrashCapture').exists())

    def test_residual_steam_ownership_blocks_local_child_with_free_locks(self):
        marker = self.state/'emulator.lock.host-steam-owner'
        marker.write_text('{"verified_empty":false}')
        result = self.run_guard()
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse((self.runtime/'child-ran').exists())
        self.assertTrue(marker.exists())

    def test_adopts_lifetime_fd_and_keeps_diagnostics_isolated(self):
        lock_path = self.state/'emulator.lock'
        with lock_path.open('w') as lock, (self.state/'emulator.lock.launch').open('w') as admission:
            os.chmod(lock_path, 0o600); os.chmod(admission.name, 0o600)
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            fcntl.flock(admission, fcntl.LOCK_EX | fcntl.LOCK_NB)
            self.env.update(AGS_EMULATOR_LOCK_FD=str(lock.fileno()), AGS_EMULATOR_LOCK_PATH=str(lock_path), AGS_SESSION_RUNTIME=str(self.runtime))
            result = self.run_guard(pass_fds=(lock.fileno(),))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue((self.runtime/'child-ran').exists())
        self.assertTrue((self.runtime/'_CrashCapture'/'last-run.txt').exists())
        self.assertFalse((self.prefix/'_CrashCapture').exists())

    def test_fake_inherited_fd_is_rejected(self):
        lock_path = self.state/'unrelated'
        with lock_path.open('w') as lock:
            self.env.update(AGS_EMULATOR_LOCK_FD=str(lock.fileno()), AGS_EMULATOR_LOCK_PATH=str(self.state/'emulator.lock'))
            result = self.run_guard(pass_fds=(lock.fileno(),))
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse((self.runtime/'child-ran').exists())

if __name__ == '__main__': unittest.main()
