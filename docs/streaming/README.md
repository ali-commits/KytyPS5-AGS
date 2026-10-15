# Kyty native streaming experiment

This Linux/RTX 3090 edition sends Kyty's composed guest game image to a separate
Sunshine-derived Moonlight host. Capture happens before local window scaling.
Your Intel-connected monitors can continue to show the normal desktop. No display
capture, dummy display connector, or desktop input injection is used by this mode.

The native launcher owns hosting, quality settings, pairing, clients, game launches
and private audio. Kyty and the host remain independently built programs in separate
repositories. This is a private experiment, not a supported upstream release.

## Start and pair

Run `kytyps5-streaming`. The experimental launcher has **Library** and **Streaming**
tabs. The library and game settings start from a copy of the working installation.

1. Open **Streaming** and use **1080p**, **60 FPS**, **HEVC**, and the default 30 Mbps
   maximum for the first TV test. Leave the NVIDIA adapter field empty for RTX 3090.
2. Select **Start hosting** and wait for **Ready**. Copy the displayed LAN address,
   including its port. The default base port is 48189, separate from system Sunshine.
3. In Moonlight, manually add that address, for example `192.168.1.3:48189`.
   Start pairing. Select that pending device in the launcher, enter Moonlight's
   four-digit PIN and select **Pair**.
4. Choose **ASTRO BOT** from Moonlight's game list. Moonlight launches the game
   through the packaged guard and the same argument builder used by local play.
5. Use **Stop game** to end the session, then **Stop hosting** when finished.

The TV and PC need to be on the same LAN for this first test. Moonlight must support
a custom host port; if a particular TV client does not, the launcher also exposes
the base port setting. Start with SDR and HDR disabled. H.264 is available if that
TV has trouble with HEVC. AV1 is disabled for this RTX 3090 edition.

Match Moonlight's resolution/FPS settings to the capture preference. The preference
sets the exported image size; it does not change the game's internal rendering.
Scaling a 1080p source to 1440p does not create additional native detail. Moonlight
chooses the final encoded output, and the launcher labels requested session values.

While hosting, local Library launch is disabled so Moonlight owns the one game
session. Stop hosting to launch locally. A global emulator guard also prevents
normal and experimental Kyty games from running together accidentally.

## Audio, input and state

Game audio uses an explicitly selected private SDL/Pulse output. The host captures
only that sink's monitor. A real selected system playback device can receive an
owned local-monitor loopback when local playback is enabled. The system default
sink is never set or restored by this edition.

On this machine's current automatic Dummy Output, the launcher instead starts a
private PipeWire graph and policy-only WirePlumber process. That graph contains no
physical audio devices and does not change the system's Dummy Output. TV audio
remains available; Dummy Output cannot provide audible local playback. The launcher
owns and stops those private processes.

Remote gamepad state goes directly into Kyty's controller state. Disconnect releases
remote-owned buttons/axes while preserving local held buttons. Basic rumble and a
bounded vibration-PCM fallback are implemented. Advanced DualSense haptics, adaptive
triggers, touch/motion and physical TV controller behavior need device testing.
Keyboard and mouse forwarding are intentionally ignored by the native host.

Installation: `/home/ali/.local/opt/kytyps5-streaming/20261016-native-v1`.
Private settings: `/home/ali/.config/kytyps5-streaming/20261016-native-v1`.
The normal `kytyps5`, `astro-bot`, system Sunshine and their settings remain separate.

`_SaveData`, `_PipelineCache`, trophies and other writable game state are independent
copies inside the experimental installation. Progress made here stays here. There
is no automatic writeback into the working saves. The package records the source
runtime, both source revisions and executable/map SHA256 values in
`installation-provenance.json`. Each actual game launch also records terminal output,
settings, process mappings and the native executable hash under `_CrashCapture`.

## Automated verification and limits

- Real RTX 3090 Vulkan memory/semaphore exports imported into matching CUDA UUID;
 1080p and 1440p, aligned pitch, asymmetric colors/orientation, guest HUD alpha,
 three-slot exhaustion/reuse and dropped-frame semaphore consumption checked.
- Backend snapshot ownership tested after 30 producer-slot overwrites. Its H.264
 NVENC output independently decoded by FFmpeg to the expected blue image.
- Unmodified Moonlight Embedded paired, listed apps and received H.264 video/audio.
 HEVC with a 1440p output request also streamed and reconnected; actual GPU snapshot
 counters increased across both sessions. That network fixture used a 1080p source;
 native 1440p GPU export was tested separately.
- Real private-audio tone capture checked the system default before/during/after.
 Native Qt tests cover pinned TLS, incorrect certificates, pairing/client API
 payloads, settings, arguments/quoting, process guards, host errors/restart, audio
 ownership, delayed setup failures and cleanup during shutdown.
