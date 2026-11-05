# Lossless DualSense controller audio and companion receiver

Date: 2026-10-16. User approved the companion design and selected full haptic waveforms and controller speaker before latency tuning. Keep the protocol reusable by a later custom Moonlight client and modified TV app; neither client fork is part of this stage.

Kyty base: c889dd109a771f9965eaa1a61d904e473d6e48ec. Host base: f35169866ddf9824a6a580abf0524bec694baede. Current tested runtime is v2/native3c624be5; preserve it.

## Goal

Preserve the actual controller actuator and PADSPK PCM produced by Kyty, including source format conversion, sample rates, timing and gains, through an authenticated host and a Linux USB companion next to unmodified Moonlight 6.2.0. Play the correct four USB channels and select the controller speaker through narrowly encoded HID reports. Normal game audio remains on laptop speakers. Preserve existing video, buttons, gyro and adaptive triggers.

Current Astro evidence: PADSPK is 512-frame/48000 Hz FloatMono and vibration is 512-frame/48000 Hz FloatStereo. Other valid guest rates/formats must be handled explicitly; do not assume every port is 48 kHz. No peak-to-two-motor reduction or lossy audio codec on the new PCM path.

## Approved approach and scope

- Companion application first; separate, documented binary receiver protocol suitable for later integrated clients.
- Linux USB DualSense 054c:0ce6 and one remote controller0 first. Bluetooth, Android/TV output implementations, controller microphone and headset support are deferred. Do not claim TV hardware parity.
- Use a separate negotiated private Unix PCM channel and dedicated TLS/TCP receiver service. Preserve existing JSON v1/8192-byte/nine-FD video contract.
- Keep Kyty/helper and Sunshine-derived host independent programs and implementations under their existing licences; the host must not include Kyty source/headers.
- New v3 candidate prefix/config; keep v2 exact executables, maps, pairing and current saves. Do not overwrite active executables or write experimental state into normal saves.
- No main-game-thread or audio-producer network I/O, receiver waits, unbounded queues or CPU video readback.
- User authorizes necessary laptop access and installations without repeated permission requests. Interactive game tests require their presence; unattended drive tests require a bounded deadline and verified full game/client closure. Never leave an automatically started game for the user to close.

## Pure PCM conversion and native export

Create a small dependency-free ControllerAudio PCM module. It selects source channels0/1, duplicates mono, normalizes S16 by32768, and applies guest per-channel volume, controller setting gain and optional AudioOut2 per-channel gains exactly once, matching current native output. Main/master output mix gains are not controller gains. F32 canonical samples must remain finite.

Port identity is an output handle0..31 plus a monotonically increasing nonzero uint64 port generation. Reopened handles cannot inherit old audio. Each stream has kind Actuator=1 or Speaker=2, source rate1000..192000 Hz, canonical stereo F32 and a sample position. Preserve source rates; receiver SDL resamples to the real USB endpoint.

Export AudioOutOutputs after gain selection and before vibration peak reduction or PADSPK main fallback. Local native playback uses the same conversion module. A ready remote lease exclusively routes controller audio remotely; it skips local controller playback and PADSPK main fallback. On lease loss, flush the epoch and restore existing fallback policy. Preserve sample-clock pacing without depending on receiver acknowledgements.

Producer callbacks may copy into preallocated bounded local storage only. Split data into chunks of at most10 ms and max16384 payload bytes. Source, broker and helper queues are bounded by samples/time and bytes. Source queue target/max20 ms per port, broker queue max20 ms per port, helper SDL queue target about20 ms. Aggregate sender/broker storage <=4 MiB each. Overall submitted-sample age plus pending playback must not exceed80 ms. On overflow, bad continuity or expired delivery deadline, explicitly discontinue/revoke the PCM lease and restore fallback; do not silently label dropped/stale PCM lossless.

Ports larger than supported storage or malformed rates/formats must cause a bounded diagnostic and fallback, not guest fatal termination. Source block splitting preserves first-frame positions and complete sample bytes.

## Ownership and regular rumble

The companion owns ordinary rumble while its controller-audio lease is ready: broker sends one ordinary-rumble stop through Moonlight, then routes subsequent ordinary rumble to the companion. This keeps Moonlight's cached nonzero rumble from disabling USB audio haptics during LED/other HID updates.

