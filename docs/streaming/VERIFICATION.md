# Native streaming verification

Date: 2026-10-16, Asia/Kuala_Lumpur.
Status: all planned modules built, reviewed and installed for experimental device testing.
The remaining morning checks require the actual TV/controller and are listed in [README.md](README.md).

## Installed programs

- Command: `kytyps5-streaming`. Application-menu entry: **KytyPS5 Native Streaming (Experimental)**.
- Prefix: `/home/ali/.local/opt/kytyps5-streaming/20261016-native-v1`.
- Private configuration: `/home/ali/.config/kytyps5-streaming/20261016-native-v1`.
- Initial preference: 1080p, 60 FPS, HEVC, 30 Mbps; base port 48189, management 48190.
- Kyty runtime code: `d1d262cfacdce8397e68bab1d94fdc1a964166de`.
- Independent streaming host: `1cceda1eafa97e26bab3084cf36d8daac8515b7a`.
- The final documentation/test-registration commit changes no installed program source.
- Source branches and the managed worktree are retained locally. No public push or PR was made.

The two programs build independently. The host is based on Sunshine
019b1ba661d829e334706557a3e2bdc3d2374591 and uses CUDA architecture 86,
with its browser frontend and tray disabled. The build script enforces the exact
host revision and clean, initialized recursive dependencies.

## Verification matrix

| Check | Result | Evidence |
|---|---|---|
| Emulator, native launcher, launch wrapper and independent host | Release builds pass | Build/staging logs and installation provenance |
| Protocol, real Vulkan/CUDA export, audio selector, controller state, launcher | 5 CTest groups pass, 61.17 s; launcher includes 31 cases | `final-ctest.log` |
| Backend protocol, listener lifecycle, GPU ownership, input, pairing and configuration | 63 focused tests pass, 0.969 s | `final-backend-tests.log` |
| Pinned source/dependency enforcement | 13 real Git-fixture tests pass | `tools/streaming/test_build_pin.py` |
| Independent save/config copy and overwrite refusal | Pass | `final-package-tests.log` |
| Existing memory and pipeline regressions | 18 tests pass, 8.75 s | `final-regressions-green.log` |
| Prefetch modes with explicit preparation setup | Inline and parallel cases each pass 10 repetitions | `prefetch-fixed-repeat.log` |
| Real RTX 3090 external-memory/semaphore import | 1080p and 1440p pass; pitch, alpha, orientation, three-slot saturation/reuse and skipped frames | Task 1/2 GPU reports |
| NVENC output independently decoded with FFmpeg | Expected blue image, RGB center [0, 0, 241] | `gpu-nvenc-decode.json` |
| Stock Moonlight protocol | Pair, catalog, H.264 video/audio; HEVC 1440p output request and same-producer reconnect pass | Loopback logs and GPU snapshot counters |
| Private audio isolation | Real tone capture and actual game PCM; global default unchanged | Audio reports and final Astro result |
| Killed host and stale private socket | Same-path restart returns waiting | `stale-socket-result.json` |
| Killed host, stubborn game descendants and UI restart | Owned group exits in 6.104 s; ownership held in 152 live-group samples; same-profile second launch passes | `crash-recovery-20261015T181944Z-787353/postcheck.json` |
| Final real Astro boot through the installed UI and stock client | Pass; decoded 1080p picture, game audio and clean shutdown | `final-astro-postcheck.json` |

The focused checks are the changed-module and relevant regression suites; this is
not a claim that every repository test target was built or run. Synthetic network
fixtures used Moonlight's fake output platform. The real Astro test additionally
used the unmodified client's SDL/software-FFmpeg decoder and inspected its picture.
Test-only frame readback and Qt adapters are retained as QA artifacts and are not installed.

## Final actual-game result

The installed launcher started its own host; stock Moonlight paired, read the real
ASTRO BOT catalog and launched the packaged guarded emulator. The stream used
1920×1080 capture and HEVC output, with 797 GPU snapshots and
0 dropped exports at the sample point. Decoded frame 600
shows the game's Team Asobi logo and starfield. Counts are not a frame-rate or
latency benchmark. Private game audio contained 4343040
bytes of signed 16-bit stereo PCM, RMS 713.37; Moonlight
received both encoded video and audio packets.

The launcher exited 0. Native crash-capture metadata records exit 0 and a completed
end timestamp at `/home/ali/.local/opt/kytyps5-streaming/20261016-native-v1/_CrashCapture/20261015T182908.528853Z`. The native executable SHA matches the
installed provenance. Every experimental executable/guard exited; only the original
system PipeWire/WirePlumber processes remain. Normal save hashes and audio default
`auto_null` stayed unchanged, and ordinary Sunshine continued running.

## Issues found and resolved during verification

The whole-branch review found two shutdown/restart defects and one build-reproducibility
gap. Verified stale-socket recovery, per-host nonce/pidfd supervision with a group
guardian, and strict backend source/dependency checks resolve them. The scoped final
review approved all three fixes with no new findings. Native tests and actual packaged
SIGKILL/restart testing cover their failure paths.

A pre-existing pipeline test assumed a worker always prepares a cold draw before
the command processor. The unchanged executable passed twice then failed while
its image readbacks were correct. Baseline-identical sources and ten inline passes
confirmed the scheduling assumption. Its registration now selects inline preparation
for the required prefetch-consumption assertion; the companion test explicitly
retains parallel preparation. Assertions and renderer behavior are unchanged. A fresh
fixture rebuild, all 18 regressions and ten repetitions of both modes pass.

The expanded test-only Qt adapter initially polled a widget tree during nested
destructor event processing. A core trace and CPU-only reproducer identified the
adapter callback. Its timer now stops before quit, is bound to the window, and guards
window access. STOP, fallback and external-quit reproductions all exit 0; the final
real Astro repeat also exits 0. No product change was needed for that adapter defect.

## Preserved state and limitations

The normal `kytyps5` command, native binary and Sunshine configuration retain their
original SHA256 values. The ordinary `/usr/bin/sunshine` process remains active.
Experimental saves/cache/trophies are independent copies with no automatic writeback.
Older experimental ELF/map/provenance versions are kept under the prefix's
`verification/build-*` directories for existing crash captures.

Physical TV gameplay, subjective latency, controller compatibility, advanced haptics,
audible physical-speaker playback, and local 4K/maximized/minimized-window behavior
remain device checks. The real Astro result is a boot/decode test, not a gameplay
playthrough. Actual 1440p Astro gameplay has not been tested; native 1440p GPU export
and a synthetic 1440p negotiated output were tested separately.

Guest VideoOut layers and game HUD are captured. Kyty's separate IME, sign-in/error
panels and trophy toasts remain local-only; remote input does not operate those
panels. This edition also has a bounded two-retired-plus-one-active GPU-pool limit,
SDR/H.264/HEVC/stereo/single remote player, and catalog refresh on host restart.
These limits and the morning checklist are documented in the operator guide.

Detailed artifacts live at `_Build/astro-evidence/streaming-20261016/` in the managed
worktree. Task briefs/reports and scoped review artifacts are retained under
`.superpowers/sdd/2026-10-16-kyty-native-streaming/`. Large generated evidence is local
and is not added to Git.
