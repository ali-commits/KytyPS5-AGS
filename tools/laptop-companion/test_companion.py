import unittest
from unittest.mock import Mock
from controller import buttons, axes, trigger_report, Feedback
from companion import Transport

class Tests(unittest.TestCase):

    def test_buttons(self):
        self.assertEqual(buttons({0, 4, 5, 6, 11, 20}), 4096 | 32 | 1024 | 16 | 1 | 1048576)

    def test_axes(self):
        self.assertEqual(axes([-32768, 32767, 0, -32768, 0, 32767]), [-1, 1, 0, -1, 0, 1])

    def test_trigger_payload(self):
        p = trigger_report(12, list(range(11)), [2] + [0] * 10)
        self.assertEqual(len(p), 47)
        self.assertEqual(p[0], 12)
        self.assertEqual(p[10:21], bytes([2] + [0] * 10))
        self.assertEqual(p[21:32], bytes(range(11)))
        self.assertEqual(p[32:], bytes(15))

    def test_off(self):
        self.assertEqual(trigger_report(12, [5] + [0] * 10, [5] + [0] * 10)[0], 12)
        c = Mock()
        f = Feedback(c)
        f.apply({'lease_ms': 200, 'rumble': [1, 2], 'triggers': {'flags': 12, 'left': [1] * 11, 'right': [1] * 11}}, 10)
        f.expire(10.201)
        c.stop.assert_called_once()

    def test_pin_before_request(self):
        t = Transport.__new__(Transport)
        t.pin = '00' * 32
        t.connection = Mock()
        t.connection.sock.getpeercert.return_value = b'wrong'
        with self.assertRaises(ValueError):
            t.post({'action': 'connect'})
        t.connection.request.assert_not_called()

    def test_transport_failure_closes(self):
        t = Transport.__new__(Transport)
        t.pin = __import__('hashlib').sha256(b'x').hexdigest()
        t.connection = Mock()
        t.connection.sock.getpeercert.return_value = b'x'
        t.connection.request.side_effect = TimeoutError()
        with self.assertRaises(TimeoutError):
            t.post({})
        t.connection.close.assert_called_once()

class LifecycleTests(unittest.TestCase):

    def test_sensor_si_unchanged_and_clamped(self):
        from controller import Controller
        c = Controller.__new__(Controller)
        c.pad = 123
        c.s = Mock()
        c.s.SDL_GameControllerGetAttached.return_value = 1
        c.s.SDL_GameControllerGetButton.return_value = 0
        c.s.SDL_GameControllerGetAxis.return_value = 0

        def sensor(pad, kind, values, count):
            for i, v in enumerate((1.25, -2.5, 3.75)):
                values[i] = v
            return 0
        c.s.SDL_GameControllerGetSensorData.side_effect = sensor
        data = c.snapshot()
        self.assertEqual(data['accel'], [1.25, -2.5, 3.75])
        self.assertEqual(data['gyro'], [1.25, -2.5, 3.75])

    def test_stop_resets_both_triggers_and_rumble(self):
        from controller import Controller
        c = Controller.__new__(Controller)
        c.pad = 123
        c.s = Mock()
        c.stop()
        c.s.SDL_GameControllerRumble.assert_called_once_with(123, 0, 0, 0)
        payload = c.s.SDL_GameControllerSendEffect.call_args.args[1].raw[:47]
        self.assertEqual(payload[0], 12)
        self.assertEqual(payload[10:21], bytes([5] + [0] * 10))
        self.assertEqual(payload[21:32], bytes([5] + [0] * 10))

    def test_worker_expiry_and_close_while_network_stalled(self):
        import queue, time
        from unittest.mock import patch
        from companion import Worker
        c = Mock()
        c.battery = 'Full'
        c.snapshot.return_value = {'connected': True, 'buttons': 0, 'axes': [0] * 6, 'accel': [0] * 3, 'gyro': [0] * 3}
        n = Mock()
        n.results = queue.Queue()
        with patch('companion.Controller', return_value=c), patch('companion.Network', return_value=n):
            w = Worker({})
            w.commands.put('connect')
            deadline = time.monotonic() + 1
            while not n.send.called and time.monotonic() < deadline:
                time.sleep(0.005)
            n.results.put(('connect', {'session': 'test'}, None, time.monotonic()))
            while n.send.call_count < 2 and time.monotonic() < deadline:
                time.sleep(0.005)
            time.sleep(0.23)
            self.assertTrue(c.stop.called)
            self.assertEqual(n.send.call_count, 2)
            w.commands.put('close')
            w.thread.join(1)
            self.assertFalse(w.thread.is_alive())
            c.close.assert_called_once()

    def test_bad_feedback_is_rejected_before_output(self):
        c = Mock()
        f = Feedback(c)
        with self.assertRaises(ValueError):
            f.apply({'lease_ms': 201, 'rumble': [0, 0], 'triggers': {'flags': 12, 'left': [0] * 11, 'right': [0] * 11}}, 0)
        c.effects.assert_not_called()
if __name__ == '__main__':
    unittest.main()
