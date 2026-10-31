This shared library compiles the emulator's original
`src/libs/dualSenseBluetooth.cpp` and header directly. The source file remains
unchanged. Only its HID open/write/close symbols are redirected at compile time
to register an atomic controller lease and count accepted or failed reports.
The logging shim replaces the emulator logger with stderr. `provenance.json`
records SHA256 hashes, and `kyty_bt_source_sha256()` exposes the compiled source
hash. This is the original 547-byte Bluetooth report `0x39` implementation,
including its 3 kHz haptic resampling, 45 kHz speaker compensation, two 480-frame
constant-bitrate 200-byte Opus packets, and Bluetooth CRC.

Build and verify with development headers:

```sh
./build.sh
ctest --test-dir build --output-on-failure
```

Runtime-only Linux systems can use copied headers without installing development
packages or unversioned library symlinks:

```sh
KYTY_SDL_INCLUDE=/path/containing/SDL3 \
KYTY_OPUS_INCLUDE=/path/containing/opus.h \
./build-runtime.sh
```

The standalone builder links `libSDL3.so.0` and `libopus.so.0`. The CMake builder
also accepts `-DKYTY_SDL_INCLUDE=... -DKYTY_OPUS_INCLUDE=...`. The shared library
must use the same SDL3 runtime as the already-opened input controller. Discovery
requires a complete normalized Bluetooth serial and exactly one opened wireless
PS5 gamepad with Sony vendor and a supported product ID.

The C ABI is declared in `kyty_bt.h`. Boolean operations return 1 on success, 0
on failure; controller discovery returns -1 on failure, open returns null, and
queue returns 0 on failure. `kyty_bt_error()` returns thread-local error text.
Callers retain handles until `close`, including after lease expiry. `shutdown`
closes and releases every remaining handle; handles must not be reused afterward.
Controller deadlines use the clock returned by `kyty_bt_now_us()`, which is not
assumed to share Python's monotonic-clock epoch.

Open automatically applies the exact 38-byte speaker routing payload copied
from the emulator's `RouteSpeaker`, including route `0x30` and preamp `0x02`.
Closing the final speaker stream restores routing after the original sender's
write barrier. Controls use SDL's 47-byte trigger payload and SDL rumble API.
Nonzero haptic PCM suppresses legacy rumble for the emulator's original
`max(queued_duration, 250ms)` tail and then restores any remaining rumble lease.

An independent native watchdog checks controller deadlines at most 10 ms apart,
closes native streams, restores the speaker route, and sends neutral trigger and
rumble state. An atomic lease check also rejects initiation of each native HID
write after expiry, independently of Python's scheduling. Zero-deadline controls
are an explicit successful stop; expired nonzero deadlines are rejected. A new
controller has a one-second setup grace; input controls or `renew` must establish
the real lease before continued streaming.

`queued_us` is an estimate: the latest original `Queue` return, minus elapsed
native-clock time. It does not include the final 21.334 ms transport slot. The
caller must include that slot when enforcing its PCM source-age budget. The
original source bounds pending audio to 80 ms and drops old whole slots on
overflow. Accepted report/byte counts mean that the OS accepted HID writes;
they are not playback acknowledgments. A blocked OS HID call may delay close,
and an already-started write cannot be recalled; this does not provide a hard
physical playback deadline.

Tests replace gamepad discovery, SDK effects, and HID devices with fakes while
running the real SDL resampler, Opus encoder, and original sender thread. They
verify report layout, CRC, CBR decoding, resampling rates, exact speaker/trigger
routing, rumble suppression and restoration, independent lease expiry, safe
handle close, close barriers, explicit stops, reacquisition, and write failures.
They never open a physical controller.
