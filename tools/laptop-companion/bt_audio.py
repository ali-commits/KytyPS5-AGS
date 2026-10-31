"""Native DualSense Bluetooth media; SDL remains responsible for calibrated input.

One writer owns every output report. Packet layouts and transport conversion match
the pinned Artemis reference described in ADVANCED-MEDIA.md in the TV test app.
No Bluetooth report is a physical playback acknowledgement.
"""
import array
import ctypes as C
import ctypes.util
import math
import os
from pathlib import Path
import sys
import threading
import time
import zlib
from collections import deque

PERIOD = 512 / 48000
MAX_AGE_US = 80000
ACTUATOR, SPEAKER = 1, 2
OFF = bytes([5]) + bytes(10)
# Genuine CELT stereo CBR silence, verified by libopus and the Java fixtures.
SILENCE = bytes.fromhex('f4fffe') + bytes(197)


def _report(size, report_id, sequence):
    p = bytearray(size)
    p[0], p[1] = report_id, (sequence & 15) << 4
    return p


def _crc(p):
    p[-4:] = zlib.crc32(b'\xa2' + p[:-4]).to_bytes(4, 'little')
    return bytes(p)


def state_report(sequence, left=OFF, right=OFF, rumble=(0, 0), native=False):
    if len(left) != 11 or len(right) != 11:
        raise ValueError('Trigger effect must contain eleven bytes')
    if len(rumble) != 2 or any(type(v) is not int or not 0 <= v <= 65535 for v in rumble):
        raise ValueError('Invalid ordinary rumble')
    p = _report(78, 0x31, sequence)
    p[2:7] = bytes([0x10, (0xff if any(rumble) else 0xfc) if native else 0x0f,
                    0xd5 if native else 0x15, rumble[1] >> 8, rumble[0] >> 8])
    p[13:24], p[24:35] = right, left
    p[41], p[44] = 3, 2
    if native:
        p[7:11] = bytes([0, 100, 255, 9])
        p[39:41], p[46] = bytes([10, 3]), 0x20
    return _crc(p)


def setup_report(sequence):
    p = _report(398, 0x36, sequence)
    p[2:10] = bytes([0x91, 7, 0xfe] + [64]*5)
    p[11:15] = bytes([0x90, 45, 0xa0, 0x80])
    p[17], p[18], p[20], p[22], p[50] = 127, 127, 255, 16, 7
    p[76:78], p[142:144] = bytes([0x92, 64]), bytes([0x93, 200])
    p[144:344] = SILENCE
    return _crc(p)


def media_report(sequence, counter, haptics, speaker=None):
    if len(haptics) != 64 or speaker is not None and len(speaker) != 200:
        raise ValueError('Native media requires 64 haptic and 200 Opus bytes')
    p = _report(206 if speaker is None else 334, 0x33 if speaker is None else 0x35, sequence)
    p[2:11] = bytes([0x91, 7, 0xfe] + [128]*5 + [counter & 255])
    if speaker is None:
        p[11:13], p[13:77] = bytes([0x92, 64]), haptics
    else:
        p[11:13], p[13:213] = bytes([0x93, 200]), speaker
        p[213:215], p[215:279] = bytes([0x92, 64]), haptics
    return _crc(p)


class Haptics:
    """Continuous 48 kHz stereo FIR/16 decimator, preserving signed waveforms."""
    def __init__(self):
        taps = []
        for n in range(127):
            x = n - 63
            sinc = 2*1400/48000 if not x else math.sin(2*math.pi*1400*x/48000)/(math.pi*x)
            taps.append(sinc * (.42 - .5*math.cos(2*math.pi*n/126) + .08*math.cos(4*math.pi*n/126)))
        self.taps = [v / sum(taps) for v in taps]
        self.history = [[0.0]*127, [0.0]*127]
        self.position = self.phase = 0

    def convert(self, samples):
        if len(samples) % 2:
            raise ValueError('Expected interleaved stereo')
        out = bytearray()
        for i in range(0, len(samples), 2):
            self.history[0][self.position], self.history[1][self.position] = samples[i:i+2]
            self.position = (self.position + 1) % 127
            self.phase += 1
            if self.phase == 16:
                self.phase = 0
                for channel in self.history:
                    filtered = sum(channel[(self.position-1-n) % 127] * t for n,t in enumerate(self.taps))
                    # Kotlin roundToInt uses ties toward positive infinity.
                    sample = math.floor(filtered * 127 * 2 + .5)
                    out.append(max(-128, min(127, sample)) & 255)
        return bytes(out)


