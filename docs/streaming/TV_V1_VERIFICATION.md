# TV streaming v1 — 2026-10-19

The dedicated `kytyps5-streaming` command now uses the current native launcher to
own hosting. Kyty TV opens its paired PC/game library and sends normal Bluetooth
controller input through the ordinary GameStream path. No phone is required.

## Initial v1 build

- Runtime: `/home/ali/.local/opt/kytyps5-streaming/20261019-tv-v1`.
- Private profile: `/home/ali/.config/kytyps5-streaming/20261019-tv-v1/streaming`.
- Launcher source: `75c1cb335bf43700d5052f8937d7c3fcebf27e78`.
- Android source: `5eced3bed9e6f3b059d2d3f7644b3827d764b3fd`.
- TV package: `io.kyty.streaming.tv.debug`, version317 / `12.2-kyty-tv-streaming-v1`.
- TV APK SHA256: `1ec6d4a13bbf6555982e21eb479293683d71ffadf635ee2a27168d2baead2a0d`.
- Existing f4305de8 emulator, native map and084411218 host reused byte-for-byte.
- Normal kytyps5 and unrelated system Sunshine unchanged; user upstream ports and
  untracked .kiro work preserved.

The old `kyty-three-apps-20261018-renderer.service` was idle before retirement.
It is now inactive and persistently masked. Its separate desktop entry and
start-host/stop-host scripts were removed with rollback copies. No replacement
standalone service was installed. The host is a child of the visible launcher.

## Verified

- Client assemble/lint passed with zero lint errors. Existing warnings retained.
- Actual compiled controller/network callbacks, device discovery, queued work and
  shutdown reject hardware feedback/sensor/LED writes, including zero/cancel calls.
- Viewer/profile/JNI/startup tests and110 Android11 input assertions passed.
- Launcher streaming tests:51 passed,0 failed,1 environment-specific audio skip.
  Launcher preservation:37 passed; post-format focused checks:6 passed.
- Both task reviews and broad integration review approved without actionable findings.
- Fresh runtime/package and profile migration preserve engine/host/map, saves,
  cache, trophies, patches, server identity and client pairing files.
- Installed TV APK hash verified; client key/certificate/unique ID unchanged.
- TV profile verified:1080p60, HEVC preference,30Mbps, SDR; USB/motion/fallback off.
- Bounded real Qt GUI startup reached Hosting ready, then quit cleanly and removed
  its listening ports. A follow-up normal visible launcher owns the ready host.
- Exact TLS-pinned management status reports waiting, zero viewers/current game0,
  and controller audio disabled. TV selected the paired PC and displayed ASTRO BOT.

No real game was launched during deployment. Physical video/audio, ordinary
controller responsiveness, reconnect behavior, latency and emulator stability
still require the user's game test. Rumble, native haptics, adaptive effects,
gyro, controller speaker and LED output remain disabled: diagnostics proved
physical effects but also showed loss of normal input when effects begin.

## First game test

1. Reconnect the DualSense to the TV once to reset the previous diagnostic mode.
2. The native PC launcher is currently hosting. After a restart, open
   `kytyps5-streaming`, select Streaming, then Start hosting.
3. Open Kyty TV, select Kyty PS5 Streaming, then ASTRO BOT. Start the game yourself.
4. Check picture, TV game audio, sticks, buttons and L2/R2 analogue input.
5. Use the PC launcher's Stop game / Stop hosting controls when finished.

Detailed private evidence and rollback references are in
`/home/ali/.local/state/kyty-tv-stream-v1-20261019`. Curated build/review evidence
is retained under this runtime's `verification/tv-v1/`. Old installations and
new copied progress remain separate; no save writeback was performed.


## v1.2 launch fixes and clean reinstall

The custom TV client was cleanly reinstalled on2026-10-19, preserving only the
client identity and saved PC database from a private backup. Controller Lab,
its self-test package and the temporary callback probe were removed. Stock
Moonlight was left installed.

Client source: `8dd1e97f11be097dd57761140e9d8561691f0bca`; version319,
`12.2-kyty-tv-streaming-v1.2`; APK SHA256
`b358ff1c1f7450a45e46745a2c4181ee23acb122b2f906e282004342c0297f51`.

Two device failures were isolated:

- Projectivy cached the former ViewerActivity component. Its exported compatibility
  entry now redirects to PcView with a fresh intent and immediately finishes.
- The TV denies WRITE_EPG_DATA for optional home-screen recommendations. That
  SecurityException escaped the native connection-started callback and produced
  error0. Recommendation denial is now logged and skipped. Native startup that
  throws after entering JNI now stops native state before dropping callback
  references and releasing connection ownership, preventing orphaned streams and
  the later native stage assertion.

The legacy-entry, optional-recommendation, startup-race, controller-output boundary
and Android11 input tests passed; assemble and lint passed. An actual TV callback
probe reproduced the permission denial before the fix and completed afterward,
without launching a game. Pairing identity files were verified byte-for-byte.
The corrected old shortcut opens PcView. Real game retry remains user acceptance.
Private evidence: `/home/ali/.local/state/kyty-tv-reinstall-20261019`.
