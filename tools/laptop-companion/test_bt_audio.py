"""Offline native Bluetooth output tests. Never opens a controller."""
import array
import ctypes as C
import ctypes.util
import json
import math
from pathlib import Path
import shutil
import subprocess
import tempfile
import threading
import time
import unittest
import zlib

import bt_audio as bt


def check_crc(test, packet):
    test.assertEqual(int.from_bytes(packet[-4:], 'little'), zlib.crc32(b'\xa2' + packet[:-4]))


class Packets(unittest.TestCase):
    def test_native_layout_wrap_crc_and_real_silence(self):
        haptics = bytes(range(64))
        p = bt.media_report(16, 256, haptics)
        self.assertEqual(len(p), 206)
        self.assertEqual(p[:13], bytes.fromhex('33009107fe8080808080009240'))
        self.assertEqual(p[13:77], haptics)
        check_crc(self, p)
        p = bt.media_report(15, 255, haptics, bt.SILENCE)
        self.assertEqual(len(p), 334)
        self.assertEqual(p[:13], bytes.fromhex('35f09107fe8080808080ff93c8'))
        self.assertEqual(p[13:213], bt.SILENCE)
        self.assertEqual(p[213:279], b'\x92\x40' + haptics)
        check_crc(self, p)
        with self.assertRaises(ValueError):
            bt.media_report(0, 0, bytes(63))
        with self.assertRaises(ValueError):
            bt.media_report(0, 0, bytes(64), bytes(199))

    def test_trigger_sides_survive_native_mode_and_rumble_arbitration(self):
        left, right = bytes([0x21] + [1]*10), bytes([0x26] + [2]*10)
        p = bt.state_report(3, left, right, (65535, 32768), native=True)
        self.assertEqual(p[3:7], bytes([0xff, 0xd5, 128, 255]))
        self.assertEqual(p[13:24], right)
        self.assertEqual(p[24:35], left)
        self.assertEqual(p[7:11], bytes([0, 100, 255, 9]))
        check_crc(self, p)
        p = bt.state_report(4, left, right, native=True)
        self.assertEqual(p[3:7], bytes([0xfc, 0xd5, 0, 0]))
        p = bt.state_report(5)
        self.assertEqual(p[3:7], bytes([15, 21, 0, 0]))
        self.assertEqual(p[13], 5)
        self.assertEqual(p[24], 5)

    def test_existing_java_oracle(self):
        root = Path(__file__).resolve().parents[4] / 'clients/android/tools/tv-hid-test-app'
        if not (root / 'tests/AdvancedMediaDump.java').exists() or not shutil.which('javac'):
            self.skipTest('Independent sibling Java packet fixtures unavailable')
        fixture = json.loads((root / 'tests/fixtures/native-media-opus.json').read_text())
        with tempfile.TemporaryDirectory() as tmp:
            subprocess.run(['javac', '-d', tmp, str(root/'src/io/kyty/tvhid/test/Reports.java'),
                            str(root/'src/io/kyty/tvhid/test/AdvancedMediaReports.java'),
                            str(root/'tests/AdvancedMediaDump.java')], check=True, capture_output=True)
            rows = subprocess.check_output(['java', '-cp', tmp, 'AdvancedMediaDump'], text=True).splitlines()
        checked = 0
        for row in rows:
            v = row.split()
            if v[:1] == ['setup'] and v[2] == 'false':
                sequence, index = int(v[1]), int(v[3])
                actual = [bt.state_report(sequence), bt.setup_report(sequence+1),
                          bt.state_report(sequence+2, native=True)][index]
            elif v[:1] == ['media'] and v[1] in ('0', '1'):
                kind, index = int(v[1]), int(v[2])
                h = bytes.fromhex(fixture['native_pcm_frames_hex'][index]) if kind == 0 else bytes(64)
                opus = None if kind == 0 else bytes.fromhex(fixture['frames_hex'][index])
                actual = bt.media_report(15+index, 255+index, h, opus)
            else:
                continue
            self.assertEqual(actual, bytes.fromhex(v[-1]), v[:-1])
            checked += 1
        self.assertEqual(checked, 97)


