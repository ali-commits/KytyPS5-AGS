import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from unittest.mock import patch

MODULE = Path(__file__).with_name('hang_stacks.py')


class HangStackTests(unittest.TestCase):
    def setUp(self):
        spec = importlib.util.spec_from_file_location('hang_stacks', MODULE)
        self.capture = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.capture)

    def test_waits_for_watchdog_and_captures_once_without_stopping_game(self):
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            done = threading.Event()
            child = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(30)'])
            try:
                with patch.object(self.capture.subprocess, 'run') as debugger:
                    debugger.return_value.returncode = 0
                    thread = self.capture.start_capture(child, root, root / 'hang', done)
                    time.sleep(0.1)
                    debugger.assert_not_called()
                    (root / 'hang').mkdir()
                    (root / 'hang' / 'watchdog.txt').write_text('pid=99999999 t_ms=1000')
                    time.sleep(0.3)
                    debugger.assert_not_called()
                    (root / 'hang' / 'watchdog.txt').write_text(f'pid={child.pid} t_ms=1000')
                    thread.join(timeout=3)
                    self.assertFalse(thread.is_alive())
                    debugger.assert_called_once()
                    self.assertIn(str(child.pid), debugger.call_args.args[0])
                    self.assertIn('detach', debugger.call_args.args[0])
                    self.assertIsNone(child.poll())
            finally:
                done.set()
                child.terminate()
                child.wait(timeout=3)

    def test_stop_and_exited_game_do_not_attach(self):
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            child = subprocess.Popen([sys.executable, '-c', 'pass'])
            child.wait(timeout=3)
            done = threading.Event()
            with patch.object(self.capture.subprocess, 'run') as debugger:
                thread = self.capture.start_capture(child, root, root, done)
                thread.join(timeout=3)
                debugger.assert_not_called()
                done.set()
                thread = self.capture.start_capture(child, root, root, done)
                thread.join(timeout=3)
                debugger.assert_not_called()

    def test_preserves_explicit_trace_paths(self):
        with patch.dict('os.environ', {'KYTY_HANG_TRACE_DIR': '/chosen/hang',
                                       'KYTY_FRAME_TIMES_LOG': '/chosen/frames.csv'}, clear=True):
            env, _ = self.capture.launch_options(Path('/capture'))
            self.assertEqual(env['KYTY_HANG_TRACE_DIR'], '/chosen/hang')
            self.assertEqual(env['KYTY_FRAME_TIMES_LOG'], '/chosen/frames.csv')


if __name__ == '__main__':
    unittest.main()
