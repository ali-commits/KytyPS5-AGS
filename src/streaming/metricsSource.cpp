#include "streaming/metricsSource.h"

#include "common/debugCounters.h"
#include "common/fpsStats.h"
#include "kernel/memory.h"

#include <memory>

namespace Streaming {
namespace {
// Shared by the two hooks. Both run on the transport thread; the memory sampler thread only
// writes its atomic.
struct SourceState {
	// guest_memory_mb takes the guest memory locks, so it is read once a second off the transport
	// thread (which also publishes frames) and the metric reads the cached value.
	PeriodicSampler memory {[] {
		return static_cast<double>(Libs::LibKernel::Memory::GetGuestMemoryUsedBytes()) /
		       (1024.0 * 1024.0);
	}};
	CounterBaseline shader_compiles;
};
} // namespace

MetricsHooks MakeEmulatorMetricsHooks() {
	const auto   state = std::make_shared<SourceState>();
	MetricsHooks hooks;
	hooks.subscribed = [state](MetricMask keys) {
		const bool shaders = (keys & MetricBit(Metric::ShaderCompiles)) != 0;
		Common::DebugCounters::SetMetricsCollecting(shaders);
		state->shader_compiles.Update(
		    shaders, Common::DebugCounters::Get(Common::DebugCounters::Counter::ShadersCompiled));
		if ((keys & MetricBit(Metric::GuestMemoryMb)) != 0)
			state->memory.Start();
		else
			state->memory.Stop();
	};
	hooks.sample = [state](MetricMask keys) {
		MetricValues values;
		if ((keys & (MetricBit(Metric::GameFps) | MetricBit(Metric::FrameTimeMs))) != 0) {
			if (const auto fps = Common::FpsStatsLatest(); fps && fps->has_samples) {
				values.Set(Metric::GameFps, fps->current);
				values.Set(Metric::FrameTimeMs, fps->frame_ms);
			}
		}
		if ((keys & MetricBit(Metric::GuestMemoryMb)) != 0) {
			if (const auto memory = state->memory.Latest())
				values.Set(Metric::GuestMemoryMb, *memory);
		}
		if ((keys & MetricBit(Metric::ShaderCompiles)) != 0) {
			values.Set(Metric::ShaderCompiles,
			           static_cast<double>(state->shader_compiles.Since(Common::DebugCounters::Get(
			               Common::DebugCounters::Counter::ShadersCompiled))));
		}
		return values;
	};
	return hooks;
}
} // namespace Streaming