Vibration peak fallback is suppressed while PCM ownership is active. Existing PadSetVibration strengths/duration are still carried as Rumble frames to the helper. The helper mirrors native arbitration: actual actuator PCM suppresses ordinary rumble while queued/recent; outside PCM it may use narrowly encoded USB emulated-rumble reports. Do not overwrite trigger or LED fields. On lease loss, stop helper rumble/audio and restore ordinary Moonlight feedback. On readiness transition, use explicit ordered state/reset and ensure the stock client's rumble cache receives zero before regular feedback is suppressed.

Speaker routing follows native USB output report semantics: zero-padded48-byte report, ID0x02, audio-control/volume fields only, excluding trigger and LED enable bits. Native 38-byte effect bodies are not complete hidraw reports. Stereo speaker occupies front-left/front-right; actuator PCM occupies rear-left/rear-right. On close/reset restore only helper-owned speaker/audio routing and zero owned output.

## Frozen binary frame contract v1

All integer header fields are unsigned big-endian. Header is exactly72 bytes; PCM payload is IEEE754 float32 little-endian, interleaved stereo. Serialize explicitly, never memcpy C++ structs.

| Offset | Bytes | Field |
|---|---|---|
|0|4|ASCII KCPA|
|4|1|version=1|
|5|1|type|
|6|1|kind:0 control,1 actuator,2 speaker|
|7|1|channels:2 for Open/Pcm/Close,0 for control|
|8|4|header bytes=72|
|12|4|payload bytes <=16384|
|16|8|audio generation/lease|
|24|8|producer epoch|
|32|4|port ID|
|36|4|source sample rate|
|40|8|port generation|
|48|8|sequence|
|56|8|first frame/sample position|
|64|8|source_time_us: host CLOCK_MONOTONIC microseconds, nonzero for PCM|

Types: Hello=1, Open=2, Pcm=3, Close=4, Reset=5, Ready=6, State=7, Rumble=8, Ping=9, Pong=10, Error=11, Status=12.

Open/Pcm/Close carry a consistent kind, rate, channels and port generation. Open has no payload; Pcm is nonempty, length divisible by8, finite samples, and frames<=ceil(rate/100), never over10 ms apart from integer rounding. Close has no payload and flushes the port. Sequence increases per port; Pcm first_frame must match expected continuity. Reset clears all ports for its lease. Old generations/epochs cannot replay data.

Control frames have zero port/rate/port-generation/sample-position fields and channels/kind0; source_time_us may be0. Control JSON is UTF-8 and <=4096 bytes except the private Unix Hello, whose payload is exactly32 nonce bytes. Parse the fixed header before bounded payload allocation. Reject invalid lengths, overflow, reserved enum values, direction violations, non-finite data, mismatched lease and stale/recycled ports.

Rumble is8 big-endian bytes: uint16 large, uint16 small, uint32 duration_ms0..1000. Control header identifies current lease/epoch; sequence is a strictly increasing control sequence. Receiver converts strengths as native SDL does and expires them.

Network Hello JSON: protocol_version1, receiver_id exactly64 lower-case hex SHA256 of the helper DER certificate, controller0, output:"usb-dualsense-4ch". Its header generation/epoch are0. Peer authentication precedes Hello. State JSON uses bounded state waiting/preparing/active/reset/error, reason<=128, and generation/epoch in the header. Ready acknowledges the proposed lease and a validated usable output, no incoming PCM from receivers. Status carries only bounded device state/counters/queued_us. Ping/Pong JSON carries client_send_us and server_receive_us/server_send_us for clock mapping; refresh periodically. Use minimum-RTT samples to estimate host monotonic offset and its uncertainty. Refuse ready if uncertainty exceeds20 ms; reject/flush data whose conservative age plus queued playback exceeds80 ms. Keep drift bounded with a small tested SDL frequency-ratio servo (within0.995..1.005); sample-exact transport does not mean physical device clocks are identical.

## Private Unix negotiation

Add optional capability controller-audio-v1 to existing three-way video negotiation. Only an agreed peer sees new state. Video, motion and adaptive-trigger framing stays unchanged. The separate PCM endpoint is AF_UNIX SOCK_STREAM with explicit72-byte header/payload framing; the old video endpoint remains SOCK_SEQPACKET. Partial/truncated frames and read/write deadlines are tested.

