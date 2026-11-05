# AGS session socket

How KytyPS5 talks to AGS (STANDARD sections 7 and 8).

## Environment

- `AGS_SESSION_SOCKET` is the socket path. It is used only when `AGS_SESSION_PROTOCOL` is `1`; with any other value the socket is ignored and a line is logged.
- `KYTY_STREAM_SOCKET` is used when `AGS_SESSION_SOCKET` is unset, so the installed ags-host keeps working.
- `KYTY_STREAM_FRAMES=0` asks for a metrics-only connection (Gamescope captures the frames). It does not apply to a stream-only launch (`KYTY_STREAM_ONLY=1`).
- Every read site calls `Streaming::SessionSocket()` in `src/streaming/sessionEnv.h`.

## Packets

- Unknown packet types from the host are ignored and logged once per type. Known types keep their handling (a known type that is not valid in the current state still drops the connection).
- `metrics.subscribe {"keys":[..],"interval_ms":n}`: unknown keys are ignored, an empty list stops metrics, the interval is clamped to 1000..3600000 ms. A malformed packet (no `keys` array) is ignored and the old subscription stays.
- `metrics {"t_ms":..,"values":{..}}` is sent at the subscribed interval with only the subscribed keys. A key without a value yet is left out.

| Key | Type | Unit | Source |
| --- | --- | --- | --- |
| `game_fps` | number | fps | `FpsStatsLatest().current` |
| `frame_time_ms` | number | ms | `FpsStatsLatest().frame_ms` |
| `guest_memory_mb` | number | MB | `direct_allocated + pooled_allocated + flexible_used` from `GetDebugStats()` |
| `shader_compiles` | integer | count | `DebugCounters::ShadersCompiled`, counted from the moment of subscription |

## Metrics-only connection

When a session socket is set but no frame pool can exist (`KYTY_STREAM_FRAMES=0`, Vulkan external memory missing, bad capture size or FPS, or the pool could not be created), the emulator still connects. Its hello is `{"type":"hello","version":1,"frames":false}` with no descriptors. STANDARD defines no marker for this; `"frames": false` is the addition the AGS host must accept. After it only `metrics.subscribe` is served; other known packets are ignored.

## Counters and overlay

`Common::DebugCounters` has two independent consumers: the performance overlay (`--perf-overlay`, F2) and metrics. `shader_compiles` turns counter collection on while subscribed and leaves the overlay hidden. F2 and Shift+F2 do nothing while a session socket is set.
