# Native AGS owner diagnostics

Open the existing Streaming tab and select **Native AGS diagnostics**. This dialog
connects to the existing launcher daemon over its owner-only `control.sock`. It
cannot start a separate worker, select another profile, or write `host.json`.
Closing the dialog leaves hosting unchanged.

Private/headless versus mirrored presentation, independent local audio/sink,
0–600 second disconnect grace (default 20), encoder selection and FEC apply to the
next game through the daemon. The main Streaming tab also exposes output size,
frame rate, bitrate and codec. The diagnostics view separates active endpoints,
configured next-launch settings and the captured active-session configuration.
Hardware-dependent encoder/input features are reported by the native owner.

Pairing exports a short-lived pinned one-use bundle through the active owner.
Only the owner-only file path is shown; transfer the file through a trusted local
channel and import it within 120 seconds. Revoke uses the displayed device's stable
UUID. Neither settings nor revocation serializes a UI snapshot of device secrets.

Launcher preferences remain in `StreamingSettings::ProfileDirectory()/streaming.ini`.
The native profile is its `native` subdirectory. Existing legacy pairing files and
per-device launch preferences stay untouched. The worker resolves each game from
the daemon's library, preserving complete device-specific arguments and cwd.

Standalone dialog tests use a fake owner socket and verify settings requests,
UUID revocation, pairing export, safe status, and the absence of process ownership
or direct configuration writes:

```
cmake -S src/launcher/tests/rust-streaming -B /tmp/ags-rust-controls-build -G Ninja
cmake --build /tmp/ags-rust-controls-build
ctest --test-dir /tmp/ags-rust-controls-build --output-on-failure
```

Run from the emulator project root. Native runtime, registry, lifecycle and
configuration tests live in `streaming/ags` and the launcher streaming/host suites.
These automated fixtures do not demonstrate physical controller or gameplay parity.
