# Temporary Rust streaming controls

This is an additive testing surface in the existing GPL launcher. New streaming
implementation remains in the separately licensed `streaming/ags` workspace.

Open the existing Streaming tab and select **AGS Rust streaming — test controls**.
Choose the built `ags-host`, select a separate profile directory, and initialize it.
Set a reachable listen address for LAN tests (the Rust default is loopback only).
The panel edits video quality/codec, backend, presentation, local audio, and grace.
Settings apply on the next host start. Closing this panel stops its owned host.

**Import current launcher library** obtains the launcher's current normal emulator
catalog. Paths remain absolute and arguments are passed as an argument array, not a
shell command. No Sunshine streaming-only export flags are generated. Each game is
assigned a fresh working directory under PROFILE/kyty-test-runtime/GAME-ID. Its
_SaveData and caches are separate from normal play. No existing saves are copied,
no settings in Kyty.ini are rewritten, and import does not launch a game.

Before the morning test, the operator must prepare required runtime assets in the
new per-game working directory. Copy only the required asset folders from the
runtime installation; leave _SaveData fresh unless deliberately testing a separate
copy. The source ROM/game/addon files stay at their existing read paths. This
importer does not establish game compatibility or sandbox arbitrary game-file writes.
Save settings to publish the imported catalog, then start the Rust host.

**Create pairing bundle (120 s)** invokes `ags-host pair-ticket PROFILE 120` and
shows only the private bundle file path. Share that owner-only file through a trusted
channel to Android/the Linux pairing client and verify the host certificate. It is
short-lived and one-use. Saving controls reloads the current private device registry
so newly paired devices are not overwritten by a stale UI snapshot.

Standalone tests compile only the dialog and exercise atomic private config writes,
symlink refusal, catalog path/save isolation, pairing-export process invocation,
registry preservation, secret-free UI text, and offscreen rendering:

```
cmake -S src/launcher/tests/rust-streaming -B /tmp/ags-rust-controls-build -G Ninja
cmake --build /tmp/ags-rust-controls-build
ctest --test-dir /tmp/ags-rust-controls-build --output-on-failure
```

Run these from the emulator project root. The full launcher target has also linked
successfully in `/tmp/ags-full-launcher-build`. Personal games were not launched.

Packet loss recovery is an optional experimental checkbox, off by default. It maps
`fec_enabled` without replacing other profile fields. The current small-group XOR
scheme uses approximately 25% extra media bandwidth and introduces a short group
wait; H264/HEVC byte chunks and PCM are covered. AV1 access units are transported
independently but are not covered by this parity mode yet. Qt tests cover false and
true round trips while preserving unrecognized fields and current device tokens.

Audio codec selection is exposed as Opus (low latency) or PCM (uncompressed). Old
profiles missing `audio_codec` load as `raw_pcm`; newly initialized host profiles
select their configured default. The combo writes only that setting and preserves
paired-device records and unrecognized profile fields.

Settings Save and Rust pairing now share the stable owner-only `.pairing.lockfile`
using the same kernel flock semantics. Save holds it from the latest profile read
through atomic replacement. Busy pairing yields a retry message, and the lock file
is never unlinked. Tests prove blocked Save leaves the file untouched, then succeeds
with fresh devices/new fields preserved after lock release. The current suite has
11 QtTest passes including init/cleanup; the full launcher links successfully.