- A real packaged launcher→Moonlight catalog→Astro boot was also decoded by the
 stock client's SDL/software-FFmpeg platform. The final 1920×1080 check exported
 797 frames with no dropped exports; decoded frame 600 shows the Team Asobi logo
 and starfield, game audio was nonzero, and normal shutdown completed both the
 capture manifest and launcher with exit 0. Normal saves and the system audio
 default stayed unchanged.
- A real packaged host SIGKILL test retained audio/session/profile ownership until
 the complete game group exited, then restarted the same profile/socket and launched
 a second catalog entry. No owned descendants or zombies remained.
- Existing 18 memory/pipeline regression checks pass. Full launcher Library and
 Streaming windows were rendered offscreen and inspected.

The synthetic network tests use Moonlight's fake platform; the real Astro boot
check additionally verifies offscreen decoded pictures. Neither substitutes for
viewing on the actual TV. Gameplay, subjective latency, physical speaker playback,
controller compatibility and minimized-window behavior still need device testing.

The capture contains guest VideoOut layers. Kyty's separately rendered system
panels—IME keyboards, sign-in/error prompts and trophy toasts—remain local-only
in this first edition, and remote input does not operate those panels. A game
waiting on one of these needs interaction at the PC. Existing login, save-data
and message dialogs auto-complete in the emulator; they are not additional hidden
interactive panels. This limitation does not claim full PS5 system-UI parity.

A slow/disconnected consumer cannot make the producer wait for a free capture slot.
For safety, the first edition retains at most two old GPU pools plus one active
pool. Repeated producer reconnects or capture-size changes beyond that limit disable
capture until the game is restarted. A Moonlight viewer reconnect to the same live
producer does not itself replace that pool. A driver hang can require killing the
helper; it is not a renderer-stability guarantee.

The app catalog is a snapshot taken when hosting starts. After changing the Library,
stop and restart hosting to publish the updated catalog. There is no automatic
software update or installation over the working edition.

A per-host guardian tracks the exact host identity. If the host or launch wrapper
crashes, it terminates the complete owned game group, including descendants that
ignore normal shutdown. The launcher holds audio and profile ownership until that
group has exited, then allows a same-profile restart. Recovery may take about six
seconds while a stubborn process is killed. Stale private frame sockets from a
killed host are recovered without removing another live listener.

## Morning TV test

1. Start at 1080p/60 and play a short Astro section. Check movement, jumping, menu
   buttons, audio timing and basic rumble using the controller connected to the TV.
2. Resize or maximize the local game window, then minimize it. Confirm the TV keeps
   receiving the game while the Intel monitors remain usable.
3. Disconnect and reconnect Moonlight to the running game. Confirm picture, sound
   and controller release/reconnect without restarting the emulator.
4. Stop the game and hosting, select 1440p, restart hosting and match Moonlight's
   resolution. Compare detail, frame pacing and latency on the TV.
5. Stop the game normally and confirm the last crash-capture manifest records an
   exit. Keep any failing session's `_CrashCapture` directory for diagnosis.

Changes to experimental game progress stay in the experimental save copy. If an
emulated system prompt appears only on the PC, complete it locally for this edition.

## Build and inspect

Kyty branch: `experiment/kyty-native-streaming` in the attached managed worktree.
Host branch: `experiment/kyty-frame-input` in
`/home/ali/projects/kyty-streaming-sunshine`, based on Sunshine 019b1ba661d829e334706557a3e2bdc3d2374591.

`tools/streaming/build.sh` builds the two programs independently and stages Kyty.
It requires the existing Clang/CMake/Ninja/Qt6, Vulkan, CUDA/NVENC, PipeWire/Pulse
and upstream Sunshine dependencies. It verifies the exact backend commit in `host-source.rev`, checks clean recursive
submodule revisions, builds CUDA architecture 86 and omits the
Sunshine browser frontend/tray. No JavaScript package installation is required.
Set `KYTY_STREAM_BUILD_DIR`, `KYTY_STREAM_HOST_SOURCE`, `KYTY_STREAM_HOST_BUILD`,
`KYTY_STREAM_PREFIX` and a fresh `KYTY_STREAM_STAGE` to override local paths.

`tools/streaming/package.py --help` describes the fresh-prefix package inputs.
Only the dedicated `kytyps5-streaming` command can be installed by that script.
It refuses existing destinations and refuses to snapshot saves while an emulator
is running. `tools/streaming/test_package.py` checks state isolation/refusal.

`tools/streaming/moonlight_smoke.py --help` runs a private loopback host using an
existing private sink and stock Moonlight client. It records source binary hashes,
packet delivery and actual GPU capture counters, and can exercise reconnects.
The Vulkan test producer supports `streaming_gpu_tests --serve <private-socket> 60`.

The binding wire contract is [PROTOCOL.md](PROTOCOL.md); the implementation spec and
plan are under `docs/superpowers/`. [VERIFICATION.md](VERIFICATION.md) records the
final checks, source revisions and remaining device tests. Detailed local evidence
and the package provenance are retained with this worktree and installation.
