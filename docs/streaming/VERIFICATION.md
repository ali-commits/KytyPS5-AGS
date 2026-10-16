# Streaming-only and DualSense verification

Date: 2026-10-16, Asia/Kuala_Lumpur.
Status: companion work paused at the user's request. TV playback is the current
priority; a dedicated laptop client built from Moonlight is deferred until later.
The user reports that the game itself works. Full controller-audio acceptance
remains incomplete, and the companion service is disabled/stopped. Interactive
game launches remain user-led.

## Xiaomi TV controller checkpoint

The user confirmed working face buttons and L2/R2 in the separately installed
**Kyty TV (Test)** client, package `io.kyty.streaming.tv.debug`, functional source
`c41957a` in `/home/ali/projects/kyty-moonlight-android`. Android11's accessibility
input filter had stripped device identity and selected Moonlight's default mapping.
The client recovers the unique controller context; Button Mapper and Projectivy
remain enabled. The controlled filter-off comparison and then the candidate
filter-on test both passed physically. Policy checks107 and actual Android menu
adapter checks850 pass; APK and scoped source review passed. Stock Moonlight and
its pairing remain available. Gyro and vibration are separate work, with USB data
connection not yet enumerated at the first checkpoint.

The user played the full first stage and reported a game crash at its end, asking
that investigation be deferred. Preserve v3 capture
`_CrashCapture/20261016T083414.641855Z`, whose manifest records native exit65 and
SHA256 `f93b52dd9a892fad3d22acf5972786e4ad7d10c62cf051798f3a1dd5f24a15bb`.
No cause was investigated. Host currentgame0 was verified afterward; this was a
crash exit, not normal Quit. Keep the candidate's independent saves and capture
for follow-up. No game was started by an agent for these TV checks.

## Controller-audio candidate v3

The active experimental host is
`/home/ali/.local/opt/kytyps5-streaming/20261016-native-v3`, with private settings
under `/home/ali/.config/kytyps5-streaming/20261016-native-v3/streaming`.
Native/launcher package source is `5ed1291329f316f7aca5e4c6be384733795bbf54`;
the independent host is `0a68a0a9f4b2043e3e24e8c409e1736aaecc55fc`.
The dedicated `kytyps5-streaming` command still selects the tested v2 rollback.
Pairing and independent latest experimental saves were preserved. The candidate
host was started only after verifying no game was running; no game or Moonlight
client was launched by the agent during this companion phase.

Thinker's installed companion is
`/home/ali/.local/opt/kyty-controller-audio/20261016-v2/bin/controller-audio-receiver`,
source `1af7464df18616824d815be7fbc563d6cdb48089`, SHA256
`4c141d06805d172cbdbb00828563e09c8b9426905e5790e5eb5fd95416c587c1`.
Its private settings are under
`/home/ali/.config/kyty-controller-audio/20261016-v2`, and its user unit is
`kyty-controller-audio-v3b.service`. Both `kyty-controller-audio-v3b.service` and the earlier
`kyty-controller-audio-v3.service` are now disabled/inactive for this pause. The receiver identity is linked to the existing Thinker
Moonlight parent; its key stayed on the laptop. Prefix/config/identity directories
are mode0700 and key/cert/config/unit files mode0600. No system packages or audio
profile/default changes were made; a private signed RPM SDK supplied build headers.

| Check | Result |
|---|---|
| Native producer → private Unix → production broker → real mTLS → guarded null helper | Exact 76,800 PCM bytes per kind, 153,600 total; distinct actuator/speaker hashes match |
| Build-script integration and relevant existing suites | 11 CTest groups and 54 backend Kyty tests pass; existing 18 memory/pipeline regressions pass |
| Final helper on PC / Fedora laptop | Four silent tests pass on each (6.55s / 6.50s), including actual SDL dummy and guarded TLS/null; sanitizer checks pass |
| Initial physical startup failure | Reproduced SDL queue full while ALSA remained PREPARED, hw/appl pointers both zero |
| Startup correction | `f21db009` requires owned RUNNING clock progress before Ready, with a 500ms preparation budget; queue/age limits unchanged |
| Final bounded USB references | Three speaker and three actuator runs all exit0 in 1.079–1.098s; PCM closed before and after each |
| Producer restart lifecycle | `1af7464d` accepts a fresh durable lease when a new emulator's local epoch restarts; stale-generation/epoch regressions remain rejected |
| Final broad review and scoped fix reviews | No remaining Important/Critical findings; safe reconnect on Reset remains a documented Minor |
| Actual Astro controller audio | PCM reached the USB helper, but repeated queue overflows interrupted output; acceptance is incomplete and work is paused |

