# Controller audio PCM producer

The binding contract, exact 72-byte field offsets, enums, limits and direction rules are in
[the approved specification](superpowers/specs/2026-10-16-dualsense-controller-audio.md).
`src/controller_audio` is dependency-free C++20 and can be linked by the emulator and its
standalone companion. The independently licensed host implements the wire format separately.

The native producer negotiates `controller-audio-v1` through the existing JSON v1 video
connection. Its nine-FD, 8192-byte `SOCK_SEQPACKET` framing is unchanged. Only an agreed peer
may send `controller-audio-state`. The additional private PCM channel is a `SOCK_STREAM`:
read exactly 72 header bytes, validate `WireSize`, then read the bounded payload. Integer
headers are big-endian; canonical stereo float payloads are explicitly little-endian.
The Unix Hello carries the raw 32-byte nonce; its matching Ready acknowledgement is empty.
Control JSON interpretation and direction/session checks belong to the relevant boundary.

`Publish` performs a bounded conversion/copy into preallocated storage under a short local
metadata/copy mutex. Its queue owns converted floats; only the worker packs F32LE bytes
and serializes frames. It never connects, writes, or waits for a receiver. No worker
allocation, encoding or I/O holds that mutex. Bad source data, stale port identity, unsupported block size, queue pressure or
an expired delivery deadline withdraw readiness. The worker closes that lease and reports
one bounded diagnostic per discontinuity. Native audio immediately resumes its existing
controller/PADSPK fallback policy; it does not call the stream lossless after discarding data.

The source queue holds at most 20 ms per port and 128 chunks of at most 16384 bytes, with
an aggregate storage static assertion below 4 MiB. Each PCM chunk is at most ceil(rate/100)
frames. Source blocks larger than ceil(rate/50) frames fall back. The worker also bounds
socket buffering and waits for the Unix send queue to drain before advancing, expiring a
stalled send at source_time_us + 20 ms. No receiver acknowledgement paces guest audio.
Normal video and ordinary PadSetVibration feedback continue through their existing paths;
the host routes ordinary rumble to the current companion lease.

Port generations increase on real native opens. Open, PCM and Close share kind, source rate
and generation, with increasing sequence and exact sample positions. A new lease clears
pending samples and replays only active metadata with sample position zero. Negotiated native
status adds ready, bytes delivered and discontinuities under `controller_audio`.

Silent tests use real Unix sockets and consuming fake SDL boundaries. They do not launch a
game/client or initialize controller audio/hidraw hardware. Physical USB proof remains a
separate, supervised delivery step.
