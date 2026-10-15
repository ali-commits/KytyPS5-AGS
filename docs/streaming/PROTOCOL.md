# Local Frame Feed Protocol v1

Independent wire specification; each program implements it under its own licence.
Transport: AF_UNIX SOCK_SEQPACKET, backend listener, emulator producer. Private
runtime directory0700/socket0600; both endpoints check SO_PEERCRED UID == geteuid().
Each datagram is one UTF-8 JSON object, maximum8192bytes. No trailing binary payload.
Reject truncated ancillary data, unknown versions, invalid types/ranges, surplus
FDs or unexpected messages. Close every received FD on all rejected/error paths.
No native pointers, raw C++ structs or shared source headers cross the interface.

## Negotiation

Backend sends {"type":"welcome","version":1} on accept.
Producer sends hello with SCM_RIGHTS in slot order:

```json
{"type":"hello","version":1,"epoch":1,"width":1920,"height":1080,
 "stride":8192,"fourcc":"BGRA","gpu_uuid":"32 lowercase hex digits",
 "slots":3,"allocation_sizes":[8847360,8847360,8847360],
 "offset":0,"dedicated":true,"transport":"vulkan-buffer-fd"}
```

Exactly9FDs: slot0 memory/READY/DONE, slot1 memory/READY/DONE, slot2memory/READY/DONE.
Dimensions1..4096 each, stride>=width*4, stride multiple4096, payload=stride*height
checked against overflow and allocation sizes; reject allocation sizes beyond256MiB.
Each memory FD describes a dedicated Vulkan OPAQUE_FD DEVICE_LOCAL buffer of offset0.
READY/DONE are exported binary OPAQUE_FD semaphores. UUID is Vulkan deviceUUID and
must equal the CUDA device UUID. Successful CUDA imports consume FD ownership.
Backend sends {"type":"accepted","version":1,"epoch":1,"transport":"vulkan-buffer-fd"}
or {"type":"error","message":"bounded diagnostic"}. No unbounded retries/log spam.

## Frames and leases

```json
{"type":"frame","epoch":1,"slot":0,"sequence":1,"pts_ns":123456789}
{"type":"release","epoch":1,"slot":0,"sequence":1}
```

sequence is positive monotonic uint64; epoch positive uint64; slot0..2. Producer
sends frame only after its submission signals READY and completion is observable.
Backend waitsREADY, converts GPU data, signalsDONE, synchronizes CUDA completion,
then release. Dropped frames undergo the same acquire/release without conversion.
Producer never marks a slot free merely because another frame is newer. Stale or
duplicate release is ignored/logged and cannot recycle a live slot. Producer polls
nonblocking; no free slot means skip this export, never pause emulation.

Before capture copy: recycled slot waits completed DONE and ownership is acquired
EXTERNAL->producer family. After copy: release producer family->EXTERNAL and signal
READY. Capture submit is separate from local swapchain acquire/present. Both source
game-layer lifetimes extend to the capture completion tick even without local present.

Pool replacement sends a new hello/new epoch only after old resources are quiescent,
or closes/reconnects and retains old imports until no CUDA work references them.
On peer death, stop producing and retire safely; reconnect starts fresh semaphores.
Max one producer and one active remote player; a second producer is rejected.

## Normalized input and feedback

Backend -> producer:

```json
{"type":"input","controller":0,"connected":true,"sequence":1,
 "buttons":["a","b","x","y","up","down","left","right","start","back",
            "lb","rb","ls","rs","guide"],
 "lx":0,"ly":0,"rx":0,"ry":0,"lt":0,"rt":0}
{"type":"reset","controller":0}
```

buttons is the currently held subset of the names above (no duplicates); sticks
-32768..32767 with negativeYup; triggers0..255. Stale sequences cannot restore holds.
Input is ignored outside an attached authenticated streaming session. A disconnect
or reset releases only remote-owned holds/axes as documented in source ownership.

Producer -> backend:

```json
{"type":"rumble","controller":0,"large":32768,"small":32768,"duration_ms":100}
{"type":"status","state":"attached","sequence":1,"dropped":0}
```

Rumble magnitudes0..65535, duration0..1000ms. Bound/throttle messages; feedback never
blocks audio/guest threads. Close/reset neutralizes feedback and remote inputs.
Audio uses the independently configured private PulseAudio sink, not this socket.
