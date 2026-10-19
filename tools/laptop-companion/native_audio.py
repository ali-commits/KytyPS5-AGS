"""KCPA sink backed by the emulator's original DualSense Bluetooth driver.

SDL input must already be open in the process's shared SDL3 runtime. This module
never initializes SDL and never writes HID packets. The native library owns its
transport thread and an independent input-lease watchdog.
"""
import ctypes as C
import math
import os
from pathlib import Path
import struct
import threading
import time

ACTUATOR, SPEAKER = 1, 2
OFF = bytes([5]) + bytes(10)
MAX_AGE_US = 80000
TRANSPORT_US = 21334  # Original 0x39 packet: 1024 source frames at 48 kHz.
_IDENTITIES = ('kind', 'channels', 'rate', 'generation', 'producer_epoch', 'port_generation')


def _load(path):
    lib = C.CDLL(str(path))
    pointer = C.c_void_p
    u8p = C.POINTER(C.c_uint8)
    signatures = {
        'version': (C.c_char_p, []), 'error': (C.c_char_p, []),
        'now_us': (C.c_uint64, []), 'find_controller': (C.c_int, [C.c_char_p]),
        'open': (pointer, [C.c_uint32, C.c_int, C.c_int]),
        'queue': (C.c_uint64, [pointer, C.POINTER(C.c_float), C.c_uint32]),
        'queued_us': (C.c_uint64, [pointer]), 'close': (None, [pointer]),
        'shutdown': (None, []), 'set_speaker': (C.c_int, [C.c_int, C.c_int]),
        'controls': (C.c_int, [C.c_int, u8p, u8p, C.c_uint8, C.c_uint16,
                               C.c_uint16, C.c_uint32, C.c_uint64]),
        'renew': (C.c_int, [C.c_int, C.c_uint64]),
        'accepted_reports': (C.c_uint64, []), 'failed_reports': (C.c_uint64, []),
    }
    for name, (result, arguments) in signatures.items():
        function = getattr(lib, 'kyty_bt_' + name)
        function.restype, function.argtypes = result, arguments
    return lib


def _header(h, kind):
    widths = dict(type=8, kind=8, channels=8, generation=32, producer_epoch=64,
                  port=32, rate=32, port_generation=64, sequence=64,
                  first_frame=64, source_time_us=64)
    if (not isinstance(h, dict) or any(type(h.get(k)) is not int or not 0 <= h[k] < 1 << bits
                                      for k, bits in widths.items()) or
            h['type'] != kind or h['kind'] not in (ACTUATOR, SPEAKER) or
            h['channels'] != 2 or h['port'] >= 32 or not 1000 <= h['rate'] <= 192000 or
            any(not h[k] for k in ('generation', 'producer_epoch', 'port_generation', 'sequence')) or
            (kind == 2 and h['first_frame'] != 0) or (kind == 3 and not h['source_time_us'])):
        raise ValueError('Invalid native audio stream header')


def _motors(large, small):
    if any(type(v) is not int or not 0 <= v <= 65535 for v in (large, small)):
        raise ValueError('Invalid ordinary rumble')


