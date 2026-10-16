#ifdef NDEBUG
#undef NDEBUG
#endif
// Reuse the consuming SDL boundary and real Unix fixture; neither opens audio hardware.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wreturn-type"
#define main AudioTimingFixtureMain
#include "AudioOutTimingTests.cpp"
#undef main
#pragma GCC diagnostic pop
#define main AudioProducerFixtureMain
#include "ControllerAudioProducerTests.cpp"
#undef main
#include "streaming/transport.h"

#include <fcntl.h>
static void TestNegotiatedState() {
	Server     audio;
	const auto video_path = audio.dir + "/frame.sock";
	int        listener   = socket(AF_UNIX, SOCK_SEQPACKET, 0);
	assert(listener >= 0);
	sockaddr_un address {};
	address.sun_family = AF_UNIX;
	std::strcpy(address.sun_path, video_path.c_str());
	assert(bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
	assert(chmod(video_path.c_str(), 0600) == 0);
	assert(listen(listener, 1) == 0);
	auto wait = [](auto condition) {
		for (int i = 0; i < 500; i++) {
			if (condition()) return true;
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		return false;
	};
	Producer::OpenPort(7, 100, Kind::Actuator, 48000);
	{
		Streaming::Transport transport(
		    video_path, [](const Streaming::Input*) {}, [](uint64_t) { return true; });
		pollfd poller {listener, POLLIN, 0};
		assert(poll(&poller, 1, 1000) > 0);
		int peer = accept(listener, nullptr, nullptr);
		assert(peer >= 0);
		assert(Streaming::SendPacket(
		    peer,
		    {{"type", "welcome"}, {"version", 1}, {"capabilities", {"controller-audio-v1"}}}));
		assert(wait([&] { return transport.Generation() != 0; }));
		std::array<int, 9> fds;
		for (auto& fd: fds) {
			fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
			assert(fd >= 0);
		}
		Streaming::PoolDescriptor descriptor;
		descriptor.epoch  = 9;
		descriptor.width  = 1920;
		descriptor.height = 1080;
		descriptor.stride = 8192;
		descriptor.allocation_sizes.fill(8847360);
		descriptor.gpu_uuid = std::string(32, 'a');
		transport.SetPool(descriptor, fds, std::make_shared<Streaming::LeaseRing>(9),
		                  transport.Generation());
		Streaming::Packet hello;
		assert(wait([&] {
			return Streaming::ReceivePacket(peer, hello, 9) == Streaming::ReceiveResult::Packet;
		}));
		assert(Streaming::HasCapability(hello.json, "controller-audio-v1"));
		assert(Streaming::SendPacket(peer, {{"type", "accepted"},
		                                    {"version", 1},
		                                    {"epoch", 9u},
		                                    {"transport", "vulkan-buffer-fd"},
		                                    {"capabilities", {"controller-audio-v1"}}}));
		assert(wait([&] { return transport.Attached(); }));
		Streaming::Json state = {{"type", "controller-audio-state"},
		                         {"ready", true},
		                         {"generation", 31u},
		                         {"producer_epoch", 7u},
		                         {"socket", audio.path},
		                         {"nonce", std::string(64, 'a')}};
		// Preparing revokes the previous binding; Ready activates this same generation.
		assert(Streaming::SendPacket(
		    peer, {{"type", "controller-audio-state"}, {"ready", false}, {"generation", 31u}}));
		assert(WaitReady(false));
		assert(Streaming::SendPacket(peer, state));
		audio.Accept(31);
		assert(ReadFrame(audio.peer).header.port == 7);
		assert(Streaming::SendPacket(peer, state));
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
		assert(Producer::Ready());
		poller = {audio.fd, POLLIN, 0};
		assert(poll(&poller, 1, 0) == 0);
		assert(Streaming::SendPacket(
		    peer, {{"type", "controller-audio-state"}, {"ready", false}, {"generation", 32u}}));
		assert(WaitReady(false));
		assert(Streaming::SendPacket(peer, state));
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
		assert(!Producer::Ready());
		poller = {audio.fd, POLLIN, 0};
		assert(poll(&poller, 1, 0) == 0);
		state["generation"] = 33u;
		assert(Streaming::SendPacket(peer, state));
		close(audio.peer);
		audio.peer = -1;
		audio.Accept(33);
		assert(ReadFrame(audio.peer).header.port == 7);
		close(peer);
		assert(wait([&] { return !transport.Attached() && !Producer::Ready(); }));
		for (int fd: fds)
			close(fd);
	}
	Producer::ClosePort(7, 100);
	close(listener);
	unlink(video_path.c_str());
}
int main() {
	TestNegotiatedState();
	{
		Server     server;
		Fixture    fixture;
		const auto port = fixture.Open(512, 4);
		std::fill(fixture.pcm.begin(), fixture.pcm.end(), 0.5f);
		Producer::Configure({server.path, std::string(64, 'a'), 21, 7}, true);
		server.Accept(21);
		auto open = ReadFrame(server.peer);
		Check(open.header.kind == Kind::Speaker, "wrong speaker metadata");
		const float                     gains[] {0.5f, 0.25f};
		Libs::Audio::Audio::OutputParam param {port, fixture.pcm.data(), gains};
		fixture.audio.AudioOutOutputs(&param, 1, true);
		auto first = ReadFrame(server.peer), second = ReadFrame(server.peer);
		Check(first.payload[3] == 0x3e && first.payload[7] == 0x3e && first.payload[6] == 0,
		      "gain applied incorrectly");
		Check(second.header.first_frame == 480, "split continuity lost");
		Check(streams.size() == 1 && streams.front()->pcm.empty(),
		      "remote speaker leaked to main output");
		const auto before = now;
		fixture.audio.AudioOutOutputs(&param, 1, true);
		ReadFrame(server.peer);
		ReadFrame(server.peer);
		Check(now - before >= 10666 && now - before <= 10667,
		      "remote speaker sample pacing changed");
		Producer::Disable();
		fixture.audio.AudioOutOutputs(&param, 1, false);
		Check(!streams.front()->pcm.empty(), "lease loss did not restore speaker fallback");
	}
	{
		Server     server;
		Fixture    fixture;
		const auto port = fixture.Open(512, 10);
		Producer::Configure({server.path, std::string(64, 'a'), 22, 7}, true);
		server.Accept(22);
		auto open = ReadFrame(server.peer);
		Check(open.header.kind == Kind::Actuator, "wrong actuator metadata");
		Streaming::feedback_enabled = true;
		Streaming::feedback_value   = 123;
		fixture.Output(port);
		ReadFrame(server.peer);
		ReadFrame(server.peer);
		Check(Streaming::feedback_value == 123, "remote actuator emitted peak rumble");
		Streaming::feedback_enabled = false;
	}
	std::puts("ControllerAudioNativeRoutingTests: remote gains, pacing, fallback and peak "
	          "suppression passed");
}
