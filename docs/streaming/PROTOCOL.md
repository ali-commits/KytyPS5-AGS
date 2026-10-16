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
Backend waitsREADY, copies GPU data into its owned snapshot (or converts directly
when all readers are accounted for), signalsDONE, synchronizes CUDA completion,
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

## Managed host control contract

The independent backend reports capabilities with `--kyty-capabilities` and accepts
`<config-path> --creds-stdin` with one bounded JSON username/password line on stdin.
No credential values appear in output or process arguments. Capture options are
`capture=kyty`, `encoder=nvenc`, `kyty_socket`, `kyty_private_audio=true`,
`kyty_input=true`, and `management_bind_address=127.0.0.1`. The backend retains its
authenticated management API at configured base port+1; stream control stays LAN.
`SUNSHINE_ENABLE_WEB_UI=OFF` disables browser pages/build assets, not Moonlight or
management JSON APIs. The launcher must not invent an API launch endpoint.

Capabilities object: `{"protocol_version":1,"capture":"kyty",
"gpu_transport":"vulkan-buffer-fd","private_audio":true,"private_input":true,
"credentials_stdin":true}`. Parse a final JSON line if startup diagnostics precede it.

## Optional controller extensions (2026-10-16)

The video version, nine descriptors, GPU allocation/semaphore rules, frame leases and
8192-byte datagram limit remain version 1. Each handshake may have a `capabilities`
array with up to 16 strings. Recognized names are `motion-v1` and
`adaptive-triggers-v1`. Missing or malformed arrays enable neither extension;
unknown string names are ignored. The backend advertises supported names in
`welcome`; the producer includes only supported, advertised names in `hello`; the
backend echoes the agreed subset in `accepted`. An extension is active only after
all three messages agree. Legacy peers receive no new message types; their old
`hello` and `accepted` objects do not acquire a capabilities field.

An `input` object may additionally contain all three arrival fields:
`controller_type` (0..255), `controller_capabilities` (0..65535), and
`supported_buttons` (0..4294967295). Values retain the standard Moonlight controller
arrival identity and bitmasks, including PS family 2, accel bit 0x10 and gyro bit
0x20. The backend retains them across ordinary travel/button updates and clears
them when the controller/session detaches. Old producers may ignore these fields.

When `motion-v1` is agreed, the backend requests standard Moonlight acceleration
and gyro reports at 100 Hz, only for sensors the attached controller advertised.
The shared standard controller-extension feature bit 0x02 is advertised so stock
Moonlight permits motion reports; this adds no touchpad handling. On source or
session loss it requests rate zero for those sensors.

Backend to producer (no descriptors):

```json
{"type":"motion","controller":0,"sensor":"gyro","sequence":1,
 "timestamp_us":123456789,"x":3.141592653589793,"y":0.0,"z":-1.0}
```

`sensor` is `accel` or `gyro`. The three numeric values must be finite; acceleration
is m/s² within ±160 per axis, inclusive of gravity, and angular velocity is rad/s
within ±40 per axis. Standard Moonlight angular velocity arrives in degrees/s;
the independently implemented backend converts it to rad/s exactly once before
this boundary. Axes follow the standard SDL sensor convention without remapping.
The standard client packet has no sample timestamp, so `timestamp_us` is the host's
monotonic receive time (1..INT64_MAX microseconds), increasing per sensor.
`sequence` is a positive uint64 increasing per sensor. A duplicate or older sequence
or timestamp is ignored. A reset, controller removal, or socket disconnect resets
ordering and remote-owned IMU/fusion state without clearing local held inputs or
an IMU owned only by a local controller. The motion callback receives SI values;
Kyty's existing fusion converts acceleration to guest G and retains raw rad/s gyro.
Remote controller connection state is independent of sensor report frequency.

There are exactly two latest sensor mailboxes, one per type; newer events replace
unsent events. These are not queues. Socket I/O belongs to the source worker.

When `adaptive-triggers-v1` is agreed, producer to backend (no descriptors):

```json
{"type":"adaptive-triggers","controller":0,"sequence":7,"enable_bits":12,
 "left":[33,1,2,3,4,5,6,7,8,9,10],
 "right":[37,10,9,8,7,6,5,4,3,2,1]}
```

`sequence` is a positive monotonic feedback revision. `enable_bits` is exactly 4
(right), 8 (left), or 12 (both). Each report contains exactly 11 integer bytes
0..255: the translated DualSense mode plus all ten parameter bytes. Disabled sides
are preserved in the producer's bounded latest-state mailbox; successive masked
updates cannot lose the other side. The worker sends changed snapshots at most
once per 16 ms. The backend splits each report into its mode and ten parameters
and uses standard Moonlight adaptive-trigger feedback with the unchanged enable
mask. Only an authenticated, connected controller advertising PS family receives
these effects. Old feedback revisions are ignored. Trigger travel remains input;
ordinary rumble retains its existing independent packet and expiry.

Reset/stop/disconnect disables both sides with mask 12, mode 5 for both reports,
and ten zero parameters per side. Rumble expiry alone does not stop sensors or
adaptive effects. Controller callbacks do no socket I/O. Standard session feedback
uses the existing bounded 32-message queue; there is no added control endpoint.

Periodic producer `status` may add uint64 counters `motion_received`,
`motion_applied` (motion callback deliveries), and `adaptive_sent`. Existing local
backend status adds aggregate received/forwarded/invalid motion and adaptive
counts, negotiated extension flags, retained arrival identity, and the two latest
SI sample vectors (`last_motion_si`, accel then gyro). These are bounded snapshots,
not per-sample logs or a remote input/effect injection API. The executable's
`--kyty-capabilities` object also adds
`"controller_extensions":["motion-v1","adaptive-triggers-v1"]`.
