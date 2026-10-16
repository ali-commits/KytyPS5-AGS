# Streaming-only and DualSense verification

Date: 2026-10-16, Asia/Kuala_Lumpur.
Status: implementation reviewed and candidate installed; laptop streaming-only video verified.
Controller hardware acceptance and a later fatal native exit remain unresolved.
No further game test will run without explicit user permission.

## Candidate and rollback

- Candidate: `/home/ali/.local/opt/kytyps5-streaming/20261016-native-v2`.
- Candidate settings: `/home/ali/.config/kytyps5-streaming/20261016-native-v2`.
- Kyty runtime code: `3c624be545b000022116502d7aa36c107bfc99a4`.
- Native SHA256: `f225e03e2839630b316a6c616a5b31b1f26dc85793b78622998856ab065afc1a`.
- Matching map SHA256: `0f0f26bf399a34b5c408b777c63daa87ff730f8774505961320721d608ea13ce`.
- Host code: `f35169866ddf9824a6a580abf0524bec694baede`; pinned by Kyty commit `598670dc`.
- Host SHA256: `d65260ae0b779b07565f3784c4bcd9b7dfe2edcc4a0bfd4273e15c30852a46e1`.
- The `kytyps5-streaming` command still selects the working `20261016-native-v1` until the remaining acceptance checks finish.
- Working v1 and normal Kyty/Sunshine installations remain separate; no public push or PR.

The candidate copied the latest v1 experimental saves, cache and trophies into
independent writable state. Neither edition writes back into the normal game state.
Source revisions and executable hashes are recorded in `installation-provenance.json`.
Crash capture remains enabled with a matching native map.

## Current feature checks

| Check | Result | Evidence |
|---|---|---|
| Full emulator, launcher and protected wrapper release build | Pass | `/tmp/kyty-stream-only-full-build.log` |
| Full launcher suite, including migration, stream-only env and wrapper validation | 33 cases pass, 59.75 s | `/tmp/kyty-stream-only-full-qt.log` |
| Protocol, presentation policy, real GPU export and controller state | 4 CTest groups pass, 0.89 s | `/tmp/kyty-stream-only-final-core.log` |
| Audio output selection | Pass | `/tmp/kyty-stream-only-final-audio.log` |
| Existing memory/pipeline regression suite | 18 pass, 8.94 s | `/tmp/kyty-stream-only-regressions.log` |
| Backend motion/effect bridge, reconnect ownership and standard Moonlight packets | 47 focused tests pass, 2.026 s, including real GPU/IPC regressions | Task 2 report and reconnect logs |
| CUDA-disabled backend compilation boundary | Actual input/capability/test translation units compile without Kyty symbols | Task 2 report |
| Actual packaged streaming-only Astro boot | 776 GPU copies, zero dropped exports, 1080p decoded picture/audio, no local game window | `headless-loopback/` and strict postcheck |
| Production SystemOverlay through Vulkan/CUDA export | 57 frames, 45 overlay recordings; dialog acceptance and numeric IME input pass | `dialog-proof-2/result.json` and PNGs |
| Stock laptop Moonlight 6.2.0 video/audio | Actual Astro title decoded; 3,391 client presents; no local game window | `laptop-proof-v2-video-1-final/`, `laptop-v2-host/` |
| Physical laptop DualSense sensors and trigger resistance | Access denied: 0 sensor samples/HID writes; 3,718 SDL effect attempts failed | Laptop observer/result and access proposal |
| Native stop boundary after laptop disconnect | Fatal guest error, native exit 65; cause relative to user Stop unknown | Preserved crash capture and timeline below |

Detailed new evidence lives under
`_Build/astro-evidence/stream-only-dualsense-20261016/`; reports and reviews live under
`.superpowers/sdd/2026-10-16-stream-only-dualsense/`. Generated evidence is local,
not added to Git. Focused checks cover changed modules and relevant regressions;
this is not a claim that every repository target was built or run.

The real Astro check used the packaged launcher/guard and stock Moonlight Embedded
SDL/FFmpeg decoder. Frame 600 shows the Astro starfield. Private stereo PCM contained
4,377,600 bytes with RMS 417.29; the launcher exited 0. A KWin query found no window
for the live native game PID. These counts are not an FPS or latency benchmark.
The original QA aggregate retained a false result because it expected yesterday's
normal Sunshine PID. A separate strict postcheck records the actual feature result
and confirms normal Sunshine remained absent, as it was before this morning's work.

The overlay fixture uses actual production SystemOverlay, controller handlers,
Vulkan rendering and CUDA export with synthetic remote input. It accepted an error
dialog with Cross, prevented that press from reaching the guest, inserted `1` into
the numeric IME, navigated to Done and accepted. Closing the panels restored exact
baseline pixels. This is rendering/input proof, not a physical controller claim.

## Review findings