class NativeAudio:
    """One output owner; Python and native deadlines use different clocks.

    Inject a C-ABI-shaped driver and monotonic clock for device-free tests.
    acquire() belongs on the SDL worker after Controller.stop(). reset() retains
    output ownership and fresh adaptive triggers; shutdown() releases everything.
    """
    def __init__(self, serial=None, *, library_path=None, driver=None, clock=time.monotonic):
        self.serial, self.clock = serial, clock
        self.library_path = library_path or os.environ.get('KYTY_BT_LIBRARY') or (
            Path(__file__).resolve().parent / 'native/build/libkyty_companion_bt.so')
        self._driver = driver
        self._lock = threading.RLock()
        self._controller = None
        self._failures_at_acquire = 0
        self._closed = self._prepared = self._fatal = False
        self.owns_output = False
        self.status = 'Original Bluetooth output not acquired'
        self._ports, self._generations = {}, {}
        self._lease = self._rumble_until = 0.0
        self._left = self._right = OFF
        self._rumble = (0, 0)

    def _call(self, name, *args):
        return getattr(self._driver, 'kyty_bt_' + name)(*args)

    def _error(self, operation):
        message = self._call('error') or b'native driver rejected operation'
        if isinstance(message, bytes):
            message = message.decode('utf-8', 'replace')
        self.status = (f'Original Bluetooth {operation} failed: {message}').replace('\n', ' ')[:240]

    def acquire(self):
        with self._lock:
            if self._closed or self._fatal:
                return False
            if self.owns_output:
                return True
            if self._driver is None:
                self._driver = _load(self.library_path)
            controller = self._call('find_controller', self.serial.encode() if self.serial else None)
            if controller < 0:
                self._error('acquire')
                return False
            self._controller = controller
            self._failures_at_acquire = self._call('failed_reports')
            self.owns_output = True
            if not self._send_controls():
                self.shutdown()
                return False
            self.status = 'Original emulator Bluetooth output acquired'
            return True

    def _send_controls(self):
        now = self.clock()
        remaining = max(0, self._lease - now)
        deadline = self._call('now_us') + int(remaining * 1000000) if remaining else 0
        duration = max(0, min(1000, int((self._rumble_until - now) * 1000)))
        large, small = self._rumble if remaining and duration else (0, 0)
        left, right = (self._left, self._right) if remaining else (OFF, OFF)
        result = self._call('controls', self._controller,
                            (C.c_uint8 * 11).from_buffer_copy(left),
                            (C.c_uint8 * 11).from_buffer_copy(right),
                            12, large, small, duration, deadline)
        if result != 1:
            self._error('controls')
            self._fatal = True
            self._lease = self._rumble_until = 0.0
            self._left = self._right = OFF
            self._rumble = (0, 0)
            self._prepared = False
            self._close_ports()
            off = (C.c_uint8 * 11).from_buffer_copy(OFF)
            self._call('controls', self._controller, off, off, 12, 0, 0, 0, 0)
            return False
        return True

    def _close_ports(self):
        ports, self._ports = self._ports, {}
        for port in ports.values():
            self._call('close', port['handle'])

    def _invalidate(self):
        self._lease = self._rumble_until = 0.0
        self._left = self._right = OFF
        self._rumble = (0, 0)
        self._prepared = False
        self._close_ports()
        if self.owns_output:
            return self._send_controls()
        return True

    def _expire(self):
        if self._lease and self.clock() >= self._lease:
            self._invalidate()
            self.status = 'Controller input lease expired; native effects stopped'

    def prepare(self):
        with self._lock:
            if not self.acquire():
                return False
            self._expire()
            if self._fatal:
                return False
            if not self._prepared:
                self._rumble, self._rumble_until = (0, 0), 0.0
                if not self._send_controls():
                    return False
            self._prepared = True
            self.status = 'Original emulator Bluetooth media ready'
            return True

    def open(self, header):
        _header(header, 2)
        with self._lock:
            self._expire()
            if not self._prepared or not self.owns_output or self._fatal or self.clock() >= self._lease:
                return False
            port = header['port']
            identity = (header['generation'], header['producer_epoch'], header['port_generation'])
            previous = self._generations.get(port)
            if (port in self._ports or previous is not None and
                    (identity[0] < previous[0] or identity[0] == previous[0] and
                     (identity[1] != previous[1] or identity[2] <= previous[2]))):
                return False
            handle = self._call('open', header['rate'], int(header['kind'] == SPEAKER), self._controller)
            if not handle:
                self._error('open')
                return False
            self._ports[port] = dict(handle=handle, header=dict(header), next_frame=0,
                                     sequence=header['sequence'], submitted_until=0.0)
            self._generations[port] = identity
            return True

    def pcm(self, header, payload, now_us):
        _header(header, 3)
        if (type(now_us) is not int or now_us < 0 or not isinstance(payload, (bytes, bytearray)) or
                not payload or len(payload) % 8 or len(payload) > 16384 or
                len(payload) // 8 > (header['rate'] + 99) // 100):
            raise ValueError('Invalid native stereo float PCM block')
        values = struct.unpack('<' + 'f' * (len(payload) // 4), payload)
        if any(not math.isfinite(v) for v in values):
            raise ValueError('PCM contains non-finite samples')
        with self._lock:
            self._expire()
            if not self._prepared or not self.owns_output or self._fatal or self.clock() >= self._lease:
                return False
            port = self._ports.get(header['port'])
            frames = len(payload) // 8
            if (port is None or any(header[k] != port['header'][k] for k in _IDENTITIES) or
                    header['sequence'] <= port['sequence'] or header['first_frame'] != port['next_frame'] or
                    header['first_frame'] + frames >= 1 << 64):
                return False
            duration = (frames * 1000000 + header['rate'] - 1) // header['rate']
            queued = self._call('queued_us', port['handle'])
            age = max(now_us, math.ceil(self.clock() * 1000000)) - header['source_time_us']
            if (age < -20000 or queued + duration + TRANSPORT_US > MAX_AGE_US or
                    age + queued + duration + TRANSPORT_US > MAX_AGE_US):
                self.status = 'Native PCM rejected: source age or queue exceeds 80 ms'
                return False
            # Per-port gain is applied by the producer. Preserve signed samples.
            samples = (C.c_float * len(values))(*values)
            native_queue = self._call('queue', port['handle'], samples, frames)
            if not native_queue:
                self._error('queue')
                status = self.status
                self._invalidate()
                self.status = status
                return False
            port['submitted_until'] = self.clock() + (native_queue + TRANSPORT_US) / 1000000
            port['next_frame'] += frames
            port['sequence'] = header['sequence']
            self.status = 'Original emulator Bluetooth media running'
            return True

    def controls(self, feedback, deadline_monotonic_seconds):
        now = self.clock()
        if (type(deadline_monotonic_seconds) not in (int, float) or
                not math.isfinite(deadline_monotonic_seconds) or deadline_monotonic_seconds > now + .200001):
            raise ValueError('Controller output lease must be at most 200 ms')
        try:
            large, small = feedback['rumble']
            triggers = feedback['triggers']
            flags = triggers['flags']
            sides = [triggers[key] for key in ('left', 'right')]
            if (type(flags) is not int or flags & ~12 or any(len(side) != 11 or
                    any(type(v) is not int or not 0 <= v <= 255 for v in side) for side in sides)):
                raise ValueError('Invalid adaptive trigger effects')
            _motors(large, small)
        except (KeyError, TypeError) as error:
            raise ValueError('Invalid controller feedback') from error
        with self._lock:
            if not self.owns_output or self._closed or self._fatal:
                return False
            self._expire()
            if deadline_monotonic_seconds <= self.clock():
                return self._invalidate()
            if flags & 8:
                self._left = bytes(sides[0])
            if flags & 4:
                self._right = bytes(sides[1])
            self._lease = float(deadline_monotonic_seconds)
            if not self._prepared:
                self._rumble = (large, small)
                self._rumble_until = self._lease
            return self._send_controls()

    def rumble(self, large, small, duration_ms, now_us):
        _motors(large, small)
        if (type(duration_ms) is not int or not 0 <= duration_ms <= 1000 or
                type(now_us) is not int or now_us < 0):
            raise ValueError('Invalid rumble duration or timestamp')
        with self._lock:
            self._expire()
            now = self.clock()
            if not self._prepared or not self.owns_output or self._fatal or now >= self._lease:
                return False
            self._rumble = (large, small)
            self._rumble_until = min(now, now_us / 1000000) + duration_ms / 1000
            return self._send_controls()

    def close(self, port):
        with self._lock:
            old = self._ports.pop(port, None)
            if old:
                self._call('close', old['handle'])
            return not self._fatal

    def reset(self):
        with self._lock:
            self._close_ports()
            self._prepared = False
            self._rumble, self._rumble_until = (0, 0), 0.0
            self._expire()
            return self._send_controls() if self.owns_output and not self._fatal else not self._fatal

    def tick(self, now_us):
        with self._lock:
            self._expire()
            if self.owns_output and not self._fatal and self._call('failed_reports') > self._failures_at_acquire:
                self.status = 'Original Bluetooth media transport rejected a report'
                self._invalidate()
                self._fatal = True
            return self.owns_output and not self._fatal

    def queued_us(self, port=None):
        with self._lock:
            self._expire()
            if self._fatal:
                raise RuntimeError(self.status)
            ports = self._ports.values() if port is None else [self._ports[port]] if port in self._ports else []
            return max((max(int(self._call('queued_us', p['handle'])),
                            max(0, math.ceil((p['submitted_until'] - self.clock()) * 1000000)))
                        for p in ports), default=0)

    @property
    def reports_accepted(self):
        return int(self._call('accepted_reports')) if self._driver else 0

    def shutdown(self):
        with self._lock:
            if self._closed:
                return
            try:
                self._invalidate()
            finally:
                if self._controller is not None:
                    self._call('shutdown')
                self.owns_output = False
                self._closed = True
                self.status = 'Original emulator Bluetooth output stopped'
