// Silent cross-process fixture: production native publisher, no SDL or game.
#include "streaming/controllerAudio.h"

#include <array>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <thread>
namespace Producer = Streaming::ControllerAudio;
using ControllerAudio::Kind;
int main(int argc, char** argv) {
	if (argc != 5) return 2;
	uint64_t generation = 0, epoch = 0;
	auto     number = [](const char* value, uint64_t& result) {
		const std::string_view input(value);
		const auto parsed = std::from_chars(input.data(), input.data() + input.size(), result);
		return parsed.ec == std::errc {} && parsed.ptr == input.data() + input.size() && result;
	};
	if (!number(argv[3], generation) || !number(argv[4], epoch)) return 2;
	struct Cleanup {
		~Cleanup() { Producer::Disable(); }
	} cleanup;
	Producer::Configure({argv[1], argv[2], generation, epoch}, true);
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (!Producer::Ready() && std::chrono::steady_clock::now() < deadline)
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	if (!Producer::Ready()) {
		std::fputs("producer was not ready\n", stderr);
		return 3;
	}
	Producer::OpenPort(0, 1, Kind::Actuator, 48000);
	Producer::OpenPort(1, 1, Kind::Speaker, 48000);
	std::array<float, 960> actuator, speaker;
	for (size_t i = 0; i < 480; i++) {
		actuator[i * 2]     = 0.125f;
		actuator[i * 2 + 1] = -0.25f;
		speaker[i * 2]      = 0.375f;
		speaker[i * 2 + 1]  = -0.5f;
	}
	auto tick = std::chrono::steady_clock::now();
	for (int i = 0; i < 20; i++) {
		if (!Producer::Publish(0, 1, Kind::Actuator,
		                       {actuator.data(), 480, 2, 48000, true, nullptr, 1.0f, nullptr}) ||
		    !Producer::Publish(1, 1, Kind::Speaker,
		                       {speaker.data(), 480, 2, 48000, true, nullptr, 1.0f, nullptr})) {
			std::fprintf(stderr, "producer publish failed at chunk %d\n", i);
			return 4;
		}
		tick += std::chrono::milliseconds(10);
		std::this_thread::sleep_until(tick);
	}
	Producer::ClosePort(0, 1);
	Producer::ClosePort(1, 1);
	std::this_thread::sleep_for(std::chrono::milliseconds(100));
	const auto status = Producer::GetStatus();
	std::printf("native payload bytes=%llu discontinuities=%llu\n",
	            static_cast<unsigned long long>(status.bytes),
	            static_cast<unsigned long long>(status.discontinuities));
	return status.discontinuities == 0 && status.bytes >= 153600 ? 0 : 5;
}
