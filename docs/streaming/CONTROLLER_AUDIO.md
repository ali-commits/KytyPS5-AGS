# Controller audio companion: provisioning and operation

This candidate sends native actuator and PADSPK stereo PCM to a separate Linux USB
companion alongside stock Moonlight. Ordinary game audio stays on the laptop's selected
Moonlight output. The helper does not own the SDL gamepad or change the system default
output, audio profiles, microphone, pairing or saves. Bluetooth and TV output are deferred.

The [binding specification](../superpowers/specs/2026-10-16-dualsense-controller-audio.md),
[native producer notes](../controller-audio-wire.md) and
[companion build/CLI reference](../../tools/controller-audio/README.md) define the wire,
queue bounds and independent verification. Silent tests do not establish USB sensations.

## Install a fresh candidate

Build and run the standalone helper's silent tests using its README. Record the exact
source commits and SHA256 of the resulting helper and host binaries. Use a fresh v3
prefix and a private configuration directory; keep the stable v2 installation available.
The installer accepts a prebuilt executable and never builds or launches it:

```sh
tools/controller-audio/install-user.sh \
  --binary /absolute/build/controller-audio-receiver \
  --prefix "$HOME/.local/opt/kyty-controller-audio-v3" \
  --config-dir "$HOME/.config/kyty-controller-audio-v3"
```

The installer creates mode0700 prefix/bin/config directories, a mode0755 binary, private
install metadata and a mode0600 user unit named `kyty-controller-audio-v3.service`.
`--unit OTHER.service` selects a separate candidate unit. Installation is idempotent for
identical files. A different binary/unit, symlink destination, unrelated existing prefix,
or configuration directory with shared permissions is refused: choose a fresh candidate.
It reloads the user manager's unit list but does not enable/start the service. No identity,
configuration, host credentials, audio device permissions or system service is created.

The service requires an existing config and runs the helper's hardware-free `--status`
as `ExecCondition`. Invalid config leaves it inactive. Once explicitly activated with a
valid config, the helper waits/reconnects; hardware opens only during authenticated session preparation with a fresh clock map.
PCM begins after the helper Ready acknowledgement and host State active.

## Provision identity and trust

Generate identity on the laptop that will execute the helper. The destination identity
directory must be new; the helper creates mode0700 and mode0600 key/certificate files:

```sh
receiver="$HOME/.local/opt/kyty-controller-audio-v3/bin/controller-audio-receiver"
config_dir="$HOME/.config/kyty-controller-audio-v3"
"$receiver" --generate-identity "$config_dir/identity"
```

Using a trusted local/SSH provisioning channel, copy only `identity/cert.pem` to the host.
Register that public certificate through the host's existing authenticated loopback API
`POST /api/kyty/controller-audio/receivers`, with fields `name`, `parent_client_id` and
`certificate_pem`. The parent must be the enabled Moonlight client that will run the
session. The response supplies receiver ID/fingerprint; duplicate certificate+parent
registration is idempotent. Keep the laptop's private key on the laptop. The helper needs
its own identity and the host certificate pin; it never receives host management or
Moonlight private credentials. SSH provisioning is an operator action, not a product
remote-execution feature.

Obtain the exact host certificate DER SHA256 through that trusted channel. Create
`$config_dir/config.json` privately (`umask 077`, mode0600), using absolute identity paths:

```json
{
  "host": "192.0.2.10",
  "port": 48219,
  "pinned_host_sha256": "REPLACE_WITH_64_LOWERCASE_HEX_SHA256",
  "cert": "/home/USER/.config/kyty-controller-audio-v3/identity/cert.pem",
  "key": "/home/USER/.config/kyty-controller-audio-v3/identity/key.pem",
  "sink": "null"
}
```

The example pin is deliberately invalid until replaced. `host` is a numeric address.
Receiver ID defaults to its certificate DER SHA256; if explicitly set, use the registered
host receiver ID. Optional `usb_path` selects a stable USB ancestry path; `audio_name`
must match the ancestry-verified SDL endpoint. Never configure transient card/hidraw indices.

