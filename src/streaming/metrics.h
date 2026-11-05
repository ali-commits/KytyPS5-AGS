#ifndef KYTY_STREAMING_METRICS_H
#define KYTY_STREAMING_METRICS_H
// Live metrics on the AGS session socket (STANDARD section 8). Header only and free of emulator
// dependencies, so the protocol tests build it without the kernel or graphics.
#include "streaming/protocol.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <ctime>
#include <functional>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>

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
	if (!packet.is_object() || !packet.contains("keys") || !packet["keys"].is_array()) return false;
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

// When the next metrics message is due. The interval restarts after every attempt, whether or not
// the send went through: under socket backpressure the source is read once per interval, not on
// every pass of the 2 ms run loop.
class MetricsPacer {
public:
	using Clock = std::chrono::steady_clock;
	[[nodiscard]] bool Due(Clock::time_point now, uint32_t interval_ms) const {
		return !m_last || now - *m_last >= std::chrono::milliseconds(interval_ms);
	}
	void Attempted(Clock::time_point now) { m_last = now; }
	void Reset() { m_last.reset(); }

private:
	std::optional<Clock::time_point> m_last;
};

// A value that is slow or takes locks to read, refreshed off the transport thread by a thread that
// runs only between Start() and Stop(). Latest() is a lock-free read.
class PeriodicSampler {
public:
	explicit PeriodicSampler(std::function<double()>   read,
	                         std::chrono::milliseconds period = std::chrono::seconds(1))
	    : m_read(std::move(read)), m_period(period) {}
	~PeriodicSampler() { Stop(); }
	PeriodicSampler(const PeriodicSampler&)            = delete;
	PeriodicSampler& operator=(const PeriodicSampler&) = delete;

	void Start() {
		if (m_thread.joinable()) return;
		m_thread = std::jthread([this](std::stop_token stop) {
			std::mutex                  mutex;
			std::condition_variable_any wake;
			while (!stop.stop_requested()) {
				m_latest.store(m_read(), std::memory_order_release);
				std::unique_lock lock(mutex);
				wake.wait_for(lock, stop, m_period, [] { return false; });
			}
		});
	}
	void Stop() {
		if (!m_thread.joinable()) return;
		m_thread.request_stop();
		m_thread.join();
		m_latest.store(-1.0, std::memory_order_release);
	}
	[[nodiscard]] std::optional<double> Latest() const {
		const double value = m_latest.load(std::memory_order_acquire);
		return value < 0 ? std::nullopt : std::optional<double> {value};
	}

private:
	std::function<double()>   m_read; // Returns a non-negative value.
	std::chrono::milliseconds m_period;
	std::atomic<double>       m_latest {-1.0};
	std::jthread              m_thread;
};

// shader_compiles counts from the start of the subscription that includes it: a cumulative counter
// is rebased at every subscribe (and again after an unsubscribe), so compiles that happened before
// collection began or while it was off never show up.
class CounterBaseline {
public:
	void Update(bool wanted, uint64_t current) {
		if (wanted && !m_active) m_base = current;
		m_active = wanted;
	}
	[[nodiscard]] uint64_t Since(uint64_t current) const {
		return current > m_base ? current - m_base : 0;
	}

private:
	bool     m_active = false;
	uint64_t m_base   = 0;
};

// Emulator side of the metrics: `subscribed` runs on the transport thread whenever the effective
// key set changes (including 0 on disconnect) so counter collection follows the subscription;
// `sample` reads the values for the subscribed keys.
struct MetricsHooks {
	std::function<void(MetricMask)>         subscribed;
	std::function<MetricValues(MetricMask)> sample;
};

} // namespace Streaming
#endif
