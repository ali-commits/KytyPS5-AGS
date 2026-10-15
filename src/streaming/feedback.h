#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>
namespace Streaming {
// Producers only publish a bounded latest value. The socket worker owns all I/O.
inline std::atomic<bool>     feedback_enabled {false};
inline std::atomic<uint64_t> feedback_value {0};
inline std::atomic<uint64_t> feedback_revision {0};
inline void PublishRumble(uint16_t large, uint16_t small, uint32_t duration_ms) noexcept {
	if (!feedback_enabled.load(std::memory_order_relaxed)) return;
	feedback_value.store(uint64_t(large) | (uint64_t(small) << 16) |
	                         (uint64_t(std::min(duration_ms, 1000U)) << 32),
	                     std::memory_order_relaxed);
	feedback_revision.fetch_add(1, std::memory_order_release);
}
} // namespace Streaming