The user-led game attempt first used the preserved v2 host. After the user quit,
v3 was started under the transient user unit
`kyty-controller-audio-candidate-launcher.service`, and the user launched Astro.
Video, motion and adaptive-trigger counters advanced, and controller PCM reached
the companion, but RUNNING USB queue overflows caused repeated lease withdrawal
and reconnects. This is a separate runtime issue from the corrected silent-clock
startup failure. No queue limits were raised. Investigation and companion testing
were paused when the user changed priority to TV playback. The companion was
stopped and disabled; the working game/host was not killed for this pause.

The ancestry-bound USB output negotiated S16_LE, four channels at48kHz, period240
and buffer480. Readiness now proves hardware-clock progress before guest PCM is
allowed. Offline reference success proves bounded physical output and cleanup;
it does not establish fidelity or latency during a streamed game. The laptop's
normal Speaker51 remained the default, and the active service held no USB PCM
while the candidate host was idle. Software transport uses complete waveform PCM
with explicit failure/fallback on queue or age violation, rather than reducing it
to two vibration strengths.

Source reports/reviews live under
`.superpowers/sdd/2026-10-16-dualsense-controller-audio/`; local evidence is under
`_Build/astro-evidence/controller-audio-20261016/`. Exact per-component PC hashes
are in the candidate's `installation-provenance.json`. The PC package's unused
helper component was refreshed from `1af7464d`; native/launcher/host bytes were
unchanged. Keep v2 as the default until the user confirms the real game result.
See [operation and rollback](CONTROLLER_AUDIO.md) for the general procedure;
this installed candidate uses the exact unit/config paths above.

## Tested v2 rollback

- Candidate: `/home/ali/.local/opt/kytyps5-streaming/20261016-native-v2`.
- Candidate settings: `/home/ali/.config/kytyps5-streaming/20261016-native-v2`.
- Kyty runtime code: `3c624be545b000022116502d7aa36c107bfc99a4`.
- Native SHA256: `f225e03e2839630b316a6c616a5b31b1f26dc85793b78622998856ab065afc1a`.
- Matching map SHA256: `0f0f26bf399a34b5c408b777c63daa87ff730f8774505961320721d608ea13ce`.
- Host code: `f35169866ddf9824a6a580abf0524bec694baede`; pinned by Kyty commit `598670dc`.
- Host SHA256: `d65260ae0b779b07565f3784c4bcd9b7dfe2edcc4a0bfd4273e15c30852a46e1`.
- The `kytyps5-streaming` command now selects v2; its previous v1 wrapper is backed up under v2 `verification/v1-command-before-v2-default/`.
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
| Initial laptop controller attempt before access rule | Access denied: 0 sensors/HID writes; 3,718 SDL effect attempts failed | Preserved first observer/result |
| Post-rule laptop gyro and triggers | 20,803 motion samples applied; user confirms gyro and resistance function, with fidelity/latency qualifications | Manual controller result and user report |
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
new code findings. Functional device acceptance is recorded below; the earlier
fatal guest error remains preserved and was not reproduced by two subsequent
manual runs. Latency and full haptic fidelity remain improvement work.

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
The user initially required being asked before further game launches. The latest
authorization below permits bounded unattended drive tests only when they also
automatically close the game; interactive gameplay checks remain user-led.

## Limits and remaining device checks

Streaming-only output keeps a hidden SDL/Vulkan surface and desktop connection. It
skips local presentation; game rendering and encoding still run. Existing profiles
keep local display enabled by default. Local-window mode retains its existing system
UI path; remote system UI composition is enabled for streaming-only mode.

Motion and adaptive triggers depend on Moonlight and its controller driver exposing
the relevant capabilities. Fedora originally permitted ordinary gamepad input but
denied raw DualSense HID access. The approved, narrowly scoped rule is now installed,
and a reopened stock Moonlight client opens the raw device and sends real sensors.
The user has now confirmed gyro gameplay and trigger resistance function, while
reporting a small noticeable delay and lower fidelity than direct USB play. HD haptic PCM, controller-speaker audio, touchpad forwarding
and USB/IP are not part of this change. Laptop speaker routing is unchanged.

