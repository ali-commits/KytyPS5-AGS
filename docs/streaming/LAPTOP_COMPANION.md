# Laptop controller with TV streaming

The Fedora laptop `thinker` takes the Bluetooth DualSense connection. Kyty TV
continues receiving game video and audio through its existing pairing. The
companion sends controller input through a separate authenticated connection to
the launcher-owned host; it does not create another video session.

1. Open `kytyps5-streaming` on the gaming PC and start hosting.
2. Connect the DualSense to the laptop by Bluetooth.
3. Start the game from Kyty TV using the TV remote.
4. On the laptop open **Kyty Controller**, then select **Connect**.
5. Select **Disconnect** or close the companion to release controller input.

Connect requires a running game and TV stream. If either stops, reconnect the
companion after restarting the TV game. A failed connection displays its cause;
there is no automatic game launch or background service. If switching back to a
controller attached to the TV, disconnect the companion and restart the TV stream.

The companion forwards buttons, sticks, analogue triggers, touchpad **click**, and
SDL's calibrated accelerometer/gyro samples. The host returns rumble and
adaptive-trigger effects. Audio-v2 adds native waveform haptics and controller
speaker audio over Bluetooth through the existing controller PCM broker.
Positional touchpad gestures remain unsupported.

Enable **Laptop controller audio companion** in the PC launcher's Streaming
preferences. The provisioned audio-v2 profile enables it already. The laptop app
shows a separate controller-audio status after Connect. Audio stops if its input
lease, TV session or controller connection ends. The laptop uses a single raw
writer so ordinary SDL rumble cannot disable native audio haptics.

Both directions use TLS with a dedicated client certificate and an exact pin for
the host certificate. The laptop's key and configuration are under
`~/.config/kyty-laptop-companion/` with owner-only access. The host registers only
the public certificate through its existing local management API.

The host releases input after 250 ms without a valid snapshot. The laptop expires
local feedback within its 200 ms lease even if its network thread stalls. Stop and
disconnect send neutral rumble/trigger output. TV input cannot override an active
laptop owner, and stale TV teardown cannot clear laptop input.

The laptop installation also includes a narrowly scoped udev rule for Bluetooth
DualSense `054c:0ce6` hidraw access by the active desktop user. This is needed for
SDL's sensor and effect support. It does not grant access to other HID models.

Source and tests: `tools/laptop-companion/`. Host policy and ownership tests live
in the separate pinned host repository. Automated tests and physical game
observations must be recorded separately in installation verification evidence.

Audio-v2 runtime is `~/.local/opt/kytyps5-streaming/20261019-laptop-audio-v2`.
The prior `20261019-laptop-v1` installation and laptop app version are retained.
Stop the current host before using the previous runtime's `start-streaming.sh`.
Game state is copied for the upgrade; later progress belongs to the selected
runtime and is not automatically copied back during rollback.
