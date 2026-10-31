"""Device-free contract tests for the original emulator driver's C ABI adapter."""
import struct
import unittest

try:
    import native_audio as native
except ModuleNotFoundError:
    native = None

OFF = bytes([5]) + bytes(10)


def header(port=1, kind=1, rate=48000, **changes):
    h = dict(type=2, kind=kind, channels=2, generation=1, producer_epoch=1,
             port=port, rate=rate, port_generation=1, sequence=1,
             first_frame=0, source_time_us=0)
    h.update(changes)
    return h


def block(h, frames=480, **changes):
    h = dict(h, type=3, sequence=h['sequence'] + 1, source_time_us=10000000)
    h.update(changes)
    return h, struct.pack('<ff', .375, -.125) * frames


def feedback(flags=12, left=OFF, right=OFF, rumble=(0, 0)):
    return dict(rumble=list(rumble), triggers=dict(flags=flags, left=list(left), right=list(right)))


class Driver:
    """Models C ABI lifetimes and a clock deliberately unrelated to Python's."""
    def __init__(self):
        self.now = 700000
        self.events = []
        self.streams = {}
        self.next = 1
        self.fail_queue = False
        self.controller = 42
        self.deadline = 0

    def kyty_bt_version(self): return b'original-emulator-driver-v1'
    def kyty_bt_error(self): return b'driver rejected operation'
    def kyty_bt_now_us(self): return self.now
    def kyty_bt_find_controller(self, serial):
        self.events.append(('find', serial))
        return self.controller
    def kyty_bt_controls(self, controller, left, right, flags, large, small, duration, deadline):
        self.deadline = deadline
        self.events.append(('controls', controller, bytes(left), bytes(right), flags,
                            large, small, duration, deadline))
        return 1
    def kyty_bt_renew(self, controller, deadline):
        self.deadline = deadline
        return 1
    def kyty_bt_open(self, rate, speaker, controller):
        handle = self.next
        self.next += 1
        self.streams[handle] = dict(queue=0, expired=False)
        self.events.append(('open', handle, rate, speaker, controller))
        return handle
    def kyty_bt_queue(self, handle, samples, frames):
        if self.fail_queue or self.streams[handle]['expired']:
            return 0
        self.events.append(('pcm', handle, tuple(samples[:frames * 2]), frames))
        self.streams[handle]['queue'] += (frames * 1000000 + 47999) // 48000
        return self.streams[handle]['queue']
    def kyty_bt_queued_us(self, handle):
        return 0 if self.streams[handle]['expired'] else self.streams[handle]['queue']
    def kyty_bt_close(self, handle):
        self.events.append(('close', handle))
        del self.streams[handle]
    def kyty_bt_set_speaker(self, controller, value):
        self.events.append(('speaker', controller, value))
        return 1
    def kyty_bt_shutdown(self): self.events.append(('shutdown',))
    def kyty_bt_accepted_reports(self): return 6
    def kyty_bt_failed_reports(self): return 0