SDR, H.264/HEVC, stereo, a single remote player, catalog refresh on host restart and
the bounded two-retired-plus-one-active GPU-pool limit remain. Actual 1440p Astro
playthrough and subjective latency remain device checks; 1440p GPU export and
negotiated synthetic output were verified separately.

## Post-reboot controller access and user-led testing

After the Plasma update/reboot, all installed emulator/launcher/host/map hashes
still matched the v2 provenance. The user launched Astro manually, confirmed
working video, full ordinary controls and no local game window. Native PID14876
ran from 03:48:35 until 03:51:38 UTC and recorded exit0 with a complete crash
capture at `20261016T034835.234706Z`. The earlier fatal exit was not reproduced.
The passive observer initially read the previous `last-run.txt`, which updates
only on game exit; `user-led-20261016T034810Z/manual-run-result-corrected.json`
uses the actual current-run manifest and explicitly records that QA selection error.

The user approved necessary laptop access/installations without asking again. They
clarified that unattended drive tests are allowed only with automatic full game
closure, while interactive checks require their presence. The old standalone
90-second client runner is insufficient on its own because `--no-quit-after`
leaves the game alive. No automatic game was launched for this controller check.

At 03:54:33 UTC, the approved rule was installed at
`/etc/udev/rules.d/70-kyty-dualsense.rules` on the laptop, root:root0644:

```udev
ACTION=="add|change", SUBSYSTEM=="hidraw", SUBSYSTEMS=="usb", ATTRS{idVendor}=="054c", ATTRS{idProduct}=="0ce6", TAG+="uaccess"
```

Only the freshly verified `/dev/hidraw1` sysfs node was triggered. The active
desktop user received an rw ACL; an actual O_RDWR open passed. A standalone SDL
probe changed from no sensors to accel=true and gyro=true. The original Moonlight
process and built-in Speaker51 audio default were unchanged by installation.
Exact rule/device/ACL snapshots and rollback instructions are preserved in
`dualsense-access-install-20261016T035433Z/`. The rule SHA256 is
`53d8d22e1b88ef8db722baa0bcf2724a51332aeb2fb5c3b8a4f81afefd52707a`.

The user quit and reopened stock Moonlight themselves, then started a new manual
controller check. New PID254115 holds `/dev/hidraw1`. Host diagnostics show real
accelerometer/gyro packets entering Kyty and being applied, with no invalid samples.
The sampled gyro magnitude reached 4.586 rad/s while the controller moved.
The dropped-export counter stayed constant while copied images increased during
the observed live-viewer interval.
Native PID26722 stopped cleanly with exit0 at 04:01:28.591068 UTC; capture
`20261016T035915.308871Z` is complete. The passive collector observed 20,803 applied
motion samples, zero invalid samples and real moving SI vectors. Host feedback
was active, and the user confirmed gyro operation and adaptive-trigger resistance.
The collector is observation-only and performed no game/client control.

The user reported three remaining quality limits: no controller-speaker output;
small noticeable gyro/haptic delay compared with buttons/sticks; muffled and missing
haptic sensations and less faithful trigger feel compared with direct PC play.
Read-only source tracing established that vibration PCM currently becomes two
absolute channel peaks, doubled/clamped, held as 100 ms ordinary rumble. This
necessarily loses waveform detail. PADSPK has no remote path and falls back to main
audio; remote controller-speaker silence is expected. The 100 Hz sensor request
versus native USB250 Hz, and the 16 ms remote adaptive send gate, are confirmed
cadence differences; their share of perceived total delay is not measured.
SDL's 10 ms HID output worker also exists in direct local playback and is not a
remote-only penalty. No network fault or calibration fault has been established.

The tested v2 command was promoted atomically at 04:17:02 UTC after verifying all
installed executable/map hashes and checking wrapper syntax. No running executable,
profile or save was overwritten. The old command and v1 installation remain as
rollback. Full haptic waveforms and controller speaker are the user's selected next
feature, requiring a separate audio path; they are not claimed by this build.

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
