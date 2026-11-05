#ifndef KYTY_STREAMING_METRICS_SOURCE_H
#define KYTY_STREAMING_METRICS_SOURCE_H
#include "streaming/metrics.h"

namespace Streaming {
// Reads the catalog values from the running emulator: FpsStats (game_fps, frame_time_ms),
// LibKernel::Memory (guest_memory_mb) and DebugCounters (shader_compiles). Subscribing to
// shader_compiles turns counter collection on without showing the performance overlay.
MetricsHooks MakeEmulatorMetricsHooks();
} // namespace Streaming
#endif
