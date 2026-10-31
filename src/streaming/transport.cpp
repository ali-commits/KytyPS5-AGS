#include "streaming/transport.h"

#include "streaming/controllerAudio.h"
#include "streaming/feedback.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#if defined(__linux__)
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif
namespace Streaming {
Packet::~Packet() {
#if defined(__linux__)
	for (int fd: fds)
		if (fd >= 0) close(fd);
#endif
}
#if defined(__linux__)
bool SameUser(int socket) {
	ucred     peer {};
	socklen_t size = sizeof(peer);
	return getsockopt(socket, SOL_SOCKET, SO_PEERCRED, &peer, &size) == 0 && size == sizeof(peer) &&
	       peer.uid == geteuid();
}
ReceiveResult ReceivePacket(int socket, Packet& packet, size_t expected_fds) {
	char                  bytes[PacketLimit];
	alignas(cmsghdr) char ancillary[CMSG_SPACE(sizeof(int) * 16)] {};
	iovec                 iov {bytes, sizeof(bytes)};
	msghdr                message {};
	message.msg_iov        = &iov;
	message.msg_iovlen     = 1;
	message.msg_control    = ancillary;
	message.msg_controllen = sizeof(ancillary);
	const auto count       = recvmsg(socket, &message, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
	if (count < 0)
		return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ? ReceiveResult::Again
		                                                                 : ReceiveResult::Closed;
	bool bad = false;
	for (auto* c = CMSG_FIRSTHDR(&message); c; c = CMSG_NXTHDR(&message, c)) {
		if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS ||
		    c->cmsg_len < CMSG_LEN(0)) {
			bad = true;
			continue;
		}
		const size_t size = c->cmsg_len - CMSG_LEN(0);
		if (size % sizeof(int) != 0) bad = true;
		auto* fds = reinterpret_cast<int*>(CMSG_DATA(c));
		for (size_t i = 0; i < size / sizeof(int); i++)
			packet.fds.push_back(fds[i]);
	}
	if (count == 0) return packet.fds.empty() ? ReceiveResult::Closed : ReceiveResult::Invalid;
	if (bad || (message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) || packet.fds.size() != expected_fds)
		return ReceiveResult::Invalid;
	packet.json = Json::parse(bytes, bytes + count, nullptr, false);
	return packet.json.is_object() ? ReceiveResult::Packet : ReceiveResult::Invalid;
}
bool SendPacket(int socket, const Json& packet, const std::array<int, 9>* fds) {
	const std::string data = packet.dump();
	if (data.size() > PacketLimit) return false;
	iovec  iov {const_cast<char*>(data.data()), data.size()};
	msghdr message {};
	message.msg_iov    = &iov;
	message.msg_iovlen = 1;
	alignas(cmsghdr) char ancillary[CMSG_SPACE(sizeof(int) * 9)] {};
	if (fds) {
		message.msg_control    = ancillary;
		message.msg_controllen = sizeof(ancillary);
		auto* c                = CMSG_FIRSTHDR(&message);
		c->cmsg_level          = SOL_SOCKET;
		c->cmsg_type           = SCM_RIGHTS;
		c->cmsg_len            = CMSG_LEN(sizeof(int) * 9);
		std::memcpy(CMSG_DATA(c), fds->data(), sizeof(int) * 9);
	}
	return sendmsg(socket, &message, MSG_DONTWAIT | MSG_NOSIGNAL) ==
	       static_cast<ssize_t>(data.size());
}
static int Connect(const std::string& path) {
	if (path.empty() || path.size() >= sizeof(sockaddr_un::sun_path)) return -1;
	const auto slash = path.rfind('/');
	if (slash == std::string::npos) return -1;
	struct stat status {};
	if (lstat(path.c_str(), &status) != 0 || !S_ISSOCK(status.st_mode) ||
	    status.st_uid != geteuid() || (status.st_mode & 077) != 0)
		return -1;
	if (lstat(path.substr(0, slash).c_str(), &status) != 0 || !S_ISDIR(status.st_mode) ||
	    status.st_uid != geteuid() || (status.st_mode & 077) != 0)
		return -1;
	int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0) return -1;
	sockaddr_un address {};
	address.sun_family = AF_UNIX;
	std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
	if (connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 || !SameUser(fd)) {
		close(fd);
		return -1;
	}
	return fd;
}
#else
bool SameUser(int) {
	return false;
}
ReceiveResult ReceivePacket(int, Packet&, size_t) {
	return ReceiveResult::Closed;
}
bool SendPacket(int, const Json&, const std::array<int, 9>*) {
	return false;
}
#endif
Transport::Transport(std::string path, InputSink input, Completion complete, MotionSink motion)
    : m_path(std::move(path)), m_input(std::move(input)), m_motion(std::move(motion)),
      m_complete(std::move(complete)) {
	m_worker = std::jthread([this](std::stop_token stop) { Run(stop); });
}
Transport::~Transport() {
	m_worker.request_stop();
	m_worker.join();
}
void Transport::SetPool(PoolDescriptor descriptor, std::array<int, 9> fds,
                        std::shared_ptr<LeaseRing> ring, uint64_t generation) {
	std::lock_guard lock(m_mutex);
	m_descriptor      = std::move(descriptor);
	m_fds             = fds;
	m_ring            = std::move(ring);
	m_pool_generation = generation;
	for (auto& pending: m_pending)
		pending.published.store(false, std::memory_order_relaxed);
	m_active_epoch.store(m_descriptor.epoch, std::memory_order_release);
}
void Transport::Publish(PendingFrame frame) {
	if (frame.lease.slot >= SlotCount ||
	    m_active_epoch.load(std::memory_order_acquire) != frame.lease.epoch) {
		Drop();
		return;
	}
	auto& pending = m_pending[frame.lease.slot];
	pending.frame = frame;
	pending.published.store(true, std::memory_order_release);
}
void Transport::Reconnect() {
	m_reconnect.store(true, std::memory_order_release);
}
void Transport::Run(std::stop_token stop) {
#if defined(__linux__)
	int         socket     = -1;
	uint64_t    generation = 0, input_sequence = 0, rumble_revision = 0;
	bool        welcomed = false, hello_sent = false, motion_cap = false, adaptive_cap = false,
	            audio_cap            = false;
	uint64_t    audio_generation     = 0;
	bool        audio_ready_proposed = false;
	MotionOrder motion_order;
	uint64_t    trigger_revision = 0, motion_received = 0, motion_applied = 0, trigger_sent = 0;
	auto        retry         = std::chrono::steady_clock::now();
	auto        last_feedback = retry;
	auto        last_trigger  = retry;
	auto        last_status   = retry;
	uint64_t    last_sequence = 0;
	const auto  disconnect    = [&] {
		ControllerAudio::Disable();
		audio_generation     = 0;
		audio_ready_proposed = false;
		audio_cap            = false;
		feedback_enabled     = false;
		adaptive_enabled     = false;
		ClearAdaptiveTriggers();
		motion_order.Reset();
		motion_cap = adaptive_cap = false;
		feedback_value            = 0;
		feedback_revision.fetch_add(1, std::memory_order_release);
		m_active_epoch = 0;
		m_attached     = false;
		m_generation   = 0;
		if (socket >= 0) close(socket);
		socket         = -1;
		welcomed       = false;
		hello_sent     = false;
		input_sequence = 0;
		{
			std::lock_guard lock(m_mutex);
			if (m_ring) m_ring->Disconnect();
			m_ring.reset();
			for (auto& pending: m_pending)
				pending.published.store(false, std::memory_order_relaxed);
			m_pool_generation = 0;
		}
		m_input(nullptr);
		if (m_motion) m_motion(nullptr);
		retry = std::chrono::steady_clock::now() + std::chrono::seconds(1);
	};
	while (!stop.stop_requested()) {
		if (m_reconnect.exchange(false)) disconnect();
		const auto now = std::chrono::steady_clock::now();
		if (socket < 0) {
			if (now >= retry) {
				socket = Connect(m_path);
				retry  = now + std::chrono::seconds(1);
			}
			if (socket < 0) {
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
				continue;
			}
		}
		pollfd poller {socket, POLLIN, 0};
		if (poll(&poller, 1, 2) < 0 && errno != EINTR) {
			disconnect();
			continue;
		}
		if (poller.revents & (POLLERR | POLLHUP | POLLNVAL)) {
			disconnect();
			continue;
		}
		bool invalid = false;
		for (size_t count = 0; count < 32; count++) {
			Packet     packet;
			const auto result = ReceivePacket(socket, packet);
			if (result == ReceiveResult::Again) break;
			if (result != ReceiveResult::Packet) {
				invalid = true;
				break;
			}
			const auto& p = packet.json;
			if (!p.contains("type") || !p["type"].is_string()) {
				invalid = true;
				break;
			}
			const auto type = p["type"].get<std::string>();
			if (!welcomed) {
				if (type != "welcome" || !p.contains("version") ||
				    !p["version"].is_number_integer() || p["version"] != 1) {
					invalid = true;
					break;
				}
				audio_cap    = HasCapability(p, "controller-audio-v1");
				motion_cap   = HasCapability(p, "motion-v1");
				adaptive_cap = HasCapability(p, "adaptive-triggers-v1");
				welcomed     = true;
				m_generation = ++generation;
				continue;
			}
			if (type == "accepted" && !m_attached && hello_sent) {
				std::lock_guard lock(m_mutex);
				if (!p.contains("epoch") || !p["epoch"].is_number_unsigned() ||
				    p["epoch"] != m_descriptor.epoch || !p.contains("version") ||
				    !p["version"].is_number_integer() || p["version"] != 1 ||
				    !p.contains("transport") || p["transport"] != "vulkan-buffer-fd") {
					invalid = true;
					break;
				}
				audio_cap        = audio_cap && HasCapability(p, "controller-audio-v1");
				motion_cap       = motion_cap && HasCapability(p, "motion-v1");
				adaptive_cap     = adaptive_cap && HasCapability(p, "adaptive-triggers-v1");
				m_attached       = true;
				feedback_enabled = true;
				rumble_revision  = feedback_revision.load(std::memory_order_acquire);
				trigger_revision = ReadAdaptiveTriggers().revision;
				adaptive_enabled = adaptive_cap;
				continue;
			}
			if (type == "error") {
				invalid = true;
				break;
			}
			if (!m_attached) {
				invalid = true;
				break;
			}
			if (type == "controller-audio-state" && audio_cap) {
				ControllerAudioState state;
				if (!ParseControllerAudioState(p, state)) {
					invalid = true;
					break;
				}
				// Preparing and active share a binding generation. Accept its readiness
				// transition once while ignoring duplicate state and older bindings.
				if (state.generation > audio_generation || (state.generation == audio_generation &&
				                                            state.ready && !audio_ready_proposed)) {
					audio_generation     = state.generation;
					audio_ready_proposed = state.ready;
					ControllerAudio::Configure(
					    {state.path, state.nonce_hex, state.generation, state.producer_epoch},
					    state.ready);
				}
			} else if (type == "release") {
				if (!p.contains("epoch") || !p.contains("slot") || !p.contains("sequence") ||
				    !p["epoch"].is_number_unsigned() || !p["slot"].is_number_unsigned() ||
				    !p["sequence"].is_number_unsigned() || p["slot"].get<uint64_t>() >= SlotCount) {
					invalid = true;
					break;
				}
				std::lock_guard lock(m_mutex);
				if (m_ring) m_ring->Release(p["epoch"], p["slot"], p["sequence"]);
			} else if (type == "input") {
				Input input;
				if (!ParseInput(p, input)) {
					invalid = true;
					break;
				}
				if (input.sequence > input_sequence) {
					input_sequence = input.sequence;
					m_input(&input);
					if (!input.connected) {
						motion_order.Reset();
						if (m_motion) m_motion(nullptr);
						ClearAdaptiveTriggers();
					}
				}
			} else if (type == "motion" && motion_cap) {
				Motion motion;
				if (!ParseMotion(p, motion)) {
					invalid = true;
					break;
				}
				++motion_received;
				if (motion_order.Accept(motion) && m_motion) {
					m_motion(&motion);
					++motion_applied;
				}
			} else if (type == "reset") {
				if (!p.contains("controller") || !p["controller"].is_number_integer() ||
				    p["controller"] != 0) {
					invalid = true;
					break;
				}
				feedback_value = 0;
				feedback_revision.fetch_add(1, std::memory_order_release);
				m_input(nullptr);
				motion_order.Reset();
				if (m_motion) m_motion(nullptr);
				ClearAdaptiveTriggers();
			} else {
				invalid = true;
				break;
			}
		}
		if (invalid) {
			disconnect();
			continue;
		}
		if (welcomed && !hello_sent) {
			std::lock_guard lock(m_mutex);
			if (m_ring && m_pool_generation == generation) {
				const auto& d     = m_descriptor;
				Json        hello = {{"type", "hello"},
				                     {"version", 1},
				                     {"epoch", d.epoch},
				                     {"width", d.width},
				                     {"height", d.height},
				                     {"stride", d.stride},
				                     {"fourcc", "BGRA"},
				                     {"gpu_uuid", d.gpu_uuid},
				                     {"slots", 3},
				                     {"allocation_sizes", d.allocation_sizes},
				                     {"offset", 0},
				                     {"dedicated", true},
				                     {"transport", "vulkan-buffer-fd"}};
				if (motion_cap || adaptive_cap || audio_cap) {
					hello["capabilities"] = Json::array();
					if (audio_cap) hello["capabilities"].push_back("controller-audio-v1");
					if (motion_cap) hello["capabilities"].push_back("motion-v1");
					if (adaptive_cap) hello["capabilities"].push_back("adaptive-triggers-v1");
				}
				if (SendPacket(socket, hello, &m_fds)) hello_sent = true;
			}
		}
		if (m_attached) {
			std::lock_guard lock(m_mutex);
			for (size_t count = 0; count < SlotCount; count++) {
				Mailbox* next = nullptr;
				for (auto& pending: m_pending) {
					if (pending.published.load(std::memory_order_acquire) &&
					    (next == nullptr ||
					     pending.frame.lease.sequence < next->frame.lease.sequence))
						next = &pending;
				}
				if (next == nullptr || !m_complete(next->frame.tick)) break;
				const auto& pending = next->frame;
				Json        frame   = {{"type", "frame"},
				                       {"epoch", pending.lease.epoch},
				                       {"slot", pending.lease.slot},
				                       {"sequence", pending.lease.sequence},
				                       {"pts_ns", pending.pts_ns}};
				if (!SendPacket(socket, frame)) break;
				last_sequence = pending.lease.sequence;
				next->published.store(false, std::memory_order_release);
			}
			if (now - last_status >= std::chrono::seconds(1)) {
				Json status = {{"type", "status"},
				               {"state", "attached"},
				               {"sequence", last_sequence},
				               {"dropped", Dropped()},
				               {"motion_received", motion_received},
				               {"motion_applied", motion_applied},
				               {"adaptive_sent", trigger_sent}};
				if (audio_cap) {
					const auto audio           = ControllerAudio::GetStatus();
					status["controller_audio"] = {{"ready", audio.ready},
					                              {"bytes", audio.bytes},
					                              {"discontinuities", audio.discontinuities}};
				}
				if (SendPacket(socket, status)) last_status = now;
			}
			if (adaptive_cap && now - last_trigger >= std::chrono::milliseconds(16)) {
				const auto effect = ReadAdaptiveTriggers();
				if (effect.revision != trigger_revision &&
				    SendPacket(socket, {{"type", "adaptive-triggers"},
				                        {"controller", 0},
				                        {"sequence", effect.revision},
				                        {"enable_bits", effect.enable_bits},
				                        {"left", effect.left},
				                        {"right", effect.right}})) {
					trigger_revision = effect.revision;
					last_trigger     = now;
					++trigger_sent;
				}
			}
			const auto revision = feedback_revision.load(std::memory_order_acquire);
			if (revision != rumble_revision &&
			    now - last_feedback >= std::chrono::milliseconds(16)) {
				const auto value = feedback_value.load(std::memory_order_relaxed);
				if (SendPacket(socket, {{"type", "rumble"},
				                        {"controller", 0},
				                        {"large", value & 65535},
				                        {"small", (value >> 16) & 65535},
				                        {"duration_ms", (value >> 32) & 1023}})) {
					rumble_revision = revision;
					last_feedback   = now;
				}
			}
		}
	}
	disconnect();
#else
	(void)stop;
#endif
}
} // namespace Streaming