class Codec(unittest.TestCase):
    def test_haptics_filter_preserves_signed_sides_and_rejects_alias(self):
        def amplitude(hz):
            c = bt.Haptics()
            src = [v for i in range(4800) for v in (.02*math.sin(2*math.pi*hz*i/48000), 0)]
            data = c.convert(src)
            self.assertEqual(len(data), 600)
            self.assertTrue(all(v == 0 for v in data[1::2]))
            left = [v if v < 128 else v-256 for v in data[100::2]]
            return max(map(abs, left)), left
        peak, samples = amplitude(90)
        self.assertGreaterEqual(peak, 4)
        self.assertLessEqual(peak, 6)
        self.assertLess(min(samples), 0)
        self.assertGreater(max(samples), 0)
        self.assertLessEqual(amplitude(6000)[0], 1)
        a, b = bt.Haptics(), bt.Haptics()
        samples = [.01, -.01] * 512
        self.assertEqual(a.convert(samples), b.convert(samples[:34]) + b.convert(samples[34:]))

    def test_real_opus_cbr_stereo_duration_volume_and_silence(self):
        codec = bt.Opus()
        library = C.CDLL(ctypes.util.find_library('opus'))
        library.opus_decoder_create.argtypes = [C.c_int, C.c_int, C.POINTER(C.c_int)]
        library.opus_decoder_create.restype = C.c_void_p
        library.opus_decode.argtypes = [C.c_void_p, C.c_void_p, C.c_int, C.c_void_p, C.c_int, C.c_int]
        library.opus_decoder_destroy.argtypes = [C.c_void_p]
        error = C.c_int()
        decoder = library.opus_decoder_create(48000, 2, C.byref(error))
        self.assertEqual(error.value, 0)
        output = (C.c_int16 * 960)()
        try:
            packets = [codec.encode([v for i in range(512) for v in (.02*math.sin(2*math.pi*600*(i+n*512)/48000), 0)]) for n in range(12)]
            self.assertTrue(all(len(p) == 200 for p in packets))
            self.assertTrue(all(p[0] == 0xf4 for p in packets))
            peaks = []
            for p in packets:
                self.assertEqual(library.opus_decode(decoder, C.c_char_p(p), len(p), output, 480, 0), 480)
                peaks.append(max(abs(v) for v in output))
            self.assertGreater(max(peaks), 100)
            self.assertLess(max(peaks), 400)
            for _ in range(6):
                p = codec.encode([0.0]*1024)
                self.assertEqual(library.opus_decode(decoder, C.c_char_p(p), len(p), output, 480, 0), 480)
            self.assertLessEqual(max(abs(v) for v in output), 4)
            self.assertEqual(library.opus_decode(decoder, C.c_char_p(bt.SILENCE), 200, output, 480, 0), 480)
            codec.reset()
        finally:
            codec.close()
            library.opus_decoder_destroy(decoder)

    def test_arbitrary_rate_resampler_is_continuous_across_chunks(self):
        src = [v for i in range(4410) for v in (.01*math.sin(i/10), -.01*math.sin(i/10))]
        a, b = bt.Resampler(44100), bt.Resampler(44100)
        try:
            whole = a.convert(src)
            chunks = []
            for start in range(0, len(src), 294):
                chunks.extend(b.convert(src[start:start+294]))
            self.assertEqual(len(whole), len(chunks))
            self.assertTrue(all(abs(x-y) < 1e-6 for x,y in zip(whole,chunks)))
            self.assertGreater(len(whole), 9400)
            self.assertLess(len(whole), 9601)
        finally:
            a.close()
            b.close()


class Clock:
    def __init__(self):
        self.now = 10.0

    def __call__(self):
        return self.now


class Writes:
    def __init__(self):
        self.records = []
        self.short = False

    def __call__(self, packet):
        self.records.append((threading.get_ident(), packet))
        return len(packet)-1 if self.short else len(packet)

    def packets(self, report_id=None):
        return [p for _,p in self.records if report_id is None or p[0] == report_id]


def eventually(predicate):
    end = time.monotonic() + 2
    while time.monotonic() < end:
        if predicate():
            return
        time.sleep(.001)
    raise AssertionError('Writer did not reach expected state')


def header(port=1, kind=1, rate=48000, source_us=10000000):
    return dict(type=3, kind=kind, channels=2, generation=1, producer_epoch=1,
                port=port, rate=rate, port_generation=1, sequence=1,
                first_frame=0, source_time_us=source_us)


