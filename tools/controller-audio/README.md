# Controller audio companion (Linux USB)

This standalone receiver takes the frozen KCPA v1 stream from the authenticated host and either verifies the PCM silently or sends it to a wired DualSense. It uses SDL **audio only**; it never owns a gamepad, changes the system output, selects a Pulse/PipeWire profile, or enables a microphone. Ordinary game audio continues through Moonlight's existing output.

## Build and silent verification

Required: C++20 compiler, CMake, OpenSSL development headers/libraries, SDL3 >=3.2 development headers/libraries, Linux ALSA kernel headers. Python3 and the `openssl` CLI are used only by tests. The repository supplies the JSON and pure KCPA codec sources; the helper does not depend on the emulator or graphics libraries.

```sh
cmake -S tools/controller-audio -B _Build/controller-audio-receiver -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build _Build/controller-audio-receiver -j2
ctest --test-dir _Build/controller-audio-receiver --output-on-failure
```

The tests use pure/fake USB output, actual SDL memory streams plus a forced-and-asserted **dummy** driver, and an actual local mTLS server plus the production receiver **null** mode. The SDL test refuses every device open unless the active driver is dummy; it measures virtual callback startup without treating it as a real hardware timing guarantee. A preloaded test guard aborts if null mode initializes SDL or opens `/dev/snd/*` or `/dev/hidraw*`. These commands do not play audio or open controller hardware. They do not prove physical USB playback.

## Private receiver identity and host registration

Generate the key on the machine that will run the receiver:

```sh
controller-audio-receiver --generate-identity "$HOME/.config/kyty-controller-audio/identity"
```

The new identity directory must not already exist. It contains `key.pem` and `cert.pem`, both mode0600, with the directory mode0700. The command prints only the public certificate path and DER SHA256 fingerprint. Register **cert.pem only** through the host's authenticated local receiver API with its enabled paired Moonlight parent. Do not move this private key to the host, copy Moonlight credentials, or give the helper host management credentials.

Create an owner-only mode0600 JSON file in a mode0700 configuration directory:

```json
{
  "host": "192.0.2.10",
  "port": 48219,
  "pinned_host_sha256": "64_lowercase_hex_characters_from_the_trusted_host_certificate",
  "cert": "/home/USER/.config/kyty-controller-audio/identity/cert.pem",
  "key": "/home/USER/.config/kyty-controller-audio/identity/key.pem",
  "sink": "null"
}
```

Replace the example pin with the exact SHA256 of the host certificate's DER bytes obtained through a trusted provisioning channel. Host certificate validity dates are checked. `host` is a numeric IPv4/IPv6 address so DNS cannot escape the bounded connect deadline. `receiver_id` is optional: it is always derived from this receiver certificate's DER SHA256, equal to the host API's receiver ID. A supplied value must exactly equal that64-character lowercase fingerprint; malformed or stale overrides are rejected before connecting. Optional `usb_path` selects a USB ancestry path or basename (for example `3-1`); optional `audio_name` must exactly match the ancestry-verified SDL ALSA endpoint name. Do not configure card numbers or hidraw indices.

```sh
controller-audio-receiver --config /private/config.json --status
controller-audio-receiver --config /private/config.json --sink null --run-seconds 10
controller-audio-receiver --config /private/config.json --sink null --run-seconds 10 --diagnostics --raw /private/new-capture.kcpa
```

`--status` prints bounded local configuration state and opens no hardware. Normal null operation keeps byte counts only. Explicit `--diagnostics` prints one payload SHA256 and byte count for each kind at exit. `--raw` also requires diagnostics and null mode; it creates a new mode0600 file exclusively and refuses overwrite/symlinks. Its bytes are concatenated **full KCPA Pcm frames**, preserving kind, source rate, port identity, source timestamp and exact little-endian float payload. Hashes cover the unmodified payload bytes in per-kind arrival order. Diagnostics accumulate across leases; queued playback never does.

Without `--run-seconds`, the process waits/reconnects until SIGINT/SIGTERM, suitable for a user service. It does not open audio/hidraw while waiting for a server/session. Connect/handshake/write/partial-frame deadlines are3s/5s/1s/2s; Pong/idle-read expire at6s/10s. Retry backoff is250ms..5s. Error text is bounded and rate limited. All TLS I/O runs on the helper thread, with TLS>=1.2 and its own client certificate. No raw network HID payload is accepted.

## USB output and later hardware proof

**USB commands are physical output commands. Run them only at the separately coordinated hardware boundary.**

```sh
controller-audio-receiver --config /private/config.json --sink usb --run-seconds 20
controller-audio-receiver --config /private/config.json --sink usb --reference-seconds 1 --reference-kind speaker
controller-audio-receiver --config /private/config.json --sink usb --reference-seconds 1 --reference-kind actuator
```

Normal USB output opens only after authenticated `State preparing`, a fresh Ping/Pong clock map with <=20ms uncertainty, and validated output. `Ready` has empty payload and echoes the proposed lease/epoch; `State active` must precede Open/Pcm. Reference mode is an explicit local diagnostic, bounded to1..3s, using a0.02-amplitude220Hz tone and5ms source blocks. It needs no live host connection, but reads the same private config and explicit USB selection. SIGINT/SIGTERM, exceptions, deadline completion and normal shutdown flush/close streams, stop helper rumble, and restore helper-owned speaker routing.