Presentation review found two modal-input ownership/wakeup races. Production-helper
regressions reproduced both before the fixes; the scoped fix review approved
`3c624be5`. Launcher review approved the default-preserving setting and protected
launch environment. Controller review found one reconnect case where motion resumed
before the new producer received connected controller state; the focused fix and
review passed. The bounded authenticated snapshot is replayed before motion;
controller removal, session detach and replacement owners clear it. The final broad
feature review reports SPEC PASS for implementation and QUALITY APPROVED, with no
new code findings. Hardware acceptance and the runtime fault below remain open.

## Actual laptop result and disconnect timeline

The final host served the user's unmodified Fedora Flatpak Moonlight 6.2.0 with
copied client settings and existing pairing. The actual 1920×1080 HEVC picture
shows Astro's title/characters/ship in `laptop-proof-v2-video-1-final/client-frame-900.png`.
The host's 175 live-viewer samples reached 3,311 copied frames with zero dropped
exports. Drops after the viewer left are recorded separately and are expected when
the producer continues without a consumer. Private audio contained 5,798,400 bytes
of stereo PCM, RMS 1141.20. KWin found no window for native PID1334315.

The original Moonlight process/configuration stayed unchanged. The laptop's actual
default audio node remained the built-in Speaker51. A raw QA comparison initially
reported false because ALSA activation added unrelated latency/headroom properties;
the separate preservation audit compares stable node identity and retains the raw
result. No audio settings or controller permissions were changed.

The observer recorded 3,718 adaptive-effect SDL calls, all failing; zero raw-HID
writes and zero motion samples were observed. This establishes network delivery
to Moonlight, not physical adaptive-trigger operation. Current permissions block
the required raw controller path.

The automated client used a 90-second deadline and `--no-quit-after`. This was
confusing during the user's manual play: disconnecting that client deliberately
left Astro running. The saved chronology is:

| Event | UTC | Local time (MYT) |
|---|---|---|
| Timed client disconnect, followed by orderly client cleanup | about 03:28:34 | about 11:28:34 |
| Final terminal write, including fatal guest fault | 03:29:08.445758 | 11:29:08.445758 |
| Native process ended with exit65 | 03:29:08.883222 | 11:29:08.883222 |
| Root test-launcher stop marker | 03:29:22.158058 | 11:29:22.158058 |

Host samples prove rendering continued after the initial client disconnect. The
user reported using Moonlight's Stop action afterward, but that click has no saved
timestamp. The fatal error predates root cleanup by roughly 13 seconds. The records
cannot prove whether it preceded, followed or resulted from the user's stop.
The fault is in guest thread `Draw Extra Geometry`, PC `0x9004a0c3e`, at bytes
`cd 42` (`INT 0x42`). Its precise guest meaning is not established. Exit65 maps
to Kyty's fatal `DbgExit(321)` path, so this is not recorded as a clean native exit.
The outer host's app-wrapper exit0 does not override the native manifest.

Capture: `/home/ali/.local/opt/kytyps5-streaming/20261016-native-v2/_CrashCapture/20261016T032705.202338Z`.
All candidate game/host/launcher processes exited; normal saves, system audio and
ordinary Sunshine process state were preserved. No second game run was attempted.
The user now requires being asked to perform any further game test; implementation
and read-only diagnosis can continue, but no game/session launch is authorized.

## Limits and remaining device checks

Streaming-only output keeps a hidden SDL/Vulkan surface and desktop connection. It
skips local presentation; game rendering and encoding still run. Existing profiles
keep local display enabled by default. Local-window mode retains its existing system
UI path; remote system UI composition is enabled for streaming-only mode.

Motion and adaptive triggers depend on Moonlight and its controller driver exposing
the relevant capabilities. Fedora currently permits ordinary gamepad input but denies
Moonlight raw DualSense HID access; a narrowly scoped access rule has been proposed
and has not been installed. Sensor packet delivery and successful effect writes can
be observed automatically, while gyro gameplay and perceived trigger resistance need
physical confirmation. HD haptic PCM, controller-speaker audio, touchpad forwarding
and USB/IP are not part of this change. Laptop speaker routing is unchanged.

SDR, H.264/HEVC, stereo, a single remote player, catalog refresh on host restart and
the bounded two-retired-plus-one-active GPU-pool limit remain. Actual 1440p Astro
playthrough and subjective latency remain device checks; 1440p GPU export and
negotiated synthetic output were verified separately.

## Previous v1 baseline

The earlier v1 verification covered 63 backend tests, 13 source-pin Git fixtures,
independent package state/overwrite refusal, real NVENC decode, standard Moonlight
pairing/catalog/reconnect, audio isolation, and killed-host cleanup/restart. A v1
actual Astro boot decoded 797 GPU copies with nonzero audio and clean shutdown.
Its detailed evidence remains under `_Build/astro-evidence/streaming-20261016/`,
including `final-ctest.log`, `final-backend-tests.log`, `final-package-tests.log`,
`final-astro-postcheck.json` and the crash-recovery postcheck. V1's prior local-only
system-panel and controller-extension limitations are superseded in v2 as described
above; physical TV compatibility still requires testing on that client.
