"""Device-free KCPA receiver tests; fixture layout matches C++ loopback test."""
import hashlib
import json
from pathlib import Path
import socket
import shutil
import ssl
import struct
import subprocess
import tempfile
import threading
import time
import unittest

import audio_client as audio


# Independent fixture from tests/ControllerAudioReceiverLoopback.py.
def wire(typ, generation=0, epoch=0, port=0, rate=0, pg=0, seq=0,
         first=0, source=0, payload=b'', stream=0):
    return struct.pack('>4sBBBBIIQQIIQQQQ', b'KCPA', 1, typ, stream,
                       2 if stream else 0, 72, len(payload), generation,
                       epoch, port, rate, pg, seq, first, source) + payload


def decoded(data):
    frames = []
    audio.Framer().feed(data, lambda h, p: frames.append((h, p)))
    return frames[0]


def state(name, generation=7, epoch=9):
    return decoded(wire(7, generation, epoch,
                        payload=json.dumps({'state': name}).encode()))


class Sink:
    def __init__(self):
        self.events = []
        self.queue = 0
        self.fail_prepare = False

    def prepare(self):
        self.events.append(('prepare',))
        return not self.fail_prepare

    def open(self, h):
        self.events.append(('open', h.copy()))
        return True

    def pcm(self, h, p, now):
        self.events.append(('pcm', h.copy(), p, now))
        return True

    def close(self, port):
        self.events.append(('close', port))
        return True

    def reset(self):
        self.events.append(('reset',))
        self.queue = 0

    def rumble(self, large, small, duration, now):
        self.events.append(('rumble', large, small, duration, now))
        return True

    def queued_us(self, port=None):
        return self.queue


class ProtocolTests(unittest.TestCase):
    def test_exact_cpp_fixture_and_every_fragment_boundary(self):
        pcm = struct.pack('<ffff', .125, -.25, .5, -.75)
        data = wire(3, 7, 9, rate=48000, pg=1, seq=2, source=123, payload=pcm, stream=1)
        for at in range(len(data) + 1):
            frames = []
            f = audio.Framer()
            f.feed(data[:at], lambda h, p: frames.append((h, p)))
            f.feed(data[at:], lambda h, p: frames.append((h, p)))
            self.assertEqual(len(frames), 1)
            self.assertEqual(audio.encode_frame(*frames[0]), data)
            self.assertEqual(frames[0][0]['kind'], 1)

    def test_header_validated_before_payload_and_buffer_is_bounded(self):
        bad = bytearray(wire(7))
        struct.pack_into('>I', bad, 12, 0xffffffff)
        f = audio.Framer()
        with self.assertRaises(audio.ProtocolError):
            f.feed(bytes(bad) + b'x' * 100000, lambda *_: None)
        self.assertLessEqual(f.buffered, 72)

    def test_codec_rejects_wrong_shapes_nan_and_oversize_pcm(self):
        bad = [wire(3, 7, 9, rate=48000, pg=1, seq=2, source=1,
                    payload=struct.pack('<ff', float('nan'), 0), stream=1),
               wire(2, 7, 9, rate=999, pg=1, seq=1, stream=1),
               wire(3, 7, 9, rate=1000, pg=1, seq=2, source=1,
                    payload=b'\0' * 88, stream=2), wire(8, seq=1, payload=struct.pack('>HHI', 1, 2, 1001))]
        for data in bad:
            with self.subTest(data=data[:72]), self.assertRaises(audio.ProtocolError):
                decoded(data)


