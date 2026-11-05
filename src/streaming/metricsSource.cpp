#include "streaming/metricsSource.h"

#include "common/debugCounters.h"
#include "common/fpsStats.h"
#include "kernel/memory.h"

namespace Streaming {
MetricsHooks MakeEmulatorMetricsHooks() {
	MetricsHooks hooks;
	hooks.subscribed = [](MetricMask keys) {
		Common::DebugCounters::SetMetricsCollecting((keys & MetricBit(Metric::ShaderCompiles)) != 0);
	};
	hooks.sample = [](MetricMask keys) {
		MetricValues values;
		if ((keys & (MetricBit(Metric::GameFps) | MetricBit(Metric::FrameTimeMs))) != 0) {
			if (const auto fps = Common::FpsStatsLatest(); fps && fps->has_samples) {
				values.Set(Metric::GameFps, fps->current);
				values.Set(Metric::FrameTimeMs, fps->frame_ms);
			}
		}
		if ((keys & MetricBit(Metric::GuestMemoryMb)) != 0) {
			const auto memory = Libs::LibKernel::Memory::GetDebugStats();
			const auto used = memory.direct_allocated + memory.pooled_allocated + memory.flexible_used;
			values.Set(Metric::GuestMemoryMb, static_cast<double>(used) / (1024.0 * 1024.0));
		}
		if ((keys & MetricBit(Metric::ShaderCompiles)) != 0) {
			values.Set(Metric::ShaderCompiles,
			           static_cast<double>(Common::DebugCounters::Get(
			               Common::DebugCounters::Counter::ShadersCompiled)));
		}
		return values;
	};
	return hooks;
}
} // namespace Streaming