def pcm(frames=512, left=.02, right=0):
    return __import__('struct').pack('<'+'ff'*frames, *([left, right]*frames))


def feedback(flags=12, left=None, right=None, rumble=(0,0)):
    return dict(lease_ms=200, rumble=list(rumble), triggers=dict(flags=flags,
                left=list(left or bt.OFF), right=list(right or bt.OFF)))


class Sink(unittest.TestCase):
    def setUp(self):
        self.clock, self.writes = Clock(), Writes()
        self.sink = bt.BluetoothAudio(clock=self.clock, writer=self.writes)
        self.assertTrue(self.sink.acquire())
        self.sink.controls(feedback(), self.clock()+.2)

    def tearDown(self):
        self.sink.shutdown()

    def advance(self, seconds):
        self.clock.now += seconds
        self.sink.tick(int(self.clock()*1000000))

    def test_prepare_does_not_arm_until_pcm_and_live_input_lease(self):
        self.assertTrue(self.sink.owns_output)
        self.assertTrue(self.sink.prepare())
        time.sleep(.01)
        self.assertEqual(self.writes.packets(0x36), [])
        h = header()
        self.assertTrue(self.sink.open(h))
        self.assertTrue(self.sink.pcm(h, pcm(), 10000000))
        self.advance(bt.PERIOD)
        eventually(lambda: len(self.writes.packets(0x33)) == 1)
        packets = self.writes.packets()
        ids = [p[0] for p in packets]
        setup = ids.index(0x36)
        self.assertEqual(ids[setup:setup+3], [0x36, 0x31, 0x33])
        self.assertTrue(any(self.writes.packets(0x33)[0][13:77]))
        self.assertEqual(len(set(t for t,_ in self.writes.records)), 1)
        self.assertNotEqual(self.writes.records[0][0], threading.get_ident())
        for p in packets:
            check_crc(self, p)
        tags = [p[1] >> 4 for p in packets]
        self.assertTrue(all((b-a) % 16 == 1 for a,b in zip(tags,tags[1:])))

    def test_submitted_frame_remains_in_conservative_queue_until_its_period_retires(self):
        self.sink.prepare()
        h=header()
        self.sink.open(h)
        self.sink.pcm(h,pcm(),10000000)
        self.advance(bt.PERIOD)
        eventually(lambda: bool(self.writes.packets(0x33)))
        self.assertGreaterEqual(self.sink.queued_us(1),10666)
        self.advance(bt.PERIOD)
        eventually(lambda: self.sink.queued_us(1) == 0)

    def test_low_rate_sinc_output_keeps_original_source_age(self):
        self.sink.prepare()
        h=header(rate=1000,source_us=9940000)
        self.sink.open(h)
        for i in range(4):
            h['source_time_us']=9940000+i*10000
            self.assertTrue(self.sink.pcm(h,pcm(frames=10),10000000))
        self.advance(.012)
        time.sleep(.02)
        # The first 480 output frames still begin at the first source timestamp,
        # despite being released only by the third input block's SRC lookahead.
        self.assertEqual(self.writes.packets(0x33),[])
        self.assertEqual(self.writes.packets(0x36),[])

    def test_producer_pause_rebases_low_rate_timestamp_without_poisoning_future_audio(self):
        self.sink.prepare()
        h=header(rate=3000)
        self.sink.open(h)
        self.sink.pcm(h,pcm(frames=30),10000000)
        self.advance(.10)
        self.sink.controls(feedback(),self.clock()+.2)
        for _ in range(4):
            h['source_time_us']=round(self.clock()*1000000)
            self.assertTrue(self.sink.pcm(h,pcm(frames=30),h['source_time_us']))
            self.advance(.01)
            time.sleep(.002)
        eventually(lambda: bool(self.writes.packets(0x33)))
        self.assertTrue(any(self.writes.packets(0x33)[-1][13:77]))

    def test_multiple_actuator_ports_mix_signed_channels(self):
        self.sink.prepare()
        a,b=header(),header(port=3)
        self.sink.open(a)
        self.sink.open(b)
        self.sink.pcm(a,pcm(left=.1,right=-.05),10000000)
        self.sink.pcm(b,pcm(left=.1,right=-.05),10000000)
        self.advance(bt.PERIOD)
        eventually(lambda: bool(self.writes.packets(0x33)))
        self.assertEqual(self.writes.packets(0x33)[0][73:77],bytes([51,231,51,231]))

    def test_partial_trigger_update_preserved_across_setup_and_reset(self):
        left, right = bytes([0x21] + [3]*10), bytes([0x26] + [4]*10)
        self.sink.controls(feedback(left=left,right=right), self.clock()+.2)
        self.sink.controls(feedback(flags=8,left=bt.OFF,right=bt.OFF), self.clock()+.2)
        self.sink.prepare()
        h=header()
        self.sink.open(h)
        self.sink.pcm(h,pcm(),10000000)
        self.advance(bt.PERIOD)
        eventually(lambda: bool(self.writes.packets(0x36)))
        native = [p for p in self.writes.packets(0x31) if p[4] == 0xd5][-1]
        self.assertEqual(native[13:24], right)
        self.assertEqual(native[24:35], bt.OFF)
        self.sink.reset()
        self.advance(.02)
        eventually(lambda: self.writes.packets()[-1][0] == 0x31)
        self.assertTrue(self.sink.owns_output)
        self.assertEqual(self.writes.packets()[-1][13:24], right)
        self.assertEqual(self.sink.queued_us(), 0)

    def test_speaker_is_combined_with_haptics_and_underrun_is_fresh_silence(self):
        self.sink.prepare()
        h, s = header(), header(port=2,kind=2)
        self.sink.open(h)
        self.sink.open(s)
        self.sink.pcm(h, pcm(),10000000)
        self.sink.pcm(s, pcm(left=.02,right=.01),10000000)
        self.advance(bt.PERIOD)
        eventually(lambda: len(self.writes.packets(0x35)) == 1)
        first = self.writes.packets(0x35)[0]
        self.assertTrue(any(first[215:279]))
        self.advance(bt.PERIOD)
        eventually(lambda: len(self.writes.packets(0x35)) == 2)
        second = self.writes.packets(0x35)[1]
        self.assertNotEqual(first[13:213], second[13:213])
        self.assertFalse(any(second[215:279]))

    def test_expired_input_lease_discards_pcm_and_stops_both_triggers(self):
        self.sink.prepare()
        self.sink.controls(feedback(left=bytes([0x21]+[3]*10),rumble=(65535,65535)),self.clock()+.02)
        h = header()
        self.sink.open(h)
        self.sink.pcm(h,pcm(),10000000)
        self.advance(.03)
        eventually(lambda: self.sink.queued_us() == 0 and self.writes.packets()[-1][13] == 5 and self.writes.packets()[-1][24] == 5)
        self.assertEqual(self.sink.queued_us(),0)
        self.assertEqual(self.writes.packets(0x33),[])
        p=self.writes.packets()[-1]
        self.assertEqual(p[3:7],bytes([15,21,0,0]))
        self.assertFalse(self.sink.pcm(h,pcm(),10030000))

    def test_stale_source_queue_and_nan_are_rejected(self):
        self.sink.prepare()
        h=header(source_us=9900000)
        self.sink.open(h)
        self.assertFalse(self.sink.pcm(h,pcm(),10000000))
        h['source_time_us']=10000000
        with self.assertRaises(ValueError):
            self.sink.pcm(h,pcm(left=float('nan')),10000000)
        self.assertEqual(self.sink.queued_us(),0)
        for _ in range(7):
            self.assertTrue(self.sink.pcm(h,pcm(),10000000))
        self.assertFalse(self.sink.pcm(h,pcm(),10000000))
        self.advance(.09)
        eventually(lambda: self.sink.queued_us() == 0)
        self.assertEqual(self.writes.packets(0x33),[])

    def test_slow_writer_skips_slots_without_catchup_burst(self):
        self.sink.prepare()
        h=header()
        self.sink.open(h)
        self.sink.pcm(h,pcm(),10000000)
        self.sink.pcm(h,pcm(),10000000)
        self.sink.pcm(h,pcm(),10000000)
        self.advance(bt.PERIOD)
        eventually(lambda: len(self.writes.packets(0x33)) == 1)
        self.advance(.045)
        eventually(lambda: len(self.writes.packets(0x33)) == 2)
        time.sleep(.02)
        self.assertEqual(len(self.writes.packets(0x33)),2)
        self.assertLessEqual(self.sink.queued_us(),10667)
        self.advance(bt.PERIOD)
        eventually(lambda: self.sink.queued_us() == 0)

    def test_audible_actuator_suppresses_kcpa_rumble_and_reset_clears_it(self):
        self.sink.prepare()
        self.sink.rumble(65535,32768,1000,10000000)
        h=header()
        self.sink.open(h)
        self.sink.pcm(h,pcm(),10000000)
        self.advance(bt.PERIOD)
        eventually(lambda: bool(self.writes.packets(0x33)))
        p=[p for p in self.writes.packets(0x31) if p[4] == 0xd5][-1]
        self.assertEqual(p[3:7],bytes([0xfc,0xd5,0,0]))
        self.sink.reset()
        self.advance(.02)
        eventually(lambda: self.writes.packets()[-1][0] == 0x31)
        self.assertEqual(self.writes.packets()[-1][5:7],bytes(2))
        self.sink.controls(feedback(rumble=(32768,16384)),self.clock()+.2)
        self.advance(.02)
        eventually(lambda: self.writes.packets()[-1][5:7] == bytes([64,128]))

    def test_kcpa_rumble_survives_http_zero_and_healthy_input_lease_renewals(self):
        self.sink.prepare()
        self.assertTrue(self.sink.rumble(65535,32768,1000,10000000))
        self.advance(.02)
        eventually(lambda: self.writes.packets()[-1][5:7] == bytes([128,255]))
        for _ in range(8):
            self.sink.controls(feedback(),self.clock()+.2)
            self.advance(.12)
            time.sleep(.002)
            self.assertEqual(self.writes.packets()[-1][5:7],bytes([128,255]))
        self.sink.controls(feedback(),self.clock()+.2)
        self.advance(.03)
        eventually(lambda: self.writes.packets()[-1][5:7] == bytes(2))

    def test_prepare_clears_http_motors_and_kcpa_zero_is_explicit_stop(self):
        self.sink.controls(feedback(rumble=(65535,32768)),self.clock()+.2)
        self.advance(.02)
        eventually(lambda: self.writes.packets()[-1][5:7] == bytes([128,255]))
        self.sink.prepare()
        self.advance(.02)
        eventually(lambda: self.writes.packets()[-1][5:7] == bytes(2))
        self.sink.rumble(65535,32768,1000,round(self.clock()*1000000))
        self.advance(.02)
        eventually(lambda: self.writes.packets()[-1][5:7] == bytes([128,255]))
        self.sink.rumble(0,0,0,round(self.clock()*1000000))
        self.advance(.02)
        eventually(lambda: self.writes.packets()[-1][5:7] == bytes(2))
        self.sink.controls(feedback(rumble=(65535,65535)),self.clock()+.2)
        self.advance(.02)
        time.sleep(.01)
        self.assertEqual(self.writes.packets()[-1][5:7],bytes(2))

    def test_input_stop_clears_kcpa_cache_without_reviving_on_renewal(self):
        self.sink.prepare()
        self.sink.rumble(65535,32768,1000,10000000)
        self.advance(.02)
        eventually(lambda: self.writes.packets()[-1][5:7] == bytes([128,255]))
        self.sink.controls(feedback(),0)
        eventually(lambda: self.writes.packets()[-1][5:7] == bytes(2))
        self.sink.controls(feedback(),self.clock()+.2)
        self.advance(.02)
        time.sleep(.01)
        self.assertEqual(self.writes.packets()[-1][5:7],bytes(2))

    def test_partial_write_fails_closed_and_shutdown_neutralizes(self):
        self.writes.short=True
        self.sink.controls(feedback(left=bytes([0x21]+[1]*10)),self.clock()+.2)
        self.advance(.02)
        eventually(lambda: not self.sink.owns_output)
        self.assertIn('short',self.sink.status.lower())
        self.assertFalse(self.sink.tick(10020000))
        self.assertFalse(self.sink.prepare())
        with self.assertRaises(RuntimeError):
            self.sink.queued_us()

    def test_eagain_fails_closed_without_retrying_media(self):
        self.sink.prepare()
        h=header()
        self.sink.open(h)
        self.sink.pcm(h,pcm(),10000000)
        original=self.sink._writer
        attempts=[]
        def rejected(packet):
            if packet[0] == 0x33:
                attempts.append(packet)
                raise BlockingIOError(11,'temporarily unavailable')
            return original(packet)
        self.sink._writer=rejected
        self.advance(bt.PERIOD)
        eventually(lambda: not self.sink.owns_output)
        self.assertEqual(len(attempts),1)
        self.assertEqual(self.writes.packets()[-1][3:7],bytes([15,21,0,0]))

    def test_close_cancels_last_port_without_trailing_media(self):
        self.sink.prepare()
        h=header()
        self.sink.open(h)
        self.sink.pcm(h,pcm(),10000000)
        self.sink.close(1)
        self.advance(bt.PERIOD)
        time.sleep(.01)
        self.assertEqual(self.writes.packets(0x33),[])
        self.assertEqual(self.sink.queued_us(1),0)
        self.sink.shutdown()
        p=self.writes.packets()[-1]
        self.assertEqual(p[3:7], bytes([15,21,0,0]))
        self.assertEqual(p[13:24],bt.OFF)
        self.assertEqual(p[24:35],bt.OFF)
        self.assertFalse(self.sink.owns_output)

    def test_explicit_zero_deadline_discards_pcm_and_restores_ordinary_neutral(self):
        self.sink.prepare()
        h=header()
        self.sink.open(h)
        self.sink.pcm(h,pcm(),10000000)
        self.advance(bt.PERIOD)
        eventually(lambda: bool(self.writes.packets(0x33)))
        self.sink.controls(feedback(),0)
        self.sink.tick(int(self.clock()*1000000))
        eventually(lambda: self.writes.packets()[-1][0] == 0x31 and self.writes.packets()[-1][4] == 21)
        before=len(self.writes.packets(0x33))
        self.advance(.03)
        time.sleep(.01)
        self.assertEqual(len(self.writes.packets(0x33)),before)
        self.assertEqual(self.sink.queued_us(),0)

    def test_setup_delay_cannot_extend_input_lease_and_send_old_pcm(self):
        self.sink.prepare()
        h=header()
        self.sink.open(h)
        self.sink.pcm(h,pcm(),10000000)
        original=self.sink._writer
        def slow_setup(packet):
            count=original(packet)
            if packet[0] == 0x36:
                self.clock.now += .25
            return count
        self.sink._writer=slow_setup
        self.advance(bt.PERIOD)
        eventually(lambda: bool(self.writes.packets(0x36)) and self.sink.queued_us() == 0 and self.writes.packets()[-1][0] == 0x31)
        self.assertEqual(self.writes.packets(0x33),[])
        self.assertEqual(self.writes.packets()[-1][3:7],bytes([15,21,0,0]))