Host controller audio defaults off. Enable the feature only on the fresh candidate after
provisioning: `kyty_controller_audio=true`, optional explicit `kyty_controller_audio_port`
(default base+30, usually48219), and a private `kyty_controller_audio_clients` registry.
The host derives its owner-only private PCM socket beside `kyty_socket`. List/revoke
receivers through the launcher's receiver controls or authenticated loopback API:

- `GET /api/kyty/controller-audio/receivers`
- `PUT /api/kyty/controller-audio/receivers/ID` with `{"enabled":false}`
- `DELETE /api/kyty/controller-audio/receivers/ID`

To open the candidate launcher on its Streaming tab and start hosting with its saved
preferences, run the candidate's executable with the explicit flag:

```sh
/absolute/candidate/start-streaming.sh --start-streaming-host
```

The launcher validates the emulator and prepares its library catalog before requesting
hosting. Invalid streaming preferences remain visible in the Streaming tab. This flag
starts only the host; choose a game manually in Moonlight when ready. Without the flag,
the launcher waits for the Start hosting button.

The optional `controller_audio` object on `GET /api/kyty/status` reports configuration,
state, receiver, counters, queue time and bounded error. A registered certificate alone
never grants another client's session; the mapped parent and running session must match.

## Silent checks and explicit activation

These bounded commands open no audio/hidraw hardware:

```sh
"$receiver" --config "$config_dir/config.json" --status
"$receiver" --config "$config_dir/config.json" --sink null --run-seconds 10 --diagnostics
systemctl --user status kyty-controller-audio-v3.service --no-pager
journalctl --user -u kyty-controller-audio-v3.service -n 30 --no-pager
```

An explicitly requested raw capture also requires null diagnostics and a new destination:
`--sink null --run-seconds 10 --diagnostics --raw "$config_dir/new-capture.kcpa"`.
It contains full canonical PCM frames and is created mode0600 without overwriting a file.
Per-kind hashes verify exact transported payload; they do not prove physical output.

After silent host/session/receiver proof and the separately coordinated hardware boundary,
set the private config's sink to `usb`. Explicitly activate the candidate only when ready:

```sh
systemctl --user enable --now kyty-controller-audio-v3.service
```

The helper handles SIGTERM and lease/device/network loss by flushing owned playback,
stopping helper rumble and restoring helper-owned speaker routing. Do not run a foreground
USB reference or a second helper while the service owns the same controller.

## Bounded physical reference and supervised game proof

The following commands intentionally play audio/actuator output. Run them only at the
agreed hardware boundary, with the service stopped and the USB controller unambiguously
selected. Each reference automatically ends within1..3seconds:

```sh
systemctl --user stop kyty-controller-audio-v3.service
"$receiver" --config "$config_dir/config.json" --sink usb --reference-seconds 1 --reference-kind speaker
"$receiver" --config "$config_dir/config.json" --sink usb --reference-seconds 1 --reference-kind actuator
"$receiver" --config "$config_dir/config.json" --sink usb --run-seconds 20
```

Reference mode is local, amplitude0.02 at220Hz, and requires no game/server. Normal USB
mode requires the authenticated prepared session. Record hardware identity, actual four
channels, cleanup and the user's speaker/texture observations separately from byte counts.
A supervised game check must verify gyro/triggers, ordinary rumble, controller audio,
normal laptop audio and full game/client/helper cleanup. No unattended game is started
by this installer or service. Do not claim native physical parity before those checks.

## Rollback and removal

1. `systemctl --user disable --now kyty-controller-audio-v3.service`.
2. Disable/delete this receiver using the authenticated host API above, then disable
   candidate host controller audio to restore existing Moonlight rumble/PADSPK fallback.
3. Remove this candidate unit from `$XDG_CONFIG_HOME/systemd/user` (default
   `$HOME/.config/systemd/user`) and run `systemctl --user daemon-reload`.
4. Remove only the fresh helper prefix, its private config/identity and explicit captures
   after confirming their exact paths. Removing the identity permanently loses that
   receiver key; a future installation must generate/register a new identity.
5. Select the preserved v2 installation/config. Preserve Moonlight pairing, the default
   audio/profile and current game saves. Keep the candidate source/evidence for review.

The installer deliberately refuses binary replacement in an existing prefix. Installing a
new candidate beside the previous one makes rollback a service/config selection operation.
