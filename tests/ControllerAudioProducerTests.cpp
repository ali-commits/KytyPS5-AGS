#ifdef NDEBUG
#undef NDEBUG
#endif
#include "controller_audio/protocol.h"
#include "streaming/controllerAudio.h"

#include <array>
#include <bit>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
using namespace ControllerAudio;
namespace Producer = Streaming::ControllerAudio;
static bool WaitReady(bool expected) {
	for (int i = 0; i < 500; i++) {
		if (Producer::Ready() == expected) return true;
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	return false;
}
static Frame ReadFrame(int fd) {
	std::vector<uint8_t> data(HeaderBytes);
	size_t               at   = 0;
	auto                 read = [&](size_t end) {
		while (at < end) {
			pollfd p {fd, POLLIN, 0};
			assert(poll(&p, 1, 1000) > 0);
			auto n = recv(fd, data.data() + at, end - at, 0);
			assert(n > 0);
			at += n;
		}
	};
	read(HeaderBytes);
	const auto total = WireSize(data);
	assert(total);
	data.resize(*total);
	read(*total);
	auto f = Decode(data);
	assert(f);
	return *f;
}
static void Send(int fd, const Frame& f) {
	auto bytes = Encode(f);
	assert(!bytes.empty());
	assert(send(fd, bytes.data(), bytes.size(), MSG_NOSIGNAL) == ssize_t(bytes.size()));
}
struct Server {
	std::string dir, path;
	int         fd = -1, peer = -1;
	Server() {
		char tmp[] = "/tmp/kyty-pcm-XXXXXX";
		dir        = mkdtemp(tmp);
		path       = dir + "/audio.sock";
		fd         = socket(AF_UNIX, SOCK_STREAM, 0);
		assert(fd >= 0);
		sockaddr_un a {};
		a.sun_family = AF_UNIX;
		std::strcpy(a.sun_path, path.c_str());
		assert(bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0);
		chmod(path.c_str(), 0600);
		assert(listen(fd, 4) == 0);
	}
	void Accept(uint64_t generation, bool valid = true, bool fragmented = false) {
		pollfd p {fd, POLLIN, 0};
		assert(poll(&p, 1, 1000) > 0);
		peer = accept(fd, nullptr, nullptr);
		assert(peer >= 0);
		auto hello = ReadFrame(peer);
		assert(hello.header.type == Type::Hello && hello.header.generation == generation &&
		       hello.header.producer_epoch == 7);
		assert(hello.payload == std::vector<uint8_t>(32, 0xaa));
		assert(!Producer::Ready());
		Frame ack {{Type::Ready, Kind::Control, 0, valid ? generation : generation - 1, 7, 0, 0, 0,
		            1, 0, 0},
		           {}};
		if (fragmented) {
			auto bytes = Encode(ack);
			assert(send(peer, bytes.data(), 17, MSG_NOSIGNAL) == 17);
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
			assert(send(peer, bytes.data() + 17, bytes.size() - 17, MSG_NOSIGNAL) ==
			       ssize_t(bytes.size() - 17));
		} else
			Send(peer, ack);
		if (valid)
			assert(WaitReady(true));
		else {
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
			assert(!Producer::Ready());
		}
	}
	~Server() {
		Producer::Disable();
		if (peer >= 0) close(peer);
		close(fd);
		std::filesystem::remove_all(dir);
	}
};
int main() {
	Server            s;
	const std::string nonce(64, 'a');
	Producer::OpenPort(1, 1, Kind::Actuator, 48000);
	int     volume[] {32768, 32768};
	float   samples[] {0.5f, -0.25f, 0.75f, -0.125f};
	PcmView view {samples, 2, 2, 48000, true, volume, 1, nullptr};
	assert(!Producer::Publish(1, 1, Kind::Actuator, view));
	chmod(s.path.c_str(), 0666);
	Producer::Configure({s.path, nonce, 1, 7}, true);
	std::this_thread::sleep_for(std::chrono::milliseconds(30));
	pollfd p {s.fd, POLLIN, 0};
	assert(poll(&p, 1, 0) == 0);
	assert(!Producer::Ready());
	chmod(s.path.c_str(), 0600);
	Producer::Configure({s.path, nonce, 2, 7}, true);
	s.Accept(2);
	auto open = ReadFrame(s.peer);
	assert(open.header.type == Type::Open && open.header.port_generation == 1);
	assert(Producer::Publish(1, 1, Kind::Actuator, view));
	auto pcm = ReadFrame(s.peer);
	assert(pcm.header.type == Type::Pcm && pcm.header.first_frame == 0 &&
	       pcm.header.source_time_us > 0);
	assert((pcm.payload == std::vector<uint8_t> {0, 0, 0, 0x3f, 0, 0, 0x80, 0xbe, 0, 0, 0x40, 0x3f,
	                                             0, 0, 0, 0xbe}));
	Producer::ClosePort(1, 1);
	auto closed = ReadFrame(s.peer);
	assert(closed.header.type == Type::Close && closed.header.sequence > pcm.header.sequence);
	Producer::OpenPort(1, 2, Kind::Speaker, 48000);
	open = ReadFrame(s.peer);
	assert(open.header.port_generation == 2 && open.header.kind == Kind::Speaker);
	assert(!Producer::Publish(1, 1, Kind::Speaker, view));
	assert(!Producer::Ready());
	Producer::Configure({s.path, nonce, 3, 7}, true);
	close(s.peer);
	s.peer = -1;
	s.Accept(3);
	open = ReadFrame(s.peer);
	assert(open.header.port_generation == 2 && open.header.first_frame == 0);
	std::vector<float> oversized(48000 / 50 * 2 + 2);
	PcmView            huge {oversized.data(), 961, 2, 48000, true, volume, 1, nullptr};
	assert(!Producer::Publish(1, 2, Kind::Speaker, huge));
	assert(!Producer::Ready());
	Producer::Configure({s.path, nonce, 4, 7}, true);
	close(s.peer);
	s.peer = -1;
	s.Accept(4);
	ReadFrame(s.peer);
	// A native 512-frame source block must split at 10ms without changing sample bytes.
	std::vector<float> block(1024);
	for (size_t i = 0; i < block.size(); i++)
		block[i] = float(i) / 2048;
	PcmView split {block.data(), 512, 2, 48000, true, volume, 1, nullptr};
	assert(Producer::Publish(1, 2, Kind::Speaker, split));
	auto first = ReadFrame(s.peer), second = ReadFrame(s.peer);
	assert(first.header.first_frame == 0 && first.payload.size() == 480 * 8);
	assert(second.header.first_frame == 480 && second.payload.size() == 32 * 8 &&
	       second.header.sequence == first.header.sequence + 1);
	std::vector<uint8_t> joined = first.payload;
	joined.insert(joined.end(), second.payload.begin(), second.payload.end());
	for (size_t i = 0; i < block.size(); i++) {
		uint32_t bits = std::bit_cast<uint32_t>(block[i]);
		for (unsigned j = 0; j < 4; j++)
			assert(joined[i * 4 + j] == uint8_t(bits >> (j * 8)));
	}
	// Pressure from a reader that stops draining must withdraw rather than silently drop.
	std::vector<float> fullblock(1920);
	PcmView            full {fullblock.data(), 960, 2, 48000, true, volume, 1, nullptr};
	bool               withdrew = false;
	for (int i = 0; i < 10000; i++) {
		if (!Producer::Publish(1, 2, Kind::Speaker, full)) {
			withdrew = true;
			break;
		}
	}
	assert(withdrew && !Producer::Ready());
	Producer::Configure({s.path, nonce, 5, 7}, true);
	close(s.peer);
	s.peer = -1;
	s.Accept(5);
	ReadFrame(s.peer);
	Producer::Configure({s.path, nonce, 6, 7}, true);
	close(s.peer);
	s.peer = -1;
	s.Accept(6, false);
	Producer::Configure({s.path, nonce, 7, 7}, true);
	close(s.peer);
	s.peer = -1;
	s.Accept(7, true, true);
	ReadFrame(s.peer);
	Producer::ClosePort(1, 2);
	assert(ReadFrame(s.peer).header.type == Type::Close);
	Producer::OpenPort(1, 2, Kind::Speaker, 48000);
	assert(!Producer::Ready());
	Producer::OpenPort(1, 3, Kind::Speaker, 48000);
	Producer::Configure({s.path, nonce, 8, 7}, true);
	close(s.peer);
	s.peer = -1;
	s.Accept(8);
	ReadFrame(s.peer);
	const auto stalled_at = std::chrono::steady_clock::now();
	assert(Producer::Publish(1, 3, Kind::Speaker, view));
	// Even a tiny successful send must expire if it is stuck in the kernel queue.
	assert(WaitReady(false));
	assert(std::chrono::steady_clock::now() - stalled_at < std::chrono::milliseconds(80));
	assert(Producer::GetStatus().bytes > 0 && Producer::GetStatus().discontinuities >= 3);
	Producer::Disable();
	assert(!Producer::Ready());
	assert(!Producer::Publish(1, 2, Kind::Speaker, view));
	Producer::ClosePort(1, 3);
	std::puts("ControllerAudioProducerTests: all assertions passed (real Unix, no devices)");
	return 0;
}