class Discovery(unittest.TestCase):
    def test_exact_identity_and_serial_or_ambiguity_failure(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp)
            for index,identity,serial in [(0,'0005:0000054C:00000CE6','aa:bb'),
                                          (1,'0003:0000054C:00000CE6','aa:bb'),
                                          (2,'0005:0000054C:00000CE6','cc:dd')]:
                d=root/f'hidraw{index}'/'device'
                d.mkdir(parents=True)
                (d/'uevent').write_text(f'HID_ID={identity}\nHID_UNIQ={serial}\n')
            self.assertEqual(bt.discover(serial='AA:BB',sysfs=root), '/dev/hidraw0')
            with self.assertRaises(RuntimeError):
                bt.discover(sysfs=root)
            with self.assertRaises(RuntimeError):
                bt.discover('/dev/hidraw1',serial='aa:bb',sysfs=root)
            with self.assertRaises(RuntimeError):
                bt.discover(serial='ee:ff',sysfs=root)
            (root/'hidraw0/device/uevent').write_text('HID_ID=0005:0000054C:00000CE6\nHID_UNIQ=12:34:ab:cd:56:ef\n')
            self.assertEqual(bt.discover(serial='1234ABCD56EF',sysfs=root), '/dev/hidraw0')
            self.assertEqual(bt.discover(serial='12-34-ab-cd-56-ef',sysfs=root), '/dev/hidraw0')


if __name__ == '__main__':
    unittest.main()
