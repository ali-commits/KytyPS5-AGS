#pragma once
#include "streaming/protocol.h"

#include <atomic>
#include <functional>
#include <memory>
#include <thread>
#include <vector>
namespace Streaming {
struct Packet {
	Json             json;
	std::vector<int> fds;
	Packet() = default;
	~Packet();
	Packet(const Packet&)            = delete;
	Packet& operator=(const Packet&) = delete;
};
enum class ReceiveResult { Packet, Again, Closed, Invalid };
ReceiveResult ReceivePacket(int socket, Packet& packet, size_t expected_fds = 0);
bool          SameUser(int socket);
bool          SendPacket(int socket, const Json& packet, const std::array<int, 9>* fds = nullptr);
struct PendingFrame {
	Lease    lease;
	uint64_t tick, pts_ns;
};
class Transport {
public:
	using InputSink  = std::function<void(const Input*)>;
	using MotionSink = std::function<void(const Motion*)>;
	using Completion = std::function<bool(uint64_t)>;
	Transport(std::string path, InputSink input, Completion complete, MotionSink motion = {});
	~Transport();
	uint64_t Generation() const { return m_generation.load(std::memory_order_acquire); }
	bool     Attached() const { return m_attached.load(std::memory_order_acquire); }
	void SetPool(PoolDescriptor descriptor, std::array<int, 9> fds, std::shared_ptr<LeaseRing> ring,
	             uint64_t generation);
	void Publish(PendingFrame frame);
	void Reconnect();
	uint64_t Dropped() const { return m_dropped.load(std::memory_order_relaxed); }
	void     Drop() { m_dropped.fetch_add(1, std::memory_order_relaxed); }

private:
	void                       Run(std::stop_token stop);
	std::string                m_path;
	InputSink                  m_input;
	MotionSink                 m_motion;
	Completion                 m_complete;
	std::mutex                 m_mutex;
	PoolDescriptor             m_descriptor;
	std::array<int, 9>         m_fds {};
	std::shared_ptr<LeaseRing> m_ring;
	struct Mailbox {
		std::atomic<bool> published {false};
		PendingFrame      frame {};
	};
	// A lease owns this slot until the worker has sent FRAME and processed its ACK.
	// Present publishes without acquiring the socket worker's mutex.
	std::array<Mailbox, SlotCount> m_pending;
	uint64_t                       m_pool_generation = 0;
	std::atomic<uint64_t>          m_generation {0}, m_dropped {0}, m_active_epoch {0};
	std::atomic<bool>              m_attached {false}, m_reconnect {false};
	std::jthread                   m_worker;
};
} // namespace Streaming