class NativeAdapter(unittest.TestCase):
    def setUp(self):
        self.assertIsNotNone(native, 'native_audio adapter must exist')
        self.now = 10.0
        self.driver = Driver()
        self.sink = native.NativeAudio(serial='aa:bb', driver=self.driver, clock=lambda: self.now)
        self.assertTrue(self.sink.acquire())
        self.assertTrue(self.sink.controls(feedback(), self.now + .2))

    def tearDown(self):
        if hasattr(self, 'sink'):
            self.sink.shutdown()

    def prepared(self, **options):
        self.assertTrue(self.sink.prepare())
        h = header(**options)
        self.assertTrue(self.sink.open(h))
        return h

    def last_controls(self):
        return [e for e in self.driver.events if e[0] == 'controls'][-1]

    def test_durable_audio_generation_keeps_full_uint64_width(self):
        h = self.prepared(generation=(1 << 40) + 7)
        packet, pcm = block(h)
        self.assertTrue(self.sink.pcm(packet, pcm, 10000000))

    def test_acquire_resolves_existing_sdl_gamepad_once_and_owns_until_shutdown(self):
        self.assertTrue(self.sink.acquire())
        self.assertEqual([e for e in self.driver.events if e[0] == 'find'], [('find', b'aa:bb')])
        self.assertTrue(self.sink.owns_output)
        self.sink.reset()
        self.assertTrue(self.sink.owns_output)
        self.sink.shutdown()
        self.assertFalse(self.sink.owns_output)
        self.assertEqual(self.driver.events[-1], ('shutdown',))
        count = len(self.driver.events)
        self.sink.shutdown()
        self.assertEqual(len(self.driver.events), count)

    def test_lease_maps_remaining_duration_into_native_clock(self):
        self.assertLessEqual(abs(self.last_controls()[-1] - 900000), 1)
        self.now += .1
        self.driver.now += 9000
        self.sink.controls(feedback(), self.now + .15)
        self.assertLessEqual(abs(self.last_controls()[-1] - 859000), 1)
        for deadline in (self.now + .201, float('nan'), True):
            with self.assertRaises(ValueError):
                self.sink.controls(feedback(), deadline)

    def test_exact_signed_float_delivery_has_no_extra_gain(self):
        h = self.prepared(kind=2)
        p, payload = block(h)
        self.assertTrue(self.sink.pcm(p, payload, 10000000))
        event = [e for e in self.driver.events if e[0] == 'pcm'][-1]
        self.assertEqual(event[2], (.375, -.125) * 480)
        self.assertEqual([e for e in self.driver.events if e[0] == 'open'][-1][2:], (48000, 1, 42))

    def test_partial_trigger_updates_send_both_retained_sides(self):
        left, right = bytes([0x21] + [3] * 10), bytes([0x26] + [4] * 10)
        self.sink.controls(feedback(left=left, right=right), self.now + .2)
        self.sink.controls(feedback(flags=8), self.now + .2)
        self.assertEqual(self.last_controls()[2:5], (OFF, right, 12))
        self.prepared()
        self.sink.reset()
        self.assertEqual(self.last_controls()[2:5], (OFF, right, 12))

    def test_prepared_kcpa_motor_authority_survives_http_renewal(self):
        self.sink.controls(feedback(rumble=(500, 600)), self.now + .2)
        self.prepared()
        self.assertEqual(self.last_controls()[5:7], (0, 0))
        self.assertTrue(self.sink.rumble(65535, 32768, 1000, 10000000))
        self.now += .1
        self.driver.now += 100000
        self.sink.controls(feedback(rumble=(1, 2)), self.now + .2)
        self.assertEqual(self.last_controls()[5:7], (65535, 32768))
        self.assertLessEqual(self.last_controls()[7], 900)
        self.sink.rumble(0, 0, 0, 10100000)
        self.sink.controls(feedback(rumble=(500, 600)), self.now + .2)
        self.assertEqual(self.last_controls()[5:7], (0, 0))

    def test_input_expiry_closes_handles_and_neutralizes_cached_effects(self):
        h = self.prepared()
        self.sink.rumble(65535, 2, 1000, 10000000)
        self.now += .201
        self.driver.streams[1]['expired'] = True  # Native guard acts without Python.
        self.sink.tick(10201000)
        self.assertEqual(self.driver.streams, {})
        self.assertEqual(self.last_controls()[2:7], (OFF, OFF, 12, 0, 0))
        self.assertEqual(self.last_controls()[-1], 0)
        self.assertTrue(self.sink.owns_output)
        self.sink.controls(feedback(), self.now + .2)
        self.assertEqual(self.last_controls()[5:7], (0, 0))
        self.assertFalse(self.sink.pcm(*block(h), 10201000))

    def test_zero_deadline_stops_everything_immediately(self):
        self.prepared(kind=2)
        self.sink.controls(feedback(left=bytes([1] * 11), rumble=(600, 700)), 0)
        self.assertEqual(self.driver.streams, {})
        self.assertEqual(self.last_controls()[2:7], (OFF, OFF, 12, 0, 0))
        self.assertEqual(self.last_controls()[-1], 0)

    def test_reset_closes_pcm_and_preserves_live_triggers_and_ownership(self):
        effect = bytes([0x21] + [7] * 10)
        self.sink.controls(feedback(left=effect), self.now + .2)
        self.prepared()
        self.sink.rumble(300, 400, 500, 10000000)
        self.assertTrue(self.sink.reset())
        self.assertEqual(self.driver.streams, {})
        self.assertTrue(self.sink.owns_output)
        self.assertEqual(self.last_controls()[2], effect)
        self.assertEqual(self.last_controls()[5:7], (0, 0))
        self.sink.controls(feedback(flags=0, rumble=(100, 200)), self.now + .2)
        self.assertEqual(self.last_controls()[5:7], (100, 200))

    def test_queue_is_per_port_and_retains_native_transport_headroom(self):
        a = self.prepared()
        b = header(port=2, kind=2)
        self.sink.open(b)
        self.assertTrue(self.sink.pcm(*block(a), 10000000))
        self.assertEqual(self.sink.queued_us(2), 0)
        self.assertGreaterEqual(self.sink.queued_us(1), 10000)
        self.assertEqual(self.sink.queued_us(), self.sink.queued_us(1))
        self.driver.streams[1]['queue'] = 55000
        self.assertFalse(self.sink.pcm(*block(a, first_frame=480, sequence=3), 10000000))

    def test_final_native_report_stays_in_queue_after_core_consumes_source(self):
        h = self.prepared()
        self.assertTrue(self.sink.pcm(*block(h), 10000000))
        self.driver.streams[1]['queue'] = 0
        self.assertGreaterEqual(self.sink.queued_us(1), 31333)
        self.now += .032
        self.assertEqual(self.sink.queued_us(1), 0)

    def test_shutdown_failure_retains_ownership_and_can_be_retried(self):
        original = self.driver.kyty_bt_shutdown
        def fail():
            raise RuntimeError('native shutdown failed')
        self.driver.kyty_bt_shutdown = fail
        with self.assertRaises(RuntimeError):
            self.sink.shutdown()
        self.assertTrue(self.sink.owns_output)
        self.driver.kyty_bt_shutdown = original
        self.sink.shutdown()
        self.assertFalse(self.sink.owns_output)

    def test_actual_local_clock_rechecks_age_after_receiver_dispatch_delay(self):
        h = self.prepared()
        self.now += .06
        self.assertFalse(self.sink.pcm(*block(h), 10000000))
        self.assertFalse(any(e[0] == 'pcm' for e in self.driver.events))

    def test_source_age_budget_includes_original_driver_packet_period(self):
        h = self.prepared()
        p, payload = block(h, source_time_us=9940000)
        self.assertFalse(self.sink.pcm(p, payload, 10000000))
        self.assertFalse(any(e[0] == 'pcm' for e in self.driver.events))

    def test_native_queue_failure_withdraws_ports_and_effect_lease(self):
        h = self.prepared()
        self.driver.fail_queue = True
        self.assertFalse(self.sink.pcm(*block(h), 10000000))
        self.assertEqual(self.driver.streams, {})
        self.assertEqual(self.last_controls()[-1], 0)

    def test_port_generation_cannot_reopen_or_receive_stale_blocks(self):
        h = self.prepared()
        self.sink.close(1)
        self.assertFalse(self.sink.open(h))
        fresh = dict(h, port_generation=2)
        self.assertTrue(self.sink.open(fresh))
        self.assertFalse(self.sink.pcm(*block(h), 10000000))
        self.assertTrue(self.sink.pcm(*block(fresh), 10000000))

    def test_old_session_cannot_reopen_after_new_generation(self):
        old = self.prepared()
        self.sink.close(1)
        fresh = dict(old, generation=2)
        self.assertTrue(self.sink.open(fresh))
        self.sink.close(1)
        self.assertFalse(self.sink.open(dict(old, port_generation=2)))

    def test_native_control_failure_stops_pcm_and_sends_immediate_neutral(self):
        self.prepared()
        original = self.driver.kyty_bt_controls
        def fail_live(*args):
            return 0 if args[-1] else original(*args)
        self.driver.kyty_bt_controls = fail_live
        self.assertFalse(self.sink.controls(feedback(), self.now + .2))
        self.assertEqual(self.driver.streams, {})
        self.assertEqual(self.last_controls()[-1], 0)
        self.assertFalse(self.sink.tick(10000000))

    def test_previous_native_failure_count_does_not_poison_a_new_owner(self):
        self.sink.shutdown()
        self.driver.kyty_bt_failed_reports = lambda: 3
        self.sink = native.NativeAudio(serial='aa:bb', driver=self.driver, clock=lambda: self.now)
        self.assertTrue(self.sink.acquire())
        self.assertTrue(self.sink.tick(10000000))
        self.driver.kyty_bt_failed_reports = lambda: 4
        self.assertFalse(self.sink.tick(10000000))

    def test_canonical_headers_sizes_and_nonfinite_samples_fail_closed(self):
        h = self.prepared()
        for key, value in (('rate', True), ('channels', 1), ('kind', 3), ('port', -1),
                           ('generation', 0), ('producer_epoch', 0), ('port_generation', 0)):
            with self.subTest(key=key), self.assertRaises(ValueError):
                self.sink.open(dict(header(port=2), **{key: value}))
        p, payload = block(h)
        for bad in (b'', payload[:-1], struct.pack('<ff', float('nan'), 0),
                    struct.pack('<ff', float('inf'), 0), payload + bytes(8)):
            with self.subTest(size=len(bad)), self.assertRaises(ValueError):
                self.sink.pcm(p, bad, 10000000)
        self.assertFalse(any(e[0] == 'pcm' for e in self.driver.events))

    def test_sequence_and_frame_continuity_are_checked_before_native_queue(self):
        h = self.prepared()
        self.assertTrue(self.sink.pcm(*block(h), 10000000))
        self.assertFalse(self.sink.pcm(*block(h), 10000000))
        self.assertFalse(self.sink.pcm(*block(h, sequence=3, first_frame=481), 10000000))
        self.assertEqual(len([e for e in self.driver.events if e[0] == 'pcm']), 1)

    def test_shutdown_neutralizes_before_native_global_stop(self):
        self.prepared(kind=2)
        self.sink.shutdown()
        self.assertEqual(self.last_controls()[2:7], (OFF, OFF, 12, 0, 0))
        self.assertEqual(self.driver.events[-1], ('shutdown',))
        self.assertFalse(self.sink.owns_output)


if __name__ == '__main__':
    unittest.main()
