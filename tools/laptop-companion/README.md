# Kyty laptop controller

This Python/Tk application forwards a Bluetooth DualSense to an existing Kyty host
while the TV handles video and audio. Start the game and TV stream first, then
click **Connect**. It does not launch games or change Bluetooth pairing.

On Fedora, install system dependencies with `sudo dnf install python3 python3-tkinter SDL2`.
Pair the DualSense in the desktop Bluetooth settings. The logged-in desktop user
needs access to its hidraw device for SDL HIDAPI; if it is unavailable, check
udev/uaccess permissions. SDL2 2.0.18 or newer is required; current SDL2 is preferred.

The included `70-kyty-dualsense-bluetooth.rules` grants the active desktop user
access to Bluetooth DualSense `054c:0ce6` only. It is installed on `thinker`.
For another laptop, install it into `/etc/udev/rules.d/`, reload udev rules and
reconnect the controller:

```sh
sudo install -m 644 70-kyty-dualsense-bluetooth.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules
```

Run:

```sh
python3 /absolute/path/tools/laptop-companion/companion.py --config /absolute/private/companion.json
```

Create a private configuration directory (`chmod 700`) and config/key files
(`chmod 600`). Register a dedicated client certificate with the host. Example
configuration (certificate/key paths may be relative to this config):

```json
{
  "host": "host.example.ts.net",
  "port": 47984,
  "cert": "laptop-client.pem",
  "key": "laptop-client-key.pem",
  "server_sha256": "64 hexadecimal characters from the trusted server certificate"
}
```

Obtain the pin from the trusted host's certificate, not an unverified network
connection: `openssl x509 -in server.pem -outform DER | sha256sum`.
TLS authenticates the exact pinned DER certificate and presents the dedicated
client identity. The pin replaces CA/hostname validation; certificate rotation
requires updating this private configuration. Config and key permissions are
checked before connection. Keep credentials out of the repository.

A desktop entry can use the same absolute command as its `Exec` value, with
`Type=Application`, `Name=Kyty Controller`, and `Terminal=false`. The app can be
copied as this whole directory; no virtual environment or Python packages are
needed. Installing or configuring it does not start the game or test motors.

The UI reports battery level categories supplied by SDL, response latency,
snapshot count and feedback status. Cross/Circle/Square/Triangle, D-pad, shoulders,
Options, PS, stick clicks and touchpad click are supported. Create and touchpad
click both reach the host's existing native touchpad-button path. Stick Y uses
negative-up; trigger axes range from 0 to 1. Calibrated accelerometer m/s² and
gyroscope rad/s retain SDL coordinates and units, bounded to 160 and 40. Positional
touchpad gestures are not supported. Battery percentage is unavailable through
this SDL2 interface.

The TLS worker keeps one request in flight and reuses HTTPS connections. The
SDL worker samples fresh state after each response, up to approximately 100 Hz;
it never queues old input. Socket operations time out after 150 ms. Independently,
the SDL loop expires effects within the returned lease (at most 200 ms), even if
DNS or a slowly arriving response stalls the TLS thread. Late responses cannot
restart expired effects. Disconnect and close stop rumble and send mode 0x05
(off) to both triggers immediately, then request host release. After network loss,
the host's separate 250 ms watchdog clears input. Reconnect explicitly after errors.

POST `/kyty/v1/relay` uses `connect`, `input` and `disconnect` actions. Input carries
opaque session, increasing sequence, connected state, Moonlight button flags,
six axes and SI sensor triples. Successful input replies include the same session,
`ok`, revision, lease_ms, two uint16 rumble values and trigger flags/left/right.
Trigger flags use native DualSense bits: left 0x08, right 0x04. Each trigger is
11 bytes (mode plus parameters). The app sends SDL's 47-byte DS5EffectsState_t,
with right at byte 10 and left at byte 21; SDL adds USB/Bluetooth framing and CRC.
The application makes no explicit LED writes.

References: [SDL2 PS5 driver](https://github.com/libsdl-org/SDL/blob/SDL2/src/joystick/hidapi/SDL_hidapi_ps5.c)
and [Moonlight button definitions](https://github.com/moonlight-stream/moonlight-common-c/blob/master/src/Limelight.h).
SDL may itself initialize controller lighting as part of opening enhanced mode.

Run tests with `python3 -m unittest discover -s tools/laptop-companion -v` from
the repository root. Tests cover mappings, SI values, output framing, pin-before-
request, transport failures, feedback expiry, disconnect neutralization and a
stalled network worker. Physical Bluetooth, in-game controls and adaptive-trigger
feel still require a real controller/game session; automated tests do not exercise
motors.
