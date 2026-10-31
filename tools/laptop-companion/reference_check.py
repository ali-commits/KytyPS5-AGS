"""Explicit, short physical checks using the emulator's actual Bluetooth driver."""
import math
import struct
import time


def run_reference(controller, kind, *, duration=2.0, canceled=lambda: False,
                  sink_factory=None, clock=time.monotonic, sleep=time.sleep):
    if kind not in (1, 2) or not .05 <= duration <= 2:
        raise ValueError('Reference must be haptics/speaker and at most two seconds')
    if sink_factory is None:
        from native_audio import NativeAudio
        sink_factory = NativeAudio
    if not controller.snapshot()['connected']:
        raise RuntimeError('Connect the DualSense to the laptop first')
    sink = sink_factory(serial=controller.serial)
    off = [5] + [0] * 10
    neutral = {'rumble': [0, 0], 'triggers': {'flags': 12, 'left': off, 'right': off}}
    attached = False
    stopped = False
    try:
        controller.attach_output(sink)
        attached = True
        reports_before = sink.reports_accepted
        sink.controls(neutral, clock() + .2)
        if not sink.prepare():
            raise RuntimeError(sink.status)
        h = dict(type=2, kind=kind, channels=2, generation=1, producer_epoch=1,
                 port=0, rate=48000, port_generation=1, sequence=1,
                 first_frame=0, source_time_us=0)
        if not sink.open(h):
            raise RuntimeError(sink.status)
        start = clock()
        blocks = math.ceil(duration * 100)
        for n in range(blocks):
            sleep(max(0, start + n * .01 - clock()))
            if canceled() or not controller.snapshot()['connected']:
                stopped = True
                break
            now = clock()
            sink.controls(neutral, now + .2)
            samples = []
            for i in range(480):
                t = (n * 480 + i) / 48000
                # Fade each short burst; reserve the final tenth for silence.
                envelope = min(1., t / .02, max(0., (duration * .9 - t) / .02))
                if kind == 2:
                    left = right = .04 * envelope * math.sin(2 * math.pi * 600 * t)
                else:
                    left = .12 * envelope * math.sin(2 * math.pi * (70 * t + 75 * t * t))
                    right = .12 * envelope * math.sin(2 * math.pi * (240 * t - 40 * t * t))
                samples.extend((left, right))
            packet = dict(h, type=3, sequence=n + 2, first_frame=n * 480,
                          source_time_us=int(now * 1000000))
            if not sink.pcm(packet, struct.pack('<' + 'f' * len(samples), *samples),
                            int(now * 1000000)):
                raise RuntimeError(sink.status)
        sink.close(0)
        sink.controls(neutral, 0)
        return dict(kind='speaker' if kind == 2 else 'haptics', canceled=stopped,
                    duration_seconds=duration,
                    reports_accepted=max(0, sink.reports_accepted - reports_before))
    finally:
        if attached:
            controller.detach_output()
        else:
            sink.shutdown()
