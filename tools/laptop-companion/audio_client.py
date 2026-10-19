"""Pinned mutual-TLS KCPA v1 receiver. No SDL, HID or device I/O lives here.

The sink owns its independent pacing thread. All protocol errors reset its output;
this client's lifetime must be gated by the application's live controller relay.
"""
from collections import deque
from datetime import datetime, timezone
import errno
import hashlib
import hmac
import ipaddress
import json
import math
from pathlib import Path
import select
import socket
import ssl
import struct
import threading
import time

HEADER = struct.Struct('>4sBBBBIIQQIIQQQQ')
FIELDS = ('type', 'kind', 'channels', 'generation', 'producer_epoch', 'port',
          'rate', 'port_generation', 'sequence', 'first_frame', 'source_time_us')
MAX_PAYLOAD = 16384
# A512-frame Bluetooth packet and SRC lookahead need phase headroom.
# The independent80ms source-age bound still applies to every delivery.
MAX_QUEUE_US = 60000
MAX_AGE_US = 80000
MAX_I64 = (1 << 63) - 1
MAX_U64 = (1 << 64) - 1


class ProtocolError(ValueError):
    """Malformed, unauthorized, stale or undeliverable KCPA input."""


def now_us():
    return time.monotonic_ns() // 1000


def _require(ok, message):
    if not ok:
        raise ProtocolError(message)


def _valid_header(h, size):
    typ = h['type']
    _require(1 <= typ <= 12 and 0 <= size <= MAX_PAYLOAD, 'invalid type or payload size')
    if typ in (2, 3, 4):
        _require(h['kind'] in (1, 2) and h['channels'] == 2 and h['port'] < 32
                 and 1000 <= h['rate'] <= 192000 and h['port_generation']
                 and h['generation'] and h['producer_epoch'] and h['sequence'],
                 'invalid stream header')
        if typ == 3:
            _require(size > 0 and size % 8 == 0 and size // 8 <= (h['rate'] + 99) // 100
                     and h['source_time_us'], 'invalid PCM size or timestamp')
        else:
            _require(size == 0, 'Open/Close payload must be empty')
    else:
        _require(all(h[k] == 0 for k in ('kind', 'channels', 'port', 'rate',
                    'port_generation', 'first_frame')), 'invalid control header')
        if typ == 8:
            _require(size == 8 and h['sequence'], 'invalid rumble payload')
        elif typ in (5, 6):
            _require(size == 0, 'invalid Reset/Ready payload')
        else:
            _require(size <= 4096, 'control payload too large')


def _valid_payload(h, payload):
    if h['type'] == 3:
        _require(all(math.isfinite(x[0]) for x in struct.iter_unpack('<f', payload)),
                 'nonfinite PCM')
    elif h['type'] == 8:
        _require(struct.unpack_from('>I', payload, 4)[0] <= 1000, 'rumble duration limit')


def encode_frame(header, payload=b''):
    h = {k: header.get(k, 0) for k in FIELDS}
    _valid_header(h, len(payload))
    _valid_payload(h, payload)
    try:
        return HEADER.pack(b'KCPA', 1, h['type'], h['kind'], h['channels'], 72,
                           len(payload), *(h[k] for k in FIELDS[3:])) + payload
    except (struct.error, OverflowError) as error:
        raise ProtocolError('header integer overflow') from error


def _header(raw):
    v = HEADER.unpack_from(raw)
    _require(v[0] == b'KCPA' and v[1] == 1 and v[5] == 72, 'invalid KCPA prefix')
    h = dict(zip(FIELDS, v[2:5] + v[7:]))
    _valid_header(h, v[6])
    return h, v[6]


class Framer:
    """Copy at most one validated bounded frame, even for a huge input batch."""
    def __init__(self):
        self._buffer = bytearray()

    @property
    def buffered(self):
        return len(self._buffer)

    def feed(self, data, consume):
        view = memoryview(data)
        try:
            while view:
                wanted = 72
                if len(self._buffer) >= 72:
                    _, size = _header(self._buffer)
                    wanted += size
                count = min(len(view), wanted - len(self._buffer))
                self._buffer.extend(view[:count])
                view = view[count:]
                if len(self._buffer) < 72:
                    continue
                h, size = _header(self._buffer)
                if len(self._buffer) == 72 + size:
                    payload = bytes(self._buffer[72:])
                    self._buffer.clear()
                    _valid_payload(h, payload)
                    consume(h, payload)
        except Exception:
            self._buffer.clear()
            raise


