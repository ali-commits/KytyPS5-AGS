#ifndef KYTY_STREAMING_METRICS_H
#define KYTY_STREAMING_METRICS_H
// Live metrics on the AGS session socket (STANDARD section 8). Header only and free of emulator
// dependencies, so the protocol tests build it without the kernel or graphics.
#include "streaming/protocol.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <functional>
#include <optional>
#include <string_view>

namespace Streaming {

// Catalog keys, units and types equal the adapter's hello catalog (agsAdapter/adapter.cpp).
enum class Metric : uint8_t { GameFps, FrameTimeMs, GuestMemoryMb, ShaderCompiles, Count };
constexpr size_t MetricCount = static_cast<size_t>(Metric::Count);

struct MetricInfo {
	std::string_view key, unit, type;
};
inline constexpr std::array<MetricInfo, MetricCount> MetricCatalog {{
    {"game_fps", "fps", "number"},
    {"frame_time_ms", "ms", "number"},
    {"guest_memory_mb", "MB", "number"},
    {"shader_compiles", "count", "integer"},
}};

// The hello catalog says 1000: the fps window is aggregated once per second.
constexpr uint32_t MetricsMinIntervalMs = 1000;
constexpr uint32_t MetricsMaxIntervalMs = 3600000;

using MetricMask = uint32_t; // Bit n is Metric n.
constexpr MetricMask MetricBit(Metric metric) {
	return MetricMask {1} << static_cast<unsigned>(metric);
}

struct MetricsSubscription {
	MetricMask keys        = 0; // Empty means send nothing.
	uint32_t   interval_ms = MetricsMinIntervalMs;
};

// Parses {"type":"metrics.subscribe","keys":[...],"interval_ms":n}. Unknown keys are ignored, an
// empty list stops metrics, and the interval is clamped to the catalog minimum (a missing or
// non-integer interval counts as the minimum). Returns false, leaving `output` alone, when `keys`
// is absent or not an array: the host's packet is malformed and the old subscription stays.
inline bool ParseMetricsSubscribe(const Json& packet, MetricsSubscription& output) noexcept {
	if (!packet.is_object() || !packet.contains("keys") || !packet["keys"].is_array())
		return false;
	MetricsSubscription parsed;
	for (const auto& key: packet["keys"]) {
		if (!key.is_string()) continue;
		const auto& name = key.get_ref<const std::string&>();
		for (size_t i = 0; i < MetricCount; i++)
			if (MetricCatalog[i].key == name) parsed.keys |= MetricMask {1} << i;
	}
	if (packet.contains("interval_ms") && packet["interval_ms"].is_number_integer()) {
		const auto interval = packet["interval_ms"].get<int64_t>();
		parsed.interval_ms  = static_cast<uint32_t>(
            std::clamp<int64_t>(interval, MetricsMinIntervalMs, MetricsMaxIntervalMs));
	}
	output = parsed;
	return true;
}

struct MetricValues {
	std::array<std::optional<double>, MetricCount> value {};
	void Set(Metric metric, double v) { value[static_cast<size_t>(metric)] = v; }
};

// {"type":"metrics","t_ms":..,"values":{..}} with only subscribed keys that have a value.
// Returns nullopt when nothing would be reported.
inline std::optional<Json> BuildMetricsMessage(MetricMask subscribed, const MetricValues& values,
                                               uint64_t t_ms) {
	Json out = Json::object();
	for (size_t i = 0; i < MetricCount; i++) {
		if ((subscribed & (MetricMask {1} << i)) == 0 || !values.value[i]) continue;
		const double v = *values.value[i];
		if (MetricCatalog[i].type == "integer")
			out[std::string(MetricCatalog[i].key)] = static_cast<uint64_t>(v < 0 ? 0 : v);
		else
			out[std::string(MetricCatalog[i].key)] = v;
	}
	if (out.empty()) return std::nullopt;
	return Json {{"type", "metrics"}, {"t_ms", t_ms}, {"values", std::move(out)}};
}

// CLOCK_MONOTONIC in milliseconds.
inline uint64_t MonotonicMs() noexcept {
#if defined(CLOCK_MONOTONIC)
	timespec now {};
	clock_gettime(CLOCK_MONOTONIC, &now);
	return static_cast<uint64_t>(now.tv_sec) * 1000 + static_cast<uint64_t>(now.tv_nsec) / 1000000;
#else
	return std::chrono::duration_cast<std::chrono::milliseconds>(
	           std::chrono::steady_clock::now().time_since_epoch())
	    .count();
#endif
}

// Emulator side of the metrics: `subscribed` runs on the transport thread whenever the effective
// key set changes (including 0 on disconnect) so counter collection follows the subscription;
// `sample` reads the values for the subscribed keys.
struct MetricsHooks {
	std::function<void(MetricMask)>               subscribed;
	std::function<MetricValues(MetricMask)>       sample;
};

} // namespace Streaming
#endif