Ready and Reset are empty-payload control frames on both transports. Private Unix Hello is exactly32 raw nonce bytes; network Hello is bounded JSON. Ping/Pong must work before Ready so the receiver can establish its clock map. Network ordering is State(preparing) -> receiver clock/device validation -> Ready -> State(active) -> Open/Pcm; active state precedes data.

When the receiver/session/device is ready, backend sends an additional JSON control object on the existing connection:
    {"type":"controller-audio-state","ready":true,"generation":G,"producer_epoch":E,"socket":"/private/controller-audio.sock","nonce":"64 lowercase hex characters"}

False state contains ready=false and a fresh generation; no socket/nonce is required. A preparing→active transition retains that generation and current producer epoch; native accepts false→true for the same generation, ignores duplicate true and older generations, and a binding replacement still rotates the generation. Accepted may initially include controller_audio with ready=false. Endpoint lives next to the frame socket in the same0700 directory, socket0600. A producer connects from a worker, checks path/inode owner/type/permissions and SO_PEERCRED UID. The host checks UID AND PID matches its authenticated primary video producer. Unix Hello uses matching generation/epoch and raw32-byte nonce. The host replies Ready only for the current ready lease. On any binding replacement rotate nonce/generation, close old audio connection, clear buffers and resend active-port metadata.

No helper, old peer, unsupported capability or disabled setting means existing rumble/main speaker fallback unchanged.

## Host service, authentication and session binding

Dedicated TLS/TCP at configurable controller-audio port, default base+30 (48189 ->48219). Listen only when kyty_controller_audio=true. Management remains loopback-only. TLS>=1.2, normal OpenSSL primitives, pinned host certificate and mandatory helper client certificate. TCP_NODELAY, bounded asynchronous reads/writes serialized on one executor, handshake/read/write deadlines. No homemade encryption.

The helper generates and retains its OWN private key/certificate on the laptop, files0600/config dir0700. It never receives host management credentials or Moonlight's private key. Host-local authenticated provisioning maps its public cert to one existing enabled paired Moonlight client. Store at most16 receiver records atomically, owner-only. Re-importing the same fingerprint with another parent is rejected. Unpair/disable parent or receiver revokes the lease.

Use exact enabled certificate verification under the existing authorization mutex. Do not use the test-only authorize wrapper, unknown-cert-permissive get_client_status, or asynchronous global last_verified_client_cert. Capture immutable active-session identity and generation from the verified launch/stream record. Receiver may receive PCM only for its mapped RUNNING Moonlight session/controller0 and current native producer. Another enabled client or stale reconnect cannot inherit it. One active receiver per session; reject duplicates deterministically. Never hold authorization/state locks across SSL operations or CUDA work.

Host-local APIs, authenticated using existing local management:
- GET /api/kyty/controller-audio/receivers -> status:true, receivers array max16 of id/name/parent_client_id/certificate_sha256/enabled.
- POST same -> name<=64, parent_client_id, certificate_pem<=16384; return id/fingerprint; parent must exist and be enabled. Same fingerprint+parent is idempotent.
- PUT /api/kyty/controller-audio/receivers/ID -> enabled bool.
- DELETE same/ID -> remove only that receiver.
- GET /api/kyty/status adds optional controller_audio object with configured/state/receiver_name/port/bytes/queue_us/discontinuities/last_error<=128.

Configuration: kyty_controller_audio bool defaultfalse; kyty_controller_audio_port uint16 explicit or base+30; kyty_controller_audio_clients private registry path; private PCM socket derived beside kyty_socket. Only enabling this feature tightens the base-port range for+30. CLI capabilities reports controller_audio protocol_version1/transport tls-tcp, independent of current enabled setting.

## Companion program

Create a native Linux receiver built separately from the emulator. Shared pure codec/conversion may be used within Kyty/helper; host implements independently. Use SDL audio only, no second SDL gamepad owner. A limited hidraw writer accepts only internally constructed USB routing/rumble/reset reports; no network raw-HID passthrough.

