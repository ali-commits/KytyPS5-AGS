#pragma once

#include <array>
#include <cstdint>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>

namespace Streaming {
constexpr size_t PacketLimit = 8192;
constexpr size_t SlotCount   = 3;
using Json                   = nlohmann::json;
struct PoolDescriptor {
	uint64_t                        epoch = 0;
	uint32_t                        width = 0, height = 0, stride = 0;
	std::array<uint64_t, SlotCount> allocation_sizes {};
	std::string                     gpu_uuid;
};
bool ValidateHello(const Json& packet, size_t fd_count, PoolDescriptor& output) noexcept;
struct Input {
	uint64_t           sequence = 0;
	uint32_t           buttons  = 0;
	std::array<int, 6> axes {128, 128, 128, 128, 0, 0};
	bool               connected               = false;
	uint8_t            controller_type         = 0;
	uint16_t           controller_capabilities = 0;
	uint32_t           supported_buttons       = 0;
};
bool ParseInput(const Json& packet, Input& output) noexcept;
// Optional v1 controller extensions. All motion values at this boundary are SI.
struct Motion {
	enum class Sensor { Accel, Gyro };
	Sensor               sensor   = Sensor::Accel;
	uint64_t             sequence = 0, timestamp_us = 0;
	std::array<float, 3> value {};
};
bool ParseMotion(const Json& packet, Motion& output) noexcept;
bool HasCapability(const Json& packet, std::string_view name) noexcept;
struct ControllerAudioState {
	bool        ready      = false;
	uint64_t    generation = 0, producer_epoch = 0;
	std::string path, nonce_hex;
};
bool ParseControllerAudioState(const Json&, ControllerAudioState&) noexcept;
class MotionOrder {
public:
	bool Accept(const Motion& motion) noexcept {
		const auto index = motion.sensor == Motion::Sensor::Gyro ? 1U : 0U;
		if (motion.sequence <= m_sequence[index] || motion.timestamp_us <= m_time[index])
			return false;
		m_sequence[index] = motion.sequence;
		m_time[index]     = motion.timestamp_us;
		return true;
	}
	void Reset() noexcept {
		m_sequence = {};
		m_time     = {};
	}

private:
	std::array<uint64_t, 2> m_sequence {}, m_time {};
};
struct Lease {
	uint64_t epoch, sequence;
	uint32_t slot;
	bool     recycled;
};
class LeaseRing {
public:
	explicit LeaseRing(uint64_t epoch): m_epoch(epoch) {}
	std::optional<Lease> TryAcquire();
	bool                 Release(uint64_t epoch, uint32_t slot, uint64_t sequence);
	void                 Disconnect();

private:
	friend struct LeaseRingTestAccess;
	std::mutex                      m_mutex;
	uint64_t                        m_epoch, m_sequence = 0;
	bool                            m_connected = true;
	std::array<uint64_t, SlotCount> m_leased {};
	std::array<bool, SlotCount>     m_used {};
};
} // namespace Streaming