def _json(payload):
    def pairs(items):
        result = {}
        for k, v in items:
            _require(k not in result, 'duplicate JSON field')
            result[k] = v
        return result
    try:
        data = json.loads(payload, object_pairs_hook=pairs,
                          parse_constant=lambda _: (_ for _ in ()).throw(ProtocolError('invalid JSON constant')))
    except (UnicodeError, ValueError) as error:
        raise ProtocolError('invalid control JSON') from error
    _require(isinstance(data, dict), 'control JSON must be an object')
    return data


class ClockMap:
    def __init__(self):
        self.offset = 0
        self.uncertainty = MAX_U64
        self._best_rtt = MAX_U64
        self._best_at = 0
        self._sampled_at = None

    def sample(self, sent, receive, server_receive, server_send):
        if not all(type(x) is int and 0 <= x <= MAX_I64
                   for x in (sent, receive, server_receive, server_send)):
            return False
        if receive < sent or server_send < server_receive:
            return False
        elapsed, processing = receive - sent, server_send - server_receive
        if elapsed < processing or elapsed > 1000000:
            return False
        rtt = elapsed - processing
        if not self.fresh(receive) or receive - self._best_at >= 5000000:
            self._best_rtt = MAX_U64
        if rtt <= self._best_rtt:
            self._best_rtt = rtt
            self.uncertainty = (rtt + 1) // 2
            total = server_receive - sent + server_send - receive
            self.offset = total // 2 if total >= 0 else -((-total) // 2)
            self._best_at = receive
        if rtt <= 40000:
            self._sampled_at = receive
        return True

    def fresh(self, now):
        return (self.uncertainty <= 20000 and self._sampled_at is not None
                and 0 <= now - self._sampled_at <= 10000000)


