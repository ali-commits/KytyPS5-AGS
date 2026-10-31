#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
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
// Each side retains its last translated report: a right-only update must not erase the left.
struct AdaptiveTriggers {
	uint8_t                 enable_bits = 0;
	std::array<uint8_t, 11> left {}, right {};
	uint64_t                revision = 0;
};
inline std::atomic<bool> adaptive_enabled {false};
inline std::mutex        adaptive_mutex;
inline AdaptiveTriggers  adaptive_value;
inline void              PublishAdaptiveTriggers(uint8_t mask, const uint8_t* left,
                                                 const uint8_t* right) noexcept {
	if (!adaptive_enabled.load(std::memory_order_relaxed) || !(mask & 0x0c) || (mask & ~0x0c))
		return;
	std::lock_guard lock(adaptive_mutex);
	if (mask & 0x08) std::copy_n(left, 11, adaptive_value.left.begin());
	if (mask & 0x04) std::copy_n(right, 11, adaptive_value.right.begin());
	adaptive_value.enable_bits |= mask;
	adaptive_value.revision++;
}
inline AdaptiveTriggers ReadAdaptiveTriggers() noexcept {
	std::lock_guard lock(adaptive_mutex);
	return adaptive_value;
}
inline void ClearAdaptiveTriggers() noexcept {
	std::lock_guard lock(adaptive_mutex);
	const auto      revision   = adaptive_value.revision + 1;
	adaptive_value             = {};
	adaptive_value.enable_bits = 0x0c;
	adaptive_value.left[0] = adaptive_value.right[0] = 0x05;
	adaptive_value.revision                          = revision;
}
} // namespace Streaming
