import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import time
import unittest


MODULE = Path(__file__).resolve().parent / 'live_ab.py'
SPEC = importlib.util.spec_from_file_location('live_ab', MODULE)
live_ab = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(live_ab)

MARKER = 'kyty-live-ab-test-marker'


class LiveAbTests(unittest.TestCase):
    def test_cycles_values_while_the_run_lives_then_restores_the_default(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'live' / 'switches.txt'
            fake = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(2.2)', MARKER])
            seen, result = [], []
            worker = threading.Thread(target=lambda: result.append(live_ab.main([
                '--file', str(path), '--key', 'KYTY_TEST_SWITCH', '--values', '256,1024',
                '--slot', '0.5', '--pattern', MARKER, '--poll', '0.05', '--wait-poll', '0.05',
                '--wait', '5', '--min-run', '0'])))
            worker.start()
            while worker.is_alive():
                try:
                    text = path.read_text()
                except OSError:
                    text = None
                if text and (not seen or seen[-1] != text):
                    seen.append(text)
                time.sleep(0.02)
            worker.join()
            fake.wait()
            final = path.read_text()
            if seen[-1] != final:
                seen.append(final)
            self.assertEqual(result, [0])
            values = [text.splitlines()[1] for text in seen]
            self.assertEqual(values[:3], ['KYTY_TEST_SWITCH=256', 'KYTY_TEST_SWITCH=1024',
                                          'KYTY_TEST_SWITCH=256'])
            self.assertEqual(values[-1], 'KYTY_TEST_SWITCH=')
            seqs = [int(text.splitlines()[0].split('=')[1]) for text in seen]
            self.assertEqual(seqs, sorted(set(seqs)))

    def test_a_short_run_does_not_count_and_the_next_run_starts_from_the_first_value(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'switches.txt'
            first = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(0.4)', MARKER])
            result = []
            worker = threading.Thread(target=lambda: result.append(live_ab.main([
                '--file', str(path), '--key', 'KYTY_TEST_SWITCH', '--values', 'a,b',
                '--slot', '10', '--pattern', MARKER, '--poll', '0.05', '--wait-poll', '0.05',
                '--wait', '8', '--min-run', '1.0'])))
            worker.start()
            first.wait()
            time.sleep(0.5)
            self.assertTrue(worker.is_alive())
            self.assertEqual(path.read_text().splitlines()[1], 'KYTY_TEST_SWITCH=')
            second = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(1.4)', MARKER])
            time.sleep(0.4)
            self.assertEqual(path.read_text().splitlines()[1], 'KYTY_TEST_SWITCH=a')
            second.wait()
            worker.join(5)
            self.assertEqual(result, [0])
            self.assertEqual(path.read_text().splitlines()[1], 'KYTY_TEST_SWITCH=')

    def test_gives_up_when_no_run_starts(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'switches.txt'
            self.assertEqual(live_ab.main(['--file', str(path), '--key', 'KYTY_TEST_SWITCH',
                                           '--values', '1,2', '--pattern', MARKER + '-absent',
                                           '--wait', '0.2', '--wait-poll', '0.05', '--poll', '0.05']), 1)
            self.assertFalse(path.exists())


if __name__ == '__main__':
    unittest.main()