@unittest.skipUnless(shutil.which('g++'), 'C++ compiler unavailable')
class CanonicalCppCodecTests(unittest.TestCase):
    def test_python_frames_against_actual_cpp_encode_decode(self):
        repo = Path(__file__).resolve().parents[2]
        source = r'''#include "controller_audio/protocol.h"
#include <iostream>
#include <iterator>
int main() {
    std::vector<uint8_t> raw((std::istreambuf_iterator<char>(std::cin)), {});
    auto frame = ControllerAudio::Decode(raw);
    if (!frame) return 1;
    auto encoded = ControllerAudio::Encode(*frame);
    std::cout.write(reinterpret_cast<const char*>(encoded.data()), encoded.size());
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            cpp, binary = Path(tmp) / 'fixture.cpp', Path(tmp) / 'fixture'
            cpp.write_text(source)
            subprocess.run(['g++', '-std=c++20', '-I', str(repo / 'src'), str(cpp),
                            str(repo / 'src/controller_audio/protocol.cpp'), '-o', str(binary)],
                           check=True, capture_output=True, timeout=30)
            valid = [wire(1, payload=b'{"protocol_version":1}'), wire(5, 7, 9),
                     wire(8, 7, 9, seq=1, payload=struct.pack('>HHI', 65535, 1, 1000))]
            for rate in (1000, 44100, 48000, 192000):
                for kind in (1, 2):
                    valid.extend([wire(2, 7, 9, port=31, rate=rate, pg=1, seq=1, stream=kind),
                        wire(3, 7, 9, port=31, rate=rate, pg=1, seq=2, stream=kind,
                             source=123, payload=b'\0' * (8 * ((rate + 99) // 100)))])
            invalid = [wire(3, 7, 9, rate=48000, pg=1, seq=1, stream=1, source=1,
                            payload=struct.pack('<ff', float('inf'), 0)),
                       wire(2, 7, 9, port=32, rate=48000, pg=1, seq=1, stream=2)]
            for data in valid:
                with self.subTest(size=len(data)):
                    fixture = subprocess.run([str(binary)], input=data, capture_output=True, timeout=2)
                    self.assertEqual(fixture.returncode, 0)
                    self.assertEqual(fixture.stdout, audio.encode_frame(*decoded(data)))
            for data in invalid:
                self.assertEqual(subprocess.run([str(binary)], input=data,
                    capture_output=True, timeout=2).returncode, 1)
                with self.assertRaises(audio.ProtocolError):
                    decoded(data)


class SessionTests(unittest.TestCase):
    def setUp(self):
        self.sink = Sink()
        self.s = audio.Session(self.sink)
        self.now = 1_000_000

    def receive(self, data):
        return self.s.receive(*decoded(data), self.now)

    def sync(self, offset=0):
        self.assertTrue(self.s.clock.sample(self.now - 100, self.now,
                                          self.now - 50 + offset, self.now - 50 + offset))

    def active(self):
        self.sync()
        ready = self.s.receive(*state('preparing'), self.now)
        self.assertEqual(decoded(ready[0])[0]['type'], 6)
        self.s.receive(*state('active'), self.now)
        self.receive(wire(2, 7, 9, rate=48000, pg=1, seq=1, stream=1))

    def pcm(self, seq=2, first=0, source=None, generation=7, epoch=9, pg=1):
        return wire(3, generation, epoch, rate=48000, pg=pg, seq=seq,
                    first=first, source=self.now if source is None else source,
                    payload=b'\0' * 3840, stream=1)  # 10ms

    def test_preparing_waits_for_clock_then_prepare_ready_and_active(self):
        self.assertEqual(self.s.receive(*state('preparing'), self.now), [])
        self.assertNotIn(('prepare',), self.sink.events)
        self.sync()
        outgoing = self.s.tick(self.now)
        self.assertEqual([decoded(x)[0]['type'] for x in outgoing if decoded(x)[0]['type'] == 6], [6])
        self.assertIn(('prepare',), self.sink.events)
        with self.assertRaisesRegex(audio.ProtocolError, 'active'):
            self.receive(wire(2, 7, 9, rate=48000, pg=1, seq=1, stream=1))
        self.assertEqual(self.s.state, 'waiting')

    def test_stale_generation_epoch_port_sequence_and_frame_fail_closed(self):
        for change in ({'generation': 6}, {'epoch': 8}, {'pg': 2}, {'seq': 1}, {'first': 1}):
            with self.subTest(change=change):
                self.setUp(); self.active()
                with self.assertRaises(audio.ProtocolError):
                    self.receive(self.pcm(**change))
                self.assertEqual(self.sink.events[-1], ('reset',))

    def test_replayed_generation_rejected_across_reset(self):
        self.active()
        self.receive(wire(5, 7, 9))
        with self.assertRaisesRegex(audio.ProtocolError, 'lease'):
            self.s.receive(*state('preparing'), self.now)

    def test_pcm_payload_preserved_and_source_clock_translated_conservatively(self):
        self.sync(offset=10000)
        self.s.receive(*state('preparing'), self.now)
        self.s.receive(*state('active'), self.now)
        self.receive(wire(2, 7, 9, rate=48000, pg=1, seq=1, stream=1))
        pcm = self.pcm(source=self.now + 10000)
        self.receive(pcm)
        event = self.sink.events[-1]
        self.assertEqual(event[0], 'pcm')
        self.assertEqual(event[2], b'\0' * 3840)
        self.assertEqual(event[1]['source_time_us'], self.now - 50)
        with self.assertRaises(audio.ProtocolError):
            self.receive(pcm)

    def test_queue_and_source_deadlines_clear_output(self):
        for source, queue in ((self.now - 71000, 0), (self.now + 1000, 0), (self.now, 11000)):
            with self.subTest(source=source, queue=queue):
                self.setUp(); self.active(); self.sink.queue = queue
                with self.assertRaisesRegex(audio.ProtocolError, 'deadline|queue'):
                    self.receive(self.pcm(source=source))
                self.assertEqual(self.sink.events[-1], ('reset',))

    def test_queued_pcm_expiry_is_checked_without_more_network_pcm(self):
        self.active()
        self.receive(self.pcm())
        self.sink.queue = 10000
        with self.assertRaisesRegex(audio.ProtocolError, 'deadline'):
            self.s.tick(self.now + 71000)
        self.assertEqual(self.sink.events[-1], ('reset',))

    def test_close_requires_final_frame_position(self):
        self.active()
        self.receive(self.pcm())
        with self.assertRaisesRegex(audio.ProtocolError, 'continuity'):
            self.receive(wire(4, 7, 9, rate=48000, pg=1, seq=3, first=0, stream=1))

    def test_heartbeat_and_preparing_have_bounded_lifetime(self):
        self.s.receive(*state('preparing'), self.now)
        with self.assertRaisesRegex(audio.ProtocolError, 'readiness'):
            self.s.tick(self.now + 5000001)
        self.setUp(); self.active()
        with self.assertRaisesRegex(audio.ProtocolError, 'clock'):
            self.s.tick(self.now + 10000001)

    def test_status_is_bounded_to_two_second_interval(self):
        self.active()
        first = self.s.tick(self.now)
        self.assertEqual(sum(decoded(x)[0]['type'] == 12 for x in first), 1)
        self.assertEqual(self.s.tick(self.now + 1000), [])
        later = self.s.tick(self.now + 2000000)
        self.assertEqual(sum(decoded(x)[0]['type'] == 12 for x in later), 1)

    def test_clock_bad_samples_expiry_and_uncertainty(self):
        clock = audio.ClockMap()
        self.assertFalse(clock.sample(100, 90, 100, 100))
        self.assertFalse(clock.sample(100, 200, 100, 300))
        self.assertTrue(clock.sample(1, 50001, 1, 1))
        self.assertFalse(clock.fresh(50001))
        self.assertTrue(clock.sample(50002, 50102, 50052, 50052))
        self.assertTrue(clock.fresh(50102))
        self.assertFalse(clock.fresh(10_050_103))

    def test_wrong_direction_duplicate_pong_and_prepare_failure(self):
        for typ in (1, 6, 9, 11, 12):
            with self.subTest(typ=typ), self.assertRaises(audio.ProtocolError):
                self.receive(wire(typ))
        self.setUp()
        ping = self.s.tick(self.now)[0]
        sent = json.loads(decoded(ping)[1])['client_send_us']
        pong = wire(10, payload=json.dumps(dict(client_send_us=sent,
                    server_receive_us=self.now, server_send_us=self.now)).encode())
        self.receive(pong)
        with self.assertRaisesRegex(audio.ProtocolError, 'Pong'):
            self.receive(pong)
        self.setUp(); self.sync(); self.sink.fail_prepare = True
        with self.assertRaisesRegex(audio.ProtocolError, 'output'):
            self.s.receive(*state('preparing'), self.now)


class TLSTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.root = Path(cls.tmp.name)
        for name in ('host', 'client'):
            subprocess.run(['openssl', 'req', '-x509', '-newkey', 'ec', '-pkeyopt',
                'ec_paramgen_curve:P-256', '-nodes', '-keyout', str(cls.root / (name + '.key')),
                '-out', str(cls.root / (name + '.pem')), '-days', '1', '-subj', '/CN=' + name],
                check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        cls.pin = hashlib.sha256(ssl.PEM_cert_to_DER_cert((cls.root / 'host.pem').read_text())).hexdigest()
        cls.fingerprint = hashlib.sha256(ssl.PEM_cert_to_DER_cert((cls.root / 'client.pem').read_text())).hexdigest()

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def server(self, callback, tls=True):
        listener = socket.socket()
        listener.bind(('127.0.0.1', 0)); listener.listen(); listener.settimeout(3)
        self.failures = []
        def serve():
            try:
                sock, _ = listener.accept()
                sock.settimeout(4)
                if tls:
                    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
                    ctx.load_cert_chain(self.root / 'host.pem', self.root / 'host.key')
                    ctx.load_verify_locations(self.root / 'client.pem')
                    ctx.verify_mode = ssl.CERT_REQUIRED
                    sock = ctx.wrap_socket(sock, server_side=True)
                with sock:
                    callback(sock)
            except Exception as e:
                self.failures.append(e)
            finally:
                listener.close()
        thread = threading.Thread(target=serve, daemon=True); thread.start()
        config = dict(host='127.0.0.1', audio_port=listener.getsockname()[1],
                      cert=str(self.root / 'client.pem'), key=str(self.root / 'client.key'), server_sha256=self.pin)
        return config, thread

    @staticmethod
    def take(sock):
        def exact(n):
            data = b''
            while len(data) < n:
                chunk = sock.recv(n - len(data))
                if not chunk:
                    raise EOFError()
                data += chunk
            return data
        header = exact(72)
        return decoded(header + exact(struct.unpack_from('>I', header, 12)[0]))

    def test_real_mutual_tls_hello_clock_ready_fragmented_pcm_and_reset(self):
        received = threading.Event()
        pcm = struct.pack('<ffff', .125, -.25, .5, -.75)
        def serve(sock):
            h, p = self.take(sock)
            self.assertEqual(h['type'], 1)
            self.assertEqual(json.loads(p), dict(protocol_version=1, receiver_id=self.fingerprint,
                                               controller=0, output='bt-dualsense-media'))
            sock.sendall(wire(7, 7, 9, payload=b'{"state":"preparing"}'))
            while True:
                h, p = self.take(sock)
                if h['type'] == 9:
                    sent = json.loads(p)['client_send_us']; now = time.monotonic_ns() // 1000
                    sock.sendall(wire(10, payload=json.dumps(dict(client_send_us=sent,
                        server_receive_us=now, server_send_us=now)).encode()))
                elif h['type'] == 6:
                    self.assertEqual((h['generation'], h['producer_epoch']), (7, 9))
                    break
            sock.sendall(wire(7, 7, 9, payload=b'{"state":"active"}'))
            sock.sendall(wire(2, 7, 9, rate=48000, pg=1, seq=1, stream=1))
            chunk = wire(3, 7, 9, rate=48000, pg=1, seq=2, stream=1,
                         source=time.monotonic_ns() // 1000, payload=pcm)
            for i in range(0, len(chunk), 3):
                sock.sendall(chunk[i:i+3])
            received.set()
            time.sleep(.1)
        config, server = self.server(serve)
        sink = Sink(); client = audio.AudioClient(config, sink)
        client.start()
        try:
            self.assertTrue(received.wait(3))
            server.join(2)
            self.assertFalse(self.failures, self.failures)
            self.assertEqual([x[2] for x in sink.events if x[0] == 'pcm'], [pcm])
        finally:
            client.close()
        self.assertEqual(sink.events[-1], ('reset',))

    def test_wrong_pin_sends_no_application_hello(self):
        observed = []
        def serve(sock):
            try:
                observed.append(sock.recv(72))
            except (ssl.SSLError, ConnectionError):
                observed.append(b'')
        config, server = self.server(serve)
        config['server_sha256'] = '0' * 64
        client = audio.AudioClient(config, Sink()); client.start()
        server.join(3); client.close()
        self.assertEqual(observed, [b''])
        self.assertIn('pin', client.status.lower())

    def test_close_cancels_handshake_promptly(self):
        accepted = threading.Event(); release = threading.Event()
        def serve(sock):
            accepted.set(); release.wait(2)
        config, server = self.server(serve, tls=False)
        client = audio.AudioClient(config, Sink()); client.start()
        self.assertTrue(accepted.wait(2))
        start = time.monotonic(); client.close()
        self.assertLess(time.monotonic() - start, .4)
        release.set(); server.join(2)

    def test_partial_frame_cannot_keep_receiver_ready(self):
        ended = threading.Event()
        def serve(sock):
            self.take(sock)  # Hello.
            sock.sendall(b'K')
            while True:
                try:
                    self.take(sock)  # Initial clock Ping, followed by connection close.
                except EOFError:
                    ended.set()
                    break
        config, server = self.server(serve)
        client = audio.AudioClient(config, Sink()); client.start()
        try:
            self.assertTrue(ended.wait(3))
            self.assertIn('partial frame deadline', client.status)
        finally:
            client.close()
        server.join(1)
        self.assertFalse(self.failures, self.failures)

    def test_peer_certificate_time_checked_before_application_data(self):
        der = ssl.PEM_cert_to_DER_cert((self.root / 'host.pem').read_text())
        audio.validate_peer(der, self.pin)
        with self.assertRaisesRegex(audio.ProtocolError, 'time'):
            audio.validate_peer(der, self.pin, wall_time=0)
        with self.assertRaisesRegex(audio.ProtocolError, 'time'):
            audio.validate_peer(der, self.pin, wall_time=10_000_000_000)


if __name__ == '__main__':
    unittest.main()