The USB binding walks sysfs to one selected wired054c:0ce6 parent, its single hidraw node, and its single playback endpoint. ALSA control **read** ioctls identify the exact name SDL's ALSA backend enumerates. SDL is forced to ALSA for this process and first requires one exact ancestry-verified SDL endpoint name. It then sets the process-only `SDL_AUDIO_ALSA_DEFAULT_PLAYBACK_DEVICE` hint to `hw:CARD=<verified-card-ID>,DEV=<verified-device>` and opens SDL's generic playback handle, which now targets that exact hardware PCM. The card ID comes from the selected USB card's read-only control metadata and is restricted to a safe ASCII identifier; no global ALSA config or system default is changed. This avoids SDL guessing an incompatible `sysdefault:...DEV=...` name on some distributions. It requests4-channel48000Hz output and checks the negotiated format **after opening** (unopened SDL ALSA devices have a placeholder stereo format). Only complete `pcmC<number>D<number>p` playback node names are considered; capture nodes ending in `c` are excluded before association. Multiple matching HID/playback endpoints or duplicate SDL names fail closed. Preparation failures print their stage and bounded reason (at most one diagnostic every2s), covering discovery, SDL initialization/name/open/format, owned ALSA parameters/progress, hidraw identity/permissions and final liveness. Runtime diagnostics distinguish PCM queue, hardware/proc liveness, SDL frequency ratio/data submission, HID writes and servicing failures. A PCM queue-bound failure also snapshots the owned ALSA state/hardware/application pointers and actual SDL queued bytes, input format and frequency ratio. A failed reference identifies the source frame and whether submission or subsequent servicing failed; it stops and cleans up immediately. Bluetooth is unsupported and falls back through the host lease policy. The desktop session must have existing permission to the selected ALSA/hidraw nodes; this program changes no permissions, services, kernel modules or mixers.

Each source port remains four-channel float at its own source rate; SDL performs the final endpoint conversion/resampling. Speaker stereo goes front-left/right, actuator stereo rear-left/right. Input queues are capped at20ms per port with an explicit lease failure rather than silent dropping. The frequency-ratio servo is clamped0.995..1.005, targeting15ms queued input; pure tests simulate ten minutes at±1000ppm. Conservative delivery age includes clock uncertainty, the **verified actual ALSA ring buffer**, and one SDL mix period, with80ms total deadline. After opening its own SDL endpoint, the helper reads its owned `/proc/asound/cardN/pcmDp/sub*/hw_params` and `status`: owner PID,4chS16_LE48k format, SDL period match, actual `buffer_size`, and monotonic hardware pointer are required. More than two negotiated periods are accounted for; missing/ambiguous parameters or a ring-plus-mix bound over40ms refuse readiness. The indices come from the verified USB ancestry.

At most4096 delivery records are retained across all ports. During each service tick, a record first waits until SDL has consumed its source frames, then remains pending until the verified hardware pointer advances through the conservative ring-plus-mix bound. Tick enforces its source deadline even if no later PCM arrives. A healthy-but-stalled software queue or hardware pointer revokes/flushed playback before old queued data remains eligible; a drained stream with observed hardware progress retires its records. Device counter regression or unavailable progress also withdraws readiness. Before returning usable USB preparation, a separate condition-based gate waits at most500ms for two verified owned RUNNING observations with increasing hardware pointer and positive, nonregressing application pointer. SDL supplies silence with no bound source streams while this clock starts; the helper submits no source PCM and does not acknowledge Ready until the gate passes. It polls at the negotiated period (clamped1..5ms) against the absolute deadline, fails closed/cleans up on missing, closed, regressing or never-started output, and leaves the20ms software and80ms submitted-source limits unchanged. This preparation budget fits the broker's5s idle/Ready wait; it is not an80ms source-age exemption after samples are accepted. These are bounded checks, not a hard real-time operating-system scheduling guarantee.

Full48-byte USB reports have reportID02, zero padding and only the audio-routing or ordinary-rumble fields. They never enable trigger/LED writes. Speaker routing is restored on last speaker close/reset. Classic ordinary rumble uses SDL's half-scale legacy emulation, expires within the supplied0..1000ms, and is cached while audible actuator PCM suppresses it. As in native Kyty, audible PCM means |sample|>1/1024 and holds suppression for at least250ms. A speaker mode change reapplies this cache/arbitration so a short non-PCM effect is not lost.

## Removal and rollback

Stop the receiver/user service and disable/remove this receiver through the host's authenticated local receiver API. Disable host controller audio to restore the existing Moonlight rumble/main-speaker fallback. Remove only this helper binary, its private configuration/identity directory and any explicit diagnostic captures. Preserve Moonlight pairing, default audio settings, the stable v2 installation, and game saves. Service/install/provision wrappers are maintained separately by the parent integration task.

## Verification limits

Local tests prove framing, exact float transport/hash, the companion mTLS handshake, pin rejection, private files, lease/port/sequence/deadline handling, logical USB channel/report/arbitration behavior and drift bounds. They do not prove the production host authorization registry/session binding (tested by the host integration), actual ALSA device availability/exclusivity and owned proc parameter/progress support, actual four USB channel sensations, speaker volume or coexistence with Moonlight's HID writes. Physical reference clips and a supervised game remain required before claiming native texture/speaker parity.
