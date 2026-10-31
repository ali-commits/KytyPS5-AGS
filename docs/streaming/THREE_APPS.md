# Kyty phone, TV and PC candidate

The phone owns the game library, TV selection, streaming settings and Bluetooth
DualSense. The TV app is a foreground viewer. The PC launcher owns the headless
game, private audio, process cleanup and a separate streaming profile.

| App | Responsibility |
|---|---|
| Kyty Controller (phone) | Library, Play/Stop, TV selection, video/audio/controller settings, Bluetooth input and feedback, dim foreground mode |
| Kyty TV | Connect to the paired PC, wait for phone commands, display video/audio, stop when exiting |
| Kyty PC host | Publish the library, authenticate apps, coordinate one session, route controller data and run/close the owned game |

## One streaming resolution, separate local settings

Full HD means1920×1080, 2K means2560×1440. The phone's selected pair reaches the TV
request, encoder, native capture and child emulator `--screen-width/height`.
The wrapper discards the local profile's screen arguments for that streaming
launch; it never writes them back into the saved local game configuration. Change
local Library game settings to2K independently of a Full HD streaming profile.

Other offered output sizes are720p and4K. The game can still select its own internal
dynamic render buffers; screen/capture resolution does not rewrite game shaders.
StreamFPS controls capture/encoding, while the game profile's simulation vblank
frequency stays intact. Quality edits require stopping the current session before
saving and starting again. The PC no longer presents competing capture/codec/FPS/
bitrate selectors for this managed flow.

## Initial installation

The built candidate is installed at
`/home/ali/.local/opt/kytyps5-streaming/20261016-native-v4`. In the morning, turn on
the TV and reconnect/unlock the phone with USB debugging, then run on the PC:

```sh
/home/ali/.local/opt/kytyps5-streaming/20261016-native-v4/setup-devices.sh
```

This starts the host if needed. It does not launch a game. Phone USB is only for
this development installation; the DualSense stays on Bluetooth and gameplay
traffic uses Wi-Fi/LAN. Open **Kyty TV (Candidate)** and **Kyty Controller** after
installation. Pick a game and press **Play** on the phone yourself.


The TV is deliberately left off during the overnight build. The phone's USB
connection disconnected, so device installation/provisioning waits until it is
connected again. The signed candidate APKs are prepared alongside the PC package.

Turn on the TV, enable its existing Wi-Fi ADB, reconnect phone USB debugging, and
run the package's `setup-devices.sh`. This installs the separate phone app and the
signed TV viewer update, exports only the phone's public AndroidKeyStore
certificate, registers it through authenticated pinned loopback management and
writes the PC's public LAN endpoint/certificate pin to the phone. No private key,
management password or TV identity is copied to the phone. The script opens apps
but has no game launch operation. USB is needed for this development setup only;
controller gameplay uses Bluetooth and LAN.

The TV package/signing identity is preserved so existing PC pairing remains.
The new PC candidate carries forward the preserved TV server identity/pairings.
If the TV has no saved paired host, its minimal pairing screen is available;
normal use shows only connection/waiting status. Background TV wake is outside
this candidate: turn on/open Kyty TV before using Play on the phone.

## Morning acceptance

1. Connect the DualSense to the phone by Bluetooth, with no controller USB cable.
   Open Kyty TV and wait until the phone lists the TV as ready.
2. On the phone, select Full HD/60FPS/HEVC and an appropriate bitrate. Choose Astro
   and tap Play yourself. Confirm TV picture and game sound, and no PC game window.
3. Check face buttons, L2/R2 travel, sticks, gyro and ordinary vibration. Enter dim
   mode, verify controls still work, then touch the screen to restore the library.
4. Tap Stop on the phone. Confirm both the TV stream and actual PC game close.
5. Select2K on the phone and start again yourself; compare output/profile values.
   Stop again. The local Library resolution must remain unchanged.
6. Exit the TV viewer or close the phone app during a short session. Input should
   neutralize promptly and the owned stream/game should close through watchdogs.

Host rejects Play if the TV is absent/busy. Startup timeout, TV loss and phone loss
close the owned process; stale callbacks and packets cannot regain controller
ownership. A phone/tv can stop only its matching managed session, not an unrelated
local game. No interactive game is automatically started by build/setup/tests.

The real phone passed public device buttons, triggers, motion sensors and brief
vibration locally before this build. Automated source/transport checks do not
replace physical whole-system acceptance. Current phone feedback is Android
ordinary vibration. Native haptic waveforms, adaptive trigger fidelity and
controller-speaker audio remain separate work; this candidate does not claim
those features or measured latency/HDR/4K performance. The laptop companion stays
paused, and the preserved v2/v3 installations remain available for rollback.

## Verification record

The host passed52 focused C++ tests, native launcher46 Qt checks plus capture
configuration tests, phone109 core checks and40 preference coverage checks, TV
startup/input/profile/PiP policies plus107 legacy input assertions, and five
real-TLS setup trust tests. Both APKs build, lint and signing checks pass.
Independent scoped reviews passed after fixes. Actual packaged-host idle RPC
checks passed without calling Play or starting an emulator.

Phone/TV hardware installation and whole-system video/controller acceptance are
pending because the phone USB connection disconnected and the TV remained off.
See `verification/three-apps` and `three-apps-provenance.json` inside the package for
exact source revisions, APK/binary hashes, test evidence and physical limits.
The earlier stage-one game crash remains deferred; its original v3 crash capture
is preserved. No emulator crash fix is claimed by this build.