class Session:
    """Device-independent receiver state machine; time is explicit for fixtures."""
    def __init__(self, sink):
        self.sink = sink
        self.last_generation = 0
        self.bytes = 0
        self.reset_connection()

    def reset_connection(self):
        self.disconnect()
        self.clock = ClockMap()
        self.sequence = 0
        self.ping_sent = 0
        self.next_ping = 0
        self.last_pong = None
        self.next_status = 0

    def disconnect(self):
        self.state = 'waiting'
        self.generation = self.epoch = self.control_sequence = 0
        self.pending = None
        self.ports = {}
        self.sink.reset()

    def _control(self, typ, data=None, leased=False):
        payload = b'' if data is None else json.dumps(data, separators=(',', ':'), allow_nan=False).encode()
        frame = encode_frame(dict(type=typ, sequence=self.sequence,
            generation=self.generation if leased else 0,
            producer_epoch=self.epoch if leased else 0), payload)
        self.sequence += 1
        return frame

    def hello(self, receiver_id):
        return self._control(1, dict(protocol_version=1, receiver_id=receiver_id,
                                    controller=0, output='bt-dualsense-media'))

    def _output(self, method, *args):
        _require(getattr(self.sink, method)(*args) is True, 'output ' + method + ' failed')

    def _ready(self, now):
        if self.pending is None:
            return []
        _require(0 <= now - self.pending <= 5000000, 'readiness clock deadline')
        if not self.clock.fresh(now):
            return []
        self._output('prepare')
        self.pending = None
        self.state = 'ready'
        return [self._control(6, leased=True)]

    def receive(self, h, payload, now):
        try:
            _valid_header(h, len(payload))
            _valid_payload(h, payload)
            return self._receive(h, payload, now)
        except Exception as error:
            self.disconnect()
            if isinstance(error, ProtocolError):
                raise
            raise ProtocolError('output or protocol failure: ' + str(error)) from error

    def _receive(self, h, payload, now):
        typ = h['type']
        _require(typ in (2, 3, 4, 5, 7, 8, 10), 'message direction violation')
        if typ == 10:
            j = _json(payload)
            keys = ('client_send_us', 'server_receive_us', 'server_send_us')
            _require(all(type(j.get(k)) is int and 0 <= j[k] <= MAX_I64 for k in keys), 'invalid Pong clock fields')
            _require(self.ping_sent != 0 and j['client_send_us'] == self.ping_sent, 'unsolicited/replayed Pong')
            _require(self.clock.sample(self.ping_sent, now, j['server_receive_us'], j['server_send_us']), 'invalid clock map')
            self.last_pong = now
            self.ping_sent = 0
            if not self.clock.fresh(now):
                self.next_ping = now + 100000
            return self._ready(now)
        if typ == 7:
            j = _json(payload)
            state = j.get('state')
            _require(isinstance(state, str), 'invalid State')
            _require('reason' not in j or (isinstance(j['reason'], str) and len(j['reason'].encode()) <= 128), 'invalid state reason')
            if state in ('waiting', 'reset', 'error'):
                self.disconnect()
                return []
            if state == 'active':
                _require(self.state == 'ready' and h['generation'] == self.generation
                         and h['producer_epoch'] == self.epoch, 'active lease mismatch')
                self.state = 'active'
                return []
            _require(state == 'preparing' and h['generation'] > self.last_generation
                     and h['producer_epoch'] and self.pending is None, 'replayed or invalid preparing lease')
            self.disconnect()
            self.last_generation = self.generation = h['generation']
            self.epoch = h['producer_epoch']
            self.state = 'preparing'
            self.pending = now
            return self._ready(now)
        _require(self.state in ('ready', 'active') and h['generation'] == self.generation
                 and h['producer_epoch'] == self.epoch, 'lease mismatch')
        if typ == 5:
            self.disconnect()
            return []
        _require(self.state == 'active', 'PCM before active state')
        if typ == 8:
            _require(h['sequence'] > self.control_sequence, 'control sequence')
            self.control_sequence = h['sequence']
            self._output('rumble', *struct.unpack('>HHI', payload), now)
            return []
        port = self.ports.get(h['port'])
        if typ == 2:
            _require((port is None or (not port['open'] and h['port_generation'] > port['h']['port_generation']))
                     and h['first_frame'] == 0, 'recycled port')
            self._output('open', h)
            self.ports[h['port']] = dict(open=True, h=h.copy(), next=0, pending=deque())
            return []
        _require(port is not None and port['open'] and all(h[k] == port['h'][k]
                 for k in ('kind', 'rate', 'port_generation'))
                 and h['sequence'] > port['h']['sequence'], 'port metadata or sequence')
        if typ == 4:
            _require(h['first_frame'] == port['next'], 'close sample continuity')
            self._output('close', h['port'])
            port.update(open=False, h=h.copy(), pending=deque())
            return []
        frames = len(payload) // 8
        _require(h['first_frame'] == port['next'] and h['first_frame'] <= MAX_U64 - frames,
                 'sample continuity')
        _require(self.clock.fresh(now), 'clock expired')
        _require(now <= MAX_I64 and h['source_time_us'] <= MAX_I64, 'clock overflow')
        age = now + self.clock.offset - h['source_time_us']
        queue = self._queue(h['port'])
        duration = (frames * 1000000 + h['rate'] - 1) // h['rate']
        _require(age >= -self.clock.uncertainty and queue + duration <= MAX_QUEUE_US
                 and age + self.clock.uncertainty + queue + duration <= MAX_AGE_US,
                 'delivery deadline or queue exceeded')
        _require(sum(len(p['pending']) for p in self.ports.values()) < 4096, 'delivery record limit')
        local = h.copy()
        local['source_time_us'] = h['source_time_us'] - self.clock.offset - self.clock.uncertainty
        self._output('pcm', local, payload, now)
        port['next'] += frames
        port['pending'].append((port['next'], h['source_time_us']))
        port['h'] = h.copy()
        self.bytes += len(payload)
        return []

    def _queue(self, port=None):
        value = self.sink.queued_us(port)
        _require(type(value) is int and 0 <= value <= MAX_QUEUE_US, 'output queue unavailable or exceeded')
        return value

    def tick(self, now):
        try:
            outgoing = self._ready(now)
            if self.state in ('ready', 'active'):
                _require(self.clock.fresh(now), 'clock expired')
                for port_id, port in self.ports.items():
                    if not port['open']:
                        continue
                    queue = self._queue(port_id)
                    pending_frames = (queue * port['h']['rate'] + 999999) // 1000000
                    # A negative cursor represents startup/transport latency
                    # before source frame zero. Keep it for per-record timing.
                    consumed = port['next'] - pending_frames
                    while port['pending'] and port['pending'][0][0] <= consumed:
                        port['pending'].popleft()
                    for end_frame, source in port['pending']:
                        # Newer queued samples are behind this record. Charging
                        # the entire queue to every old record double-counts
                        # latency and rejects a healthy native Bluetooth stream.
                        ahead = max(0, end_frame - consumed)
                        remaining = min(queue, (ahead * 1000000 + port['h']['rate'] - 1)
                                        // port['h']['rate'])
                        _require(now + self.clock.offset - source + self.clock.uncertainty
                                 + remaining <= MAX_AGE_US, 'queued sample delivery deadline')
            if now >= self.next_ping and self.ping_sent == 0:
                self.ping_sent = now
                outgoing.append(self._control(9, dict(client_send_us=now)))
                self.next_ping = now + 2000000
            if self.state in ('ready', 'active') and now >= self.next_status:
                outgoing.append(self._control(12, dict(state=self.state,
                    queued_us=self._queue(), bytes=self.bytes), leased=True))
                self.next_status = now + 2000000
            return outgoing
        except Exception as error:
            self.disconnect()
            if isinstance(error, ProtocolError):
                raise
            raise ProtocolError('output failure: ' + str(error)) from error


def _der_item(data, at):
    _require(at + 2 <= len(data), 'invalid certificate DER')
    tag, size = data[at], data[at + 1]
    at += 2
    if size & 128:
        count = size & 127
        _require(1 <= count <= 4 and at + count <= len(data), 'invalid certificate DER length')
        size = int.from_bytes(data[at:at + count], 'big')
        at += count
    _require(at + size <= len(data), 'truncated certificate DER')
    return tag, data[at:at + size], at + size


def _certificate_times(der):
    # X.509 TBSCertificate: optional version, serial, signature, issuer, validity.
    tag, cert, end = _der_item(der, 0)
    _require(tag == 0x30 and end == len(der), 'invalid certificate DER')
    tag, tbs, _ = _der_item(cert, 0)
    _require(tag == 0x30, 'invalid certificate TBS')
    at = 0
    if tbs[0] == 0xa0:
        _, _, at = _der_item(tbs, at)
    for _ in range(3):
        _, _, at = _der_item(tbs, at)
    tag, validity, _ = _der_item(tbs, at)
    _require(tag == 0x30, 'invalid certificate validity')
    result, at = [], 0
    for _ in range(2):
        tag, raw, at = _der_item(validity, at)
        text = raw.decode('ascii')
        if tag == 0x17:
            _require(len(text) == 13, 'invalid certificate UTC time')
            year = int(text[:2])
            text = str(1900 + year if year >= 50 else 2000 + year) + text[2:]
        else:
            _require(tag == 0x18 and len(text) == 15, 'invalid certificate generalized time')
        result.append(datetime.strptime(text, '%Y%m%d%H%M%SZ').replace(tzinfo=timezone.utc).timestamp())
    _require(at == len(validity), 'invalid certificate validity fields')
    return result


def validate_peer(der, pin, wall_time=None):
    _require(der and hmac.compare_digest(hashlib.sha256(der).hexdigest(), pin), 'server certificate pin mismatch')
    try:
        before, after = _certificate_times(der)
        _require(before < (time.time() if wall_time is None else wall_time) < after,
                 'server certificate time mismatch')
    except (IndexError, UnicodeError, ValueError) as error:
        if isinstance(error, ProtocolError):
            raise
        raise ProtocolError('invalid server certificate time') from error


class AudioClient:
    """Start once after relay connection; close on any relay/input/feedback loss."""
    def __init__(self, config, sink):
        self.config = dict(config)
        self.sink = sink
        self.pin = self.config['server_sha256'].replace(':', '').lower()
        _require(len(self.pin) == 64 and all(c in '0123456789abcdef' for c in self.pin), 'invalid server_sha256 pin')
        self.address = ipaddress.ip_address(self.config['host'])
        self.port = int(self.config.get('audio_port', 48219))
        _require(0 < self.port <= 65535, 'invalid audio port')
        self.status = 'Audio stopped'
        self._stop = threading.Event()
        self._lock = threading.Lock()
        self._socket = None
        self._thread = None

    def start(self):
        if self._thread is not None:
            return
        _require(not self._stop.is_set(), 'closed audio client cannot restart')
        self.status = 'Audio connecting'
        self._thread = threading.Thread(target=self._run, daemon=True, name='kyty-audio-tls')
        self._thread.start()

    def close(self):
        self._stop.set()
        with self._lock:
            sock = self._socket
            if sock is not None:
                try:
                    sock.shutdown(socket.SHUT_RDWR)
                except OSError:
                    pass
        if self._thread is not None and self._thread is not threading.current_thread():
            self._thread.join(1)
            if self._thread.is_alive():
                raise RuntimeError('audio receiver failed to stop')
        self.sink.reset()

    def _wait(self, sock, writing, deadline):
        while not self._stop.is_set() and now_us() < deadline:
            read, write, error = select.select([] if writing else [sock], [sock] if writing else [], [sock], .01)
            _require(not error, 'audio socket error')
            if read or write:
                return
        raise ProtocolError('audio operation cancelled or deadline exceeded')

    def _send(self, sock, data):
        deadline, offset = now_us() + 1000000, 0
        while offset < len(data):
            _require(not self._stop.is_set() and now_us() < deadline, 'TLS write cancelled or deadline')
            try:
                count = sock.send(data[offset:])
                _require(count > 0, 'TLS peer disconnected')
                offset += count
            except ssl.SSLWantReadError:
                self._wait(sock, False, deadline)
            except ssl.SSLWantWriteError:
                self._wait(sock, True, deadline)

    def _connect(self, context):
        family = socket.AF_INET if self.address.version == 4 else socket.AF_INET6
        sock = socket.socket(family, socket.SOCK_STREAM)
        sock.setblocking(False)
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        with self._lock:
            self._socket = sock
        result = sock.connect_ex((str(self.address), self.port))
        _require(result in (0, errno.EINPROGRESS, errno.EWOULDBLOCK), 'audio connect refused')
        if result:
            self._wait(sock, True, now_us() + 3000000)
            _require(sock.getsockopt(socket.SOL_SOCKET, socket.SO_ERROR) == 0, 'audio connect failed')
        with self._lock:
            _require(not self._stop.is_set(), 'audio cancelled')
            sock = context.wrap_socket(sock, server_hostname=str(self.address), do_handshake_on_connect=False)
            self._socket = sock
        deadline = now_us() + 5000000
        while True:
            _require(not self._stop.is_set(), 'audio cancelled')
            try:
                sock.do_handshake()
                break
            except ssl.SSLWantReadError:
                self._wait(sock, False, deadline)
            except ssl.SSLWantWriteError:
                self._wait(sock, True, deadline)
        validate_peer(sock.getpeercert(binary_form=True), self.pin)
        return sock

    def _connection(self, context, session, receiver_id):
        session.reset_connection()
        sock = self._connect(context)
        self._send(sock, session.hello(receiver_id))
        framer = Framer()
        started = last_read = now_us()
        partial_since = None
        next_status = started

        def consume(h, p):
            for outgoing in session.receive(h, p, now_us()):
                self._send(sock, outgoing)

        while not self._stop.is_set():
            now = now_us()
            _require(now - (session.last_pong if session.last_pong is not None else started) <= 6000000
                     and now - last_read <= 10000000, 'server read/Pong deadline')
            _require(partial_since is None or now - partial_since <= 2000000, 'partial frame deadline')
            for outgoing in session.tick(now):
                self._send(sock, outgoing)
            if now >= next_status:
                self.status = 'Audio ' + session.state + ' (' + str(session.bytes) + ' bytes)'
                next_status = now + 2000000
            try:
                chunk = sock.recv(4096)
                _require(chunk, 'TLS peer disconnected')
                last_read = now_us()
                framer.feed(chunk, consume)
                partial_since = (partial_since if partial_since is not None else now_us()) if framer.buffered else None
            except ssl.SSLWantReadError:
                select.select([sock], [], [], .01)
            except ssl.SSLWantWriteError:
                select.select([], [sock], [], .01)

    def _run(self):
        session = Session(self.sink)
        try:
            pem = Path(self.config['cert']).read_text()
            receiver_id = hashlib.sha256(ssl.PEM_cert_to_DER_cert(pem)).hexdigest()
            context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
            context.check_hostname = False
            context.verify_mode = ssl.CERT_NONE  # Exact DER pin and validity verified before Hello.
            context.minimum_version = ssl.TLSVersion.TLSv1_2
            context.options |= ssl.OP_NO_COMPRESSION
            if hasattr(ssl, 'OP_NO_RENEGOTIATION'):
                context.options |= ssl.OP_NO_RENEGOTIATION
            context.load_cert_chain(self.config['cert'], self.config['key'])
            while not self._stop.is_set():
                try:
                    self._connection(context, session, receiver_id)
                except Exception as error:
                    if not self._stop.is_set():
                        self.status = 'Audio: ' + str(error)
                finally:
                    session.disconnect()
                    with self._lock:
                        if self._socket is not None:
                            self._socket.close()
                            self._socket = None
                self._stop.wait(1)
        except Exception as error:
            self.status = 'Audio: ' + str(error)
        finally:
            session.disconnect()