class Opus:
    """Persistent 48 kHz stereo 160 kbit/s CBR encoder on the 512:480 clock."""
    def __init__(self, gain=.25):
        if not 0 <= gain <= .30:
            raise ValueError('Speaker gain must be between zero and 0.30')
        self.gain = gain
        self.lib = C.CDLL(ctypes.util.find_library('opus') or 'libopus.so.0')
        self.lib.opus_encoder_create.argtypes = [C.c_int, C.c_int, C.c_int, C.POINTER(C.c_int)]
        self.lib.opus_encoder_create.restype = C.c_void_p
        self.lib.opus_encoder_ctl.argtypes = [C.c_void_p, C.c_int]
        self.lib.opus_encode.argtypes = [C.c_void_p, C.c_void_p, C.c_int, C.c_void_p, C.c_int]
        self.lib.opus_encoder_destroy.argtypes = [C.c_void_p]
        error = C.c_int()
        self.encoder = self.lib.opus_encoder_create(48000, 2, 2049, C.byref(error))
        if not self.encoder or error.value:
            raise RuntimeError('Cannot create controller Opus encoder')
        try:
            for request,value in ((4002,160000), (4006,0), (4010,0), (4040,5003)):
                if self.lib.opus_encoder_ctl(self.encoder, request, value) < 0:
                    raise RuntimeError('Cannot configure controller Opus encoder')
        except Exception:
            self.close()
            raise

    def encode(self, stereo):
        if len(stereo) != 1024:
            raise ValueError('Speaker transport requires 512 stereo source frames')
        source = [max(-32768, min(32767, round(v*self.gain*32768))) for v in stereo]
        pcm = (C.c_int16 * 960)()
        for n in range(480):
            at, fraction = divmod(n*16, 15)
            for c in range(2):
                value = source[at*2+c]*(15-fraction) + source[min(at+1,511)*2+c]*fraction
                pcm[n*2+c] = int(value/15)
        output = (C.c_ubyte * 200)()
        size = self.lib.opus_encode(self.encoder, pcm, 480, output, 200)
        if size != 200:
            raise RuntimeError(f'Controller Opus encoded {size} bytes, expected 200')
        return bytes(output)

    def reset(self):
        if self.encoder and self.lib.opus_encoder_ctl(self.encoder, 4028) < 0:
            raise RuntimeError('Cannot reset controller Opus encoder')

    def close(self):
        if self.encoder:
            self.lib.opus_encoder_destroy(self.encoder)
            self.encoder = None


class _SrcData(C.Structure):
    _fields_ = [('data_in', C.POINTER(C.c_float)), ('data_out', C.POINTER(C.c_float)),
                ('input_frames', C.c_long), ('output_frames', C.c_long),
                ('input_frames_used', C.c_long), ('output_frames_gen', C.c_long),
                ('end_of_input', C.c_int), ('src_ratio', C.c_double)]


