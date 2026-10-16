#include "streaming/controllerAudio.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#if defined(__linux__)
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif
namespace Streaming::ControllerAudio {
namespace {
using namespace ::ControllerAudio;
uint64_t Now() {
	return std::chrono::duration_cast<std::chrono::microseconds>(
	           std::chrono::steady_clock::now().time_since_epoch())
	    .count();
}
struct Port {
	bool     active     = false;
	Kind     kind       = Kind::Control;
	uint32_t rate       = 0;
	uint64_t generation = 0, sequence = 0, position = 0;
	uint32_t queued = 0;
};
struct Entry {
	Header                                          header {};
	uint32_t                                        bytes = 0, frames = 0;
	std::array<float, PayloadLimit / sizeof(float)> samples {};
};
class Producer {
public:
	std::atomic<bool>       ready {false};
	std::atomic<uint64_t>   bytes {0}, discontinuities {0};
	std::mutex              mutex;
	std::condition_variable wake;
	Endpoint                endpoint {};
	uint64_t                revision   = 0;
	bool                    configured = false;
	std::array<Port, 32>    ports {};
	std::array<Entry, 128>  queue {};
	size_t                  head = 0, count = 0;
	std::jthread            worker;
	Producer(): worker([this](std::stop_token stop) { Run(stop); }) {}
	~Producer() {
		worker.request_stop();
		wake.notify_one();
		worker.join();
	}
	void Withdraw(bool failure = true) noexcept {
		if (ready.exchange(false, std::memory_order_acq_rel) && failure)
			discontinuities.fetch_add(1, std::memory_order_relaxed);
		wake.notify_one();
	}
	void Clear() {
		head = count = 0;
		for (auto& p: ports) {
			p.queued   = 0;
			p.sequence = p.position = 0;
		}
	}
	Header Make(Type type, uint32_t id, const Port& p) {
		return {type,
		        p.kind,
		        2,
		        endpoint.generation,
		        endpoint.producer_epoch,
		        id,
		        p.rate,
		        p.generation,
		        p.sequence,
		        p.position,
		        0};
	}
	bool Push(const Header& h) {
		if (count == queue.size()) {
			Withdraw();
			return false;
		}
		auto& e  = queue[(head + count++) % queue.size()];
		e.header = h;
		e.bytes = e.frames = 0;
		return true;
	}
	void Run(std::stop_token stop);
};
static_assert(sizeof(Producer) + sizeof(Entry) + PayloadLimit < 4 * 1024 * 1024,
              "Controller PCM source storage must remain bounded");
Producer& State() {
	static Producer state;
	return state;
}
#if defined(__linux__)
int Connect(const Endpoint& ep) {
	if (ep.path.empty() || ep.path.size() >= sizeof(sockaddr_un::sun_path)) return -1;
	const auto slash = ep.path.rfind('/');
	if (slash == std::string::npos) return -1;
	struct stat status {}, before {};
	if (lstat(ep.path.c_str(), &before) != 0 || !S_ISSOCK(before.st_mode) ||
	    before.st_uid != geteuid() || (before.st_mode & 0777) != 0600)
		return -1;
	if (lstat(ep.path.substr(0, slash).c_str(), &status) != 0 || !S_ISDIR(status.st_mode) ||
	    status.st_uid != geteuid() || (status.st_mode & 0777) != 0700)
		return -1;
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0) return -1;
	sockaddr_un address {};
	address.sun_family = AF_UNIX;
	std::memcpy(address.sun_path, ep.path.c_str(), ep.path.size() + 1);
	ucred     peer {};
	socklen_t size = sizeof(peer);
	if (connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
	    getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &size) != 0 || size != sizeof(peer) ||
	    peer.uid != geteuid() || lstat(ep.path.c_str(), &status) != 0 ||
	    status.st_ino != before.st_ino || status.st_dev != before.st_dev) {
		close(fd);
		return -1;
	}
	// Keep kernel buffering bounded as well as the preallocated source queue.
	const int send_bytes = 4096;
	if (setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &send_bytes, sizeof(send_bytes)) != 0) {
		close(fd);
		return -1;
	}
	return fd;
}
bool Transfer(int fd, std::span<uint8_t> bytes, bool sending, uint64_t deadline) {
	size_t at = 0;
	while (at < bytes.size()) {
		if (Now() > deadline) return false;
		auto n = sending
		             ? send(fd, bytes.data() + at, bytes.size() - at, MSG_DONTWAIT | MSG_NOSIGNAL)
		             : recv(fd, bytes.data() + at, bytes.size() - at, MSG_DONTWAIT);
		if (n > 0) {
			at += size_t(n);
			continue;
		}
		if (n == 0) return false;
		if (errno == EINTR) continue;
		if (errno != EAGAIN && errno != EWOULDBLOCK) return false;
		pollfd p {fd, short(sending ? POLLOUT : POLLIN), 0};
		if (poll(&p, 1, 2) < 0 && errno != EINTR) return false;
		if (p.revents & (POLLERR | POLLHUP | POLLNVAL)) return false;
	}
	return true;
}
bool Send(int fd, const Frame& f, uint64_t deadline) {
	auto bytes = Encode(f);
	if (bytes.empty() || !Transfer(fd, bytes, true, deadline)) return false;
	// A successful send can still leave old PCM inside the Unix socket. Bound that
	// age without involving the audio callback or adding protocol acknowledgements.
	for (;;) {
		int pending = 0;
		if (ioctl(fd, TIOCOUTQ, &pending) != 0) return false;
		if (pending == 0) return true;
		if (Now() > deadline) return false;
		pollfd p {fd, POLLIN, 0};
		if (poll(&p, 1, 1) < 0 && errno != EINTR) return false;
		if (p.revents & (POLLERR | POLLHUP | POLLNVAL)) return false;
	}
}
std::optional<Frame> Receive(int fd, uint64_t deadline) {
	std::vector<uint8_t> bytes(HeaderBytes);
	if (!Transfer(fd, bytes, false, deadline)) return {};
	auto size = WireSize(bytes);
	if (!size) return {};
	bytes.resize(*size);
	if (!Transfer(fd, std::span(bytes).subspan(HeaderBytes), false, deadline)) return {};
	return Decode(bytes);
}
#endif
void Producer::Run(std::stop_token stop) {
#if defined(__linux__)
	int      fd    = -1;
	uint64_t bound = 0, reported = 0;
	while (!stop.stop_requested()) {
		const auto failures = discontinuities.load(std::memory_order_relaxed);
		if (failures != reported) {
			reported = failures;
			std::fprintf(stderr,
			             "ControllerAudio: PCM lease discontinued; restoring controller fallback "
			             "(count %llu)\n",
			             static_cast<unsigned long long>(failures));
		}
		Endpoint next;
		uint64_t next_revision = 0;
		bool     connect       = false;
		int      retired       = -1;
		{
			std::unique_lock lock(mutex);
			wake.wait_for(lock, std::chrono::milliseconds(2), [&] {
				return stop.stop_requested() || revision != bound || count ||
				       (fd >= 0 && !ready.load());
			});
			if (stop.stop_requested()) break;
			if (revision != bound || (fd >= 0 && !ready.load())) {
				retired = fd;
				fd      = -1;
				Clear();
				bound = revision;
				if (configured) {
					next          = std::move(endpoint);
					next_revision = revision;
					connect       = true;
					configured    = false;
				}
			}
		}
		if (retired >= 0) close(retired);
		if (connect) {
			fd = Connect(next);
			std::vector<uint8_t> nonce;
			for (size_t i = 0; i < next.nonce_hex.size(); i += 2) {
				auto digit = [](char c) -> unsigned { return c <= '9' ? c - '0' : c - 'a' + 10; };
				nonce.push_back(
				    uint8_t(digit(next.nonce_hex[i]) * 16 + digit(next.nonce_hex[i + 1])));
			}
			const auto deadline = Now() + 20000;
			bool       accepted = fd >= 0 && Send(fd,
			                                      {{Type::Hello, Kind::Control, 0, next.generation,
			                                        next.producer_epoch, 0, 0, 0, 0, 0, 0},
			                                       std::move(nonce)},
			                                      deadline);
			auto       reply    = accepted ? Receive(fd, deadline) : std::optional<Frame> {};
			accepted            = reply && reply->header.type == Type::Ready &&
			                      reply->header.generation == next.generation &&
			                      reply->header.producer_epoch == next.producer_epoch;
			bool keep           = false;
			{
				std::lock_guard lock(mutex);
				if (accepted && revision == next_revision) {
					for (uint32_t id = 0; id < ports.size(); id++)
						if (ports[id].active) {
							auto& p = ports[id];
							++p.sequence;
							Push(Make(Type::Open, id, p));
						}
					ready.store(true, std::memory_order_release);
					keep = true;
				}
			}
			if (!keep) {
				if (fd >= 0) close(fd);
				fd = -1;
			}
		}
		if (fd < 0) continue;
		pollfd check {fd, POLLIN, 0};
		if (poll(&check, 1, 0) > 0 && check.revents) {
			Withdraw();
			continue;
		}
		Entry entry;
		bool  have = false;
		{
			std::lock_guard lock(mutex);
			if (ready.load() && count) {
				entry = queue[head];
				head  = (head + 1) % queue.size();
				--count;
				have = true;
			}
		}
		if (have) {
			const auto deadline = entry.header.type == Type::Pcm
			                          ? entry.header.source_time_us + 20000
			                          : Now() + 20000;
			Frame      frame {entry.header, std::vector<uint8_t>(entry.bytes)};
			for (size_t i = 0; i < size_t(entry.frames) * 2; i++) {
				const auto bits = std::bit_cast<uint32_t>(entry.samples[i]);
				for (unsigned j = 0; j < 4; j++)
					frame.payload[i * 4 + j] = uint8_t(bits >> (8 * j));
			}
			bool sent = Send(fd, frame, deadline);
			if (sent) bytes.fetch_add(entry.bytes, std::memory_order_relaxed);
			{
				std::lock_guard lock(mutex);
				auto&           p = ports[entry.header.port];
				if (p.generation == entry.header.port_generation)
					p.queued -= std::min(p.queued, entry.frames);
				if (!sent) Withdraw();
			}
		}
	}
	if (fd >= 0) close(fd);
#else
	(void)stop;
#endif
	ready = false;
}
} // namespace
void Configure(const Endpoint& endpoint, bool ready) {
	Endpoint prepared = endpoint; // Allocate/copy strings before taking the bounded local lock.
	auto&    s        = State();
	{
		std::lock_guard lock(s.mutex);
		s.Withdraw(false);
		++s.revision;
		s.Clear();
		std::swap(s.endpoint, prepared);
		s.configured =
		    ready && endpoint.generation && endpoint.producer_epoch &&
		    endpoint.nonce_hex.size() == 64 &&
		    endpoint.nonce_hex.find_first_not_of("0123456789abcdef") == std::string::npos;
		s.wake.notify_one();
	} // Any old endpoint strings are freed after releasing the local publisher lock.
}
void Disable() noexcept {
	auto&           s = State();
	std::lock_guard lock(s.mutex);
	s.Withdraw(false);
	s.configured = false;
	++s.revision;
	s.Clear();
	s.wake.notify_one();
}
bool Ready() noexcept {
	return State().ready.load(std::memory_order_acquire);
}
Status GetStatus() noexcept {
	auto& s = State();
	return {s.ready.load(std::memory_order_acquire), s.bytes.load(std::memory_order_relaxed),
	        s.discontinuities.load(std::memory_order_relaxed)};
}
void OpenPort(uint32_t id, uint64_t generation, ::ControllerAudio::Kind kind, uint32_t rate) {
	auto&           s = State();
	std::lock_guard lock(s.mutex);
	if (id >= s.ports.size() || !generation || (kind != Kind::Actuator && kind != Kind::Speaker) ||
	    rate < 1000 || rate > 192000) {
		s.Withdraw();
		return;
	}
	auto& p = s.ports[id];
	if (p.active || generation <= p.generation) {
		s.Withdraw();
		return;
	}
	p = {true, kind, rate, generation};
	if (s.ready.load()) {
		++p.sequence;
		s.Push(s.Make(Type::Open, id, p));
		s.wake.notify_one();
	}
}
bool Publish(uint32_t id, uint64_t generation, ::ControllerAudio::Kind kind,
             const ::ControllerAudio::PcmView& view) noexcept {
	auto& s = State();
	if (!s.ready.load(std::memory_order_acquire)) return false;
	// The worker holds this local lock only to move metadata or copy bounded storage.
	// It never allocates, encodes, performs I/O or waits for a peer while holding it.
	std::lock_guard lock(s.mutex);
	if (!s.ready.load()) return false;
	if (id >= s.ports.size()) {
		s.Withdraw();
		return false;
	}
	auto&          p     = s.ports[id];
	const uint32_t chunk = std::min<uint32_t>((view.rate + 99) / 100, PayloadLimit / 8);
	if (!p.active || p.generation != generation || p.kind != kind || p.rate != view.rate ||
	    !view.frames || !chunk || view.frames > (p.rate + 49) / 50 ||
	    p.queued + view.frames > (p.rate + 49) / 50 ||
	    (view.frames + chunk - 1) / chunk > s.queue.size() - s.count ||
	    p.position > UINT64_MAX - view.frames ||
	    p.sequence > UINT64_MAX - (view.frames + chunk - 1) / chunk) {
		s.Withdraw();
		return false;
	}
	const auto                          now = Now();
	std::array<float, PayloadLimit / 4> converted {};
	for (uint32_t offset = 0; offset < view.frames;) {
		auto part   = view;
		part.frames = std::min(chunk, view.frames - offset);
		if (!view.data) {
			s.Withdraw();
			return false;
		}
		part.data = static_cast<const uint8_t*>(view.data) +
		            size_t(offset) * view.channels * (view.is_float ? 4 : 2);
		if (!ConvertStereo(part, converted)) {
			s.Withdraw();
			return false;
		}
		auto& e = s.queue[(s.head + s.count++) % s.queue.size()];
		++p.sequence;
		e.header                = s.Make(Type::Pcm, id, p);
		e.header.source_time_us = now;
		e.frames                = part.frames;
		e.bytes                 = part.frames * 8;
		std::copy_n(converted.begin(), size_t(part.frames) * 2, e.samples.begin());
		p.position += part.frames;
		p.queued += part.frames;
		offset += part.frames;
	}
	s.wake.notify_one();
	return true;
}
void ClosePort(uint32_t id, uint64_t generation) noexcept {
	auto&           s = State();
	std::lock_guard lock(s.mutex);
	if (id >= s.ports.size()) return;
	auto& p = s.ports[id];
	if (!p.active || p.generation != generation) return;
	if (s.ready.load()) {
		if (p.sequence == UINT64_MAX)
			s.Withdraw();
		else {
			++p.sequence;
			s.Push(s.Make(Type::Close, id, p));
		}
	}
	p.active = false;
	s.wake.notify_one();
}
} // namespace Streaming::ControllerAudio
