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
	bool               connected = false;
};
bool ParseInput(const Json& packet, Input& output) noexcept;
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