class Resampler:
    """Per-port streaming conversion to the shared 48 kHz transport clock."""
    def __init__(self, rate):
        if type(rate) is not int or not 1000 <= rate <= 192000:
            raise ValueError('Invalid PCM sample rate')
        self.rate, self.state = rate, None
        self._input_seconds = self._output_seconds = 0.0
        if rate == 48000:
            return
        self.lib = C.CDLL(ctypes.util.find_library('samplerate') or 'libsamplerate.so.0')
        self.lib.src_new.argtypes = [C.c_int, C.c_int, C.POINTER(C.c_int)]
        self.lib.src_new.restype = C.c_void_p
        self.lib.src_process.argtypes = [C.c_void_p, C.POINTER(_SrcData)]
        self.lib.src_reset.argtypes = [C.c_void_p]
        self.lib.src_delete.argtypes = [C.c_void_p]
        self.lib.src_delete.restype = C.c_void_p
        error = C.c_int()
        self.state = self.lib.src_new(2, 2, C.byref(error))
        if not self.state or error.value:
            raise RuntimeError('Cannot create PCM sample rate converter')

    def convert(self, stereo):
        if len(stereo) % 2:
            raise ValueError('Expected interleaved stereo')
        if self.rate == 48000:
            return list(stereo)
        size = math.ceil(len(stereo)/2 * 48000/self.rate) + 512
        src = (C.c_float * len(stereo))(*stereo)
        dest = (C.c_float * (size*2))()
        data = _SrcData(src, dest, len(stereo)//2, size, 0, 0, 0, 48000/self.rate)
        result = self.lib.src_process(self.state, C.byref(data))
        if result or data.input_frames_used != len(stereo)//2:
            raise RuntimeError('PCM resampler did not consume the bounded source block')
        self._input_seconds += data.input_frames_used/self.rate
        self._output_seconds += data.output_frames_gen/48000
        return list(dest[:data.output_frames_gen*2])

    def pending_us(self):
        return max(0, math.ceil((self._input_seconds-self._output_seconds)*1000000))

    def reset(self):
        if self.state and self.lib.src_reset(self.state):
            raise RuntimeError('Cannot reset PCM sample rate converter')
        self._input_seconds = self._output_seconds = 0.0

    def close(self):
        if self.state:
            self.lib.src_delete(self.state)
            self.state = None


def _serial(value):
    value = value.casefold()
    compact = value.replace(':', '').replace('-', '')
    if len(compact) == 12 and all(c in '0123456789abcdef' for c in compact):
        return compact
    return value


def discover(device_path=None, serial=None, sysfs=Path('/sys/class/hidraw')):
    """Resolve one exact Bluetooth Sony DualSense, never a USB or guessed node."""
    matches = []
    for node in Path(sysfs).glob('hidraw*'):
        try:
            values = dict(line.split('=', 1) for line in (node/'device/uevent').read_text().splitlines() if '=' in line)
            identity = tuple(int(v, 16) for v in values.get('HID_ID', '').split(':'))
        except (OSError, ValueError):
            continue
        if identity != (5, 0x054c, 0x0ce6):
            continue
        path = '/dev/' + node.name
        if device_path and os.path.realpath(device_path) != path:
            continue
        if serial and _serial(values.get('HID_UNIQ', '')) != _serial(serial):
            continue
        matches.append(path)
    if len(matches) != 1:
        raise RuntimeError('Select exactly one Bluetooth DualSense matching its controller serial')
    return matches[0]


class _Port:
    def __init__(self, header):
        self.header = dict(header)
        self.resampler = Resampler(header['rate'])
        self.chunks = deque()
        self.frames = 0
        self.source_origin_us = self.expected_source_us = None
        self.output_frames = 0
        self.submitted_until = 0.0

    def discard(self):
        self.chunks.clear()
        self.frames = 0
        self.resampler.reset()
        self.source_origin_us = self.expected_source_us = None
        self.output_frames = 0
        self.submitted_until = 0.0

    def expire(self, now_us):
        while self.chunks:
            samples, source_us, offset = self.chunks[0]
            remaining_us = min(512,(len(samples)-offset)//2) * 1000000 // 48000
            if now_us - (source_us + offset * 1000000 // 96000) + remaining_us <= MAX_AGE_US:
                break
            self.frames -= (len(samples)-offset)//2
            self.chunks.popleft()

    def take(self, frames):
        out = []
        while frames and self.chunks:
            samples, source_us, offset = self.chunks[0]
            count = min(frames, (len(samples)-offset)//2)
            out.extend(samples[offset:offset+count*2])
            offset += count*2
            frames -= count
            self.frames -= count
            if offset == len(samples):
                self.chunks.popleft()
            else:
                self.chunks[0][2] = offset
        return out


class BluetoothAudio:
    """KCPA sink with bounded PCM storage and a single asynchronous HID writer.

    Call acquire() after stopping SDL feedback. reset() withdraws only PCM
    readiness, retaining ordinary output ownership until shutdown(). All times
    are local monotonic; PCM source timestamps must already be clock-mapped.
    Injecting writer(bytes)->count bypasses all discovery/device access for tests.
    """
    def __init__(self, device_path=None, serial=None, clock=time.monotonic, writer=None):
        self.device_path, self.serial, self.clock = device_path, serial, clock
        self._writer, self._fd = writer, None
        self._condition = threading.Condition(threading.RLock())
        self._thread = None
        self._started = threading.Event()
        self._closed = self._fatal = self._prepared = False
        self.owns_output = False
        self.status = 'Bluetooth output not acquired'
        self._ports = {}
        self._lease = self._rumble_until = self._pcm_until = 0.0
        self._left = self._right = OFF
        self._rumble = (0, 0)
        self._armed = False
        self._next_media = self._last_pcm = 0.0
        self._sequence = self._counter = 0
        self._last_state = None
        self._last_state_at = 0.0
        self._urgent = True
        self._codec = None
        self._haptics = Haptics()
        self._codec_reset = False
        self.reports_accepted = self.media_accepted = 0

    def acquire(self):
        with self._condition:
            if self._closed or self._fatal:
                return False
            if self.owns_output:
                return True
            if self._writer is None:
                path = discover(self.device_path, self.serial)
                # O_NONBLOCK prevents application queues from hiding a stalled
                # transport. A rejected/partial write fails the owned output.
                self._fd = os.open(path, os.O_WRONLY | os.O_NONBLOCK | os.O_CLOEXEC)
                self._writer = lambda p: os.write(self._fd, p)
            self.owns_output = True
            self._thread = threading.Thread(target=self._run, daemon=True, name='kyty-bt-output')
            self._thread.start()
        if not self._started.wait(1):
            self.shutdown()
            raise RuntimeError('Bluetooth output writer did not start')
        return self.owns_output

    def prepare(self):
        if not self.acquire():
            return False
        with self._condition:
            if self._closed or self._fatal or not self.owns_output:
                return False
            if self._codec is None:
                self._codec = Opus()
            if not self._prepared:
                # Ready transfers motor authority from HTTPS to KCPA. Never
                # carry a cached fallback effect into the new audio lease.
                self._rumble = (0,0)
                self._rumble_until = 0.0
                self._urgent = True
            self._prepared = True
            self.status = 'Bluetooth native media ready'
            self._condition.notify()
            return True

    def open(self, header):
        with self._condition:
            if not self._prepared or not self.owns_output:
                return False
            port, kind, rate = header['port'], header['kind'], header['rate']
            if type(port) is not int or not 0 <= port < 32 or kind not in (ACTUATOR, SPEAKER) or header['channels'] != 2:
                raise ValueError('Invalid native audio port')
            if port in self._ports:
                return False
            self._ports[port] = _Port(header)
            self._condition.notify()
            return True

    def pcm(self, header, payload, now_us):
        if not payload or len(payload) % 8 or len(payload) > 16384:
            raise ValueError('Invalid stereo float PCM size')
        samples = array.array('f')
        samples.frombytes(payload)
        if sys.byteorder != 'little':
            samples.byteswap()
        if any(not math.isfinite(v) for v in samples):
            raise ValueError('PCM contains non-finite samples')
        with self._condition:
            now = self.clock()
            if not self._prepared or not self.owns_output or now >= self._lease:
                return False
            port = self._ports.get(header['port'])
            if port is None or any(header[key] != port.header[key] for key in ('kind','rate','generation','producer_epoch','port_generation')):
                return False
            duration = (len(samples)//2 * 1000000 + header['rate']-1)//header['rate']
            source_us = header['source_time_us']
            queued = self._queued_us(port)
            if source_us > now_us+20000 or now_us-source_us+duration+queued > MAX_AGE_US or queued+duration > MAX_AGE_US:
                self.status = 'Bluetooth PCM rejected: source age or queue exceeds 80 ms'
                return False
            converted = port.resampler.convert(samples)
            if port.expected_source_us is not None and abs(source_us-port.expected_source_us) > 20000:
                # Source sample positions may continue across a long producer
                # pause. Rebase a discontinuous source clock instead of making
                # every subsequent fresh block inherit the earlier timestamp.
                port.discard()
                converted = port.resampler.convert(samples)
                self._haptics = Haptics()
            if port.source_origin_us is None:
                port.source_origin_us = source_us
            port.expected_source_us = source_us + duration
            if converted:
                converted_source_us = (source_us if header['rate'] == 48000 else
                    port.source_origin_us + port.output_frames*1000000//48000)
                port.chunks.append([converted, converted_source_us, 0])
                port.frames += len(converted)//2
                port.output_frames += len(converted)//2
            if header['kind'] == ACTUATOR and any(abs(v) > 1/1024 for v in samples):
                if now >= self._pcm_until:
                    self._urgent = True
                self._pcm_until = max(self._pcm_until, now + .25)
            self._last_pcm = now
            if not self._next_media:
                self._next_media = now + PERIOD
            self._condition.notify()
            return True

    def controls(self, feedback, deadline_monotonic_seconds):
        now = self.clock()
        if not isinstance(deadline_monotonic_seconds, (float,int)) or not math.isfinite(deadline_monotonic_seconds) or deadline_monotonic_seconds > now+.201:
            raise ValueError('Controller output lease must be at most 200 ms')
        rumble, triggers = feedback['rumble'], feedback['triggers']
        flags = triggers['flags']
        if type(flags) is not int or flags & ~12:
            raise ValueError('Invalid trigger flags')
        # The pure builder validates both trigger sides and rumble before state
        # is changed; an unselected side must never erase the cached effect.
        sides = []
        for key in ('left', 'right'):
            values = triggers[key]
            if len(values) != 11 or any(type(v) is not int or not 0 <= v <= 255 for v in values):
                raise ValueError('Invalid trigger effect')
            sides.append(bytes(values))
        state_report(0, *sides, rumble)
        with self._condition:
            if not self.owns_output or self._closed or self._fatal:
                return False
            if flags & 8:
                self._left = sides[0]
            if flags & 4:
                self._right = sides[1]
            self._lease = float(deadline_monotonic_seconds)
            if not self._prepared:
                self._rumble = tuple(rumble)
                self._rumble_until = self._lease
            if self._lease <= now:
                self._invalidate_input()
            self._condition.notify()
            return True

    def rumble(self, large, small, duration_ms, now_us):
        if type(duration_ms) is not int or not 0 <= duration_ms <= 1000:
            raise ValueError('Invalid rumble duration')
        state_report(0, rumble=(large, small))
        with self._condition:
            if not self._prepared or not self.owns_output or self.clock() >= self._lease:
                return False
            self._rumble = (large, small)
            # Input freshness remains a separate gate in _state(). Healthy
            # input renewals must not shorten this event's absolute duration.
            self._rumble_until = now_us/1000000 + duration_ms/1000
            self._urgent = True
            self._condition.notify()
            return True

    def close(self, port):
        with self._condition:
            old = self._ports.pop(port, None)
            if old:
                old.resampler.close()
            if not self._ports:
                self._stop_media()
            self._condition.notify()
            return not self._fatal

    def reset(self):
        with self._condition:
            for port in self._ports.values():
                port.resampler.close()
            self._ports.clear()
            self._prepared = False
            self._rumble = (0,0)
            self._rumble_until = 0.0
            self._stop_media()
            self._condition.notify()
            return not self._fatal

    def _stop_media(self):
        self._armed = False
        self._next_media = self._last_pcm = self._pcm_until = 0.0
        self._haptics = Haptics()
        self._codec_reset = True
        self._urgent = True

    def tick(self, now_us):
        with self._condition:
            self._condition.notify()
            return self.owns_output and not self._fatal

    def _queued_us(self, port):
        submitted = max(0, math.ceil((port.submitted_until-self.clock())*1000000))
        return math.ceil(port.frames*1000000/48000) + port.resampler.pending_us() + submitted

    def queued_us(self, port=None):
        with self._condition:
            if self._fatal:
                raise RuntimeError(self.status)
            if port is not None:
                value = self._ports.get(port)
                return self._queued_us(value) if value else 0
            return max((self._queued_us(p) for p in self._ports.values()), default=0)

    def shutdown(self):
        with self._condition:
            self._closed = True
            self._condition.notify()
            thread = self._thread
        if thread and thread is not threading.current_thread():
            thread.join(1)
            if thread.is_alive():
                self.status = 'Bluetooth output writer failed to stop'
                raise RuntimeError(self.status)

    def _send(self, report):
        written = self._writer(report)
        if written != len(report):
            raise OSError(f'short HID write: {written}/{len(report)}')
        self.reports_accepted += 1
        if report[0] in (0x33,0x35):
            self.media_accepted += 1

    def _tag(self):
        value = self._sequence
        self._sequence += 1
        return value

    def _state(self, now, force=False):
        live = now < self._lease
        rumble = self._rumble if live and now < self._rumble_until and now >= self._pcm_until else (0,0)
        left, right = (self._left, self._right) if live else (OFF,OFF)
        key = (left, right, rumble, self._armed)
        if self._last_state and self._last_state[2] != (0,0) and now >= self._rumble_until:
            self._urgent = True
        if (force or key != self._last_state) and (force or self._urgent or now-self._last_state_at >= (.096 if self._armed else .016)):
            self._send(state_report(self._tag(), left, right, rumble, self._armed))
            self._last_state, self._last_state_at, self._urgent = key, self.clock(), False

    def _expire(self, now):
        if self._lease and now >= self._lease:
            self._invalidate_input()
        for p in self._ports.values():
            p.expire(int(now*1000000))

    def _invalidate_input(self):
        self._lease = self._rumble_until = 0.0
        self._left = self._right = OFF
        self._rumble = (0,0)
        for p in self._ports.values():
            p.discard()
        self._stop_media()
        self.status = 'Controller input lease expired; effects stopped'

    def _media(self, now):
        now = self.clock()
        self._expire(now)
        if not self._prepared or now >= self._lease or not self._next_media or now+1e-9 < self._next_media:
            return
        has_frames = any(p.frames >= 512 for p in self._ports.values())
        if not has_frames and (not self._armed or now-self._last_pcm >= .25):
            if self._armed:
                self._stop_media()
            else:
                self._next_media = now + PERIOD
            return
        if not self._armed:
            self._send(setup_report(self._tag()))
            self._armed = True
            now = self.clock()
            self._expire(now)
            self._state(now, force=True)
            if not self._armed or now >= self._lease:
                return
            has_frames = any(p.frames >= 512 for p in self._ports.values())
        oldest = min((p.chunks[0][1] + p.chunks[0][2]*1000000//96000
                      for p in self._ports.values() if p.chunks), default=None)
        mixed = {ACTUATOR: [0.0]*1024, SPEAKER: [0.0]*1024}
        speaker = any(p.header['kind'] == SPEAKER for p in self._ports.values())
        contributors = []
        if has_frames:
            for p in self._ports.values():
                samples = p.take(512)
                if samples:
                    contributors.append(p)
                dest = mixed[p.header['kind']]
                for i,v in enumerate(samples):
                    dest[i] += v
        for samples in mixed.values():
            for i,v in enumerate(samples):
                samples[i] = max(-1.0, min(1.0, v))
        if has_frames:
            haptics = self._haptics.convert(mixed[ACTUATOR])
        else:
            haptics = bytes(64)
            self._haptics = Haptics()
        opus = self._codec.encode(mixed[SPEAKER]) if speaker else None
        finished = self.clock()
        if finished >= self._lease or oldest is not None and finished*1000000-oldest+PERIOD*1000000 > MAX_AGE_US:
            self._expire(finished)
            self._haptics = Haptics()
            if self._armed:
                self._next_media = finished + PERIOD
            return
        self._send(media_report(self._tag(), self._counter, haptics, opus))
        for p in contributors:
            p.submitted_until = self.clock() + PERIOD
        self.status = f'Bluetooth native media: {self.media_accepted} reports accepted'
        self._counter = (self._counter+1) & 255
        # Advance on the original clock. A late/slow write discards matching
        # source frames rather than emitting a burst into the kernel queue.
        following = self._next_media + PERIOD
        finished = self.clock()
        skipped = 0
        if following <= finished:
            while following < finished + PERIOD:
                following += PERIOD
                skipped += 1
        if skipped:
            for p in self._ports.values():
                p.take(skipped*512)
            self._haptics = Haptics()
        self._next_media = following

    def _run(self):
        try:
            with self._condition:
                self._state(self.clock(), force=True)
                self.status = 'Bluetooth output acquired'
                self._started.set()
                while not self._closed:
                    now = self.clock()
                    self._expire(now)
                    if self._codec_reset and self._codec:
                        self._codec.reset()
                        self._codec_reset = False
                    self._state(now)
                    self._media(now)
                    self._state(self.clock())
                    wait = .005
                    if self._next_media:
                        wait = min(wait, max(.0005, self._next_media-self.clock()))
                    self._condition.wait(wait)
        except Exception as e:
            self._fatal = True
            self.status = ('Bluetooth output failed: ' + str(e).replace('\n',' '))[:200]
        finally:
            # Executed only by this writer, after all unsent media is abandoned.
            with self._condition:
                self._started.set()
                try:
                    self._send(state_report(self._tag()))
                except Exception:
                    pass
                for port in self._ports.values():
                    port.resampler.close()
                self._ports.clear()
                if self._codec:
                    self._codec.close()
                    self._codec = None
                if self._fd is not None:
                    os.close(self._fd)
                    self._fd = None
                self._prepared = self.owns_output = False
                if not self._fatal:
                    self.status = 'Bluetooth output stopped'