Bind the audio endpoint and hidraw through USB ancestry/HID unique ID, never fixed card/node indices. Require one unambiguous 054c:0ce6 wired controller and its4-channel48k playback endpoint; refuse Bluetooth/ambiguous devices with a bounded error and fallback. Select the explicit ancestry-derived hardware PCM through a process-only SDL ALSA playback override after checking its unique SDL name; verify the opened PID-owned selected substream and4 channels. The generic SDL handle with this explicit override is not the system default. Leave system default/profile/microphone unchanged. Laptop currently exposes S16LE/48000/4ch via ALSA, PipeWire and Pulse; native input to SDL remains four-channel float at each source port rate. Dependency installation on laptop is authorized when required.

CLI/config supports identity generation, pinned server, receiver cert/key, controller/audio selection, null sink (never initializes SDL/hidraw), USB sink, bounded status output, and bounded reference playback for hardware proof. Null sink records sample hashes/data only under explicit diagnostics. Background user service waits without opening audio hardware until a ready session; output stops/flushes on lease/producer/device/network loss and process shutdown. Reconnect starts fresh metadata, never queued old samples.

Pair/provision initially via trusted SSH plus host-local API; add launcher enable/status and receiver listing/removal. Do not add SSH remote-execution features to the product. Store credentials privately and never print them. Provide exact removal/rollback instructions.

## Stable code seams inside Kyty/helper

    namespace ControllerAudio {
      enum class Kind : uint8_t { Control=0, Actuator=1, Speaker=2 };
      enum class Type : uint8_t { Hello=1, Open=2, Pcm=3, Close=4, Reset=5,
        Ready=6, State=7, Rumble=8, Ping=9, Pong=10, Error=11, Status=12 };
      struct Header {
        Type type; Kind kind; uint8_t channels;
        uint64_t generation, producer_epoch;
        uint32_t port, rate;
        uint64_t port_generation, sequence, first_frame, source_time_us;
      };
      struct Frame { Header header; std::vector<uint8_t> payload; };
      constexpr size_t HeaderBytes=72, PayloadLimit=16384;
      std::vector<uint8_t> Encode(const Frame&);
      std::optional<Frame> Decode(std::span<const uint8_t>);
      struct PcmView {
        const void* data; uint32_t frames, channels, rate; bool is_float;
        const int* volume; float gain; const float* gains;
      };
      bool ConvertStereo(const PcmView&, std::span<float> output) noexcept;
    }

Producer-only namespace Streaming::ControllerAudio:
    struct Endpoint { std::string path, nonce_hex; uint64_t generation, producer_epoch; };
    void Configure(const Endpoint&, bool ready);
    void Disable() noexcept;
    bool Ready() noexcept;
    void OpenPort(uint32_t id, uint64_t port_generation, ::ControllerAudio::Kind, uint32_t rate);
    bool Publish(uint32_t id, uint64_t port_generation, ::ControllerAudio::Kind,
                 const ::ControllerAudio::PcmView&) noexcept;
    void ClosePort(uint32_t id, uint64_t port_generation) noexcept;

Publish true means accepted by a ready PCM lease; failure atomically withdraws readiness for that lease before restoring fallback. Open/close metadata maintained for active ports and resent on a new generation. Ordinary rumble interception belongs to the host; do not simply suppress all PadSetVibration in native code and lose non-PCM effects.

## Verification and delivery

1. Test-first pure conversion/framing/continuity, bad bounds/NaN/old-generation, gains exactly once.
2. Actual production producer -> private Unix -> TLS broker -> companion null sink with exact per-kind hashes. Wrong/missing cert, wrong parent, disabled/unpaired client, stale session, stalled reader, partial network frames, disconnect/rebind and all cleanup paths.
3. USB sink tests using fake output boundaries for correct4-channel map, exact48-byte routing masks, ordinary-rumble/PCM arbitration, drift/queue bounds, default-output preservation and reset. Then short low-level physical reference clips, automatically ending and restoring routing.
4. Build native, launcher, backend and standalone helper independently; focused existing Qt/controller/audio/stream/GPU and relevant18 pipeline/memory regressions.
5. Fresh v3 install, exact source pins/ELF/map hashes, preserve v2 and latest independent saves/pairing. Install private laptop helper only after silent tests pass.
6. Actual Astro with real source PCM counts and helper playback evidence, gyro/triggers coexistence, no local game window, default speaker unchanged, verified full process cleanup. User confirms physical speaker/native texture. Record any remaining perceptual limits honestly.

Do not implement gyro/adaptive latency tuning in this stage. No public push/PR. Keep worktree/evidence for continued client/TV development.

