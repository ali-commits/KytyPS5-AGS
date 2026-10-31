#include "common/atomicNotify.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

// Exercise publication racing with a newly parked waiter. A completed publication
// must never leave the receiver asleep. The extra reader and cache misses make the
// store-buffering window observable on affected libstdc++ runtimes (PR127757).
int main(int argc, char** argv) {
	using Clock = std::chrono::steady_clock;
	constexpr uint32_t rounds = 1000000;
	const bool control = argc == 2 && std::strcmp(argv[1], "--unfenced-control") == 0;
	alignas(64) std::atomic<uint32_t> message {0};
	alignas(64) std::atomic<uint32_t> requested {0};
	alignas(64) std::atomic<uint32_t> received {0};
	std::atomic<bool> stop {false}, missed {false};
	std::vector<uint8_t> memory(64 * 1024 * 1024);

	std::thread receiver([&] {
		for (uint32_t n = 1; n <= rounds && !missed.load(); n++) {
			requested.store(n, std::memory_order_release);
			message.wait(n - 1, std::memory_order_acquire);
			received.store(n, std::memory_order_release);
		}
	});
	std::thread reader([&] {
		while (!stop.load(std::memory_order_relaxed))
			(void)message.load(std::memory_order_relaxed);
	});
	std::thread monitor([&] {
		uint32_t last = 0;
		auto changed = Clock::now();
		while (!stop.load(std::memory_order_acquire)) {
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
			const auto count = received.load(std::memory_order_acquire);
			if (count != last) {
				last = count;
				changed = Clock::now();
			} else if (Clock::now() - changed > std::chrono::seconds(5) &&
			           message.load(std::memory_order_acquire) > count) {
				missed.store(true, std::memory_order_release);
				// Recover the stuck receiver without changing the published value.
				std::atomic_thread_fence(std::memory_order_seq_cst);
				message.notify_all();
				return;
			}
		}
	});

	size_t cursor = 0;
	uint32_t random = 17;
	volatile uint32_t jitter = 0;
	for (uint32_t n = 1; n <= rounds && !missed.load(); n++) {
		while (requested.load(std::memory_order_acquire) < n && !missed.load()) {}
		if (missed.load()) break;
		random ^= random << 13;
		random ^= random >> 17;
		random ^= random << 5;
		for (uint32_t k = 0; k < random % 4000; k++) jitter = jitter + 1;
		for (uint32_t k = 0; k < 64; k++) {
			memory[cursor] = static_cast<uint8_t>(n);
			cursor = (cursor + 4160) % memory.size();
		}
		message.store(n, std::memory_order_release);
		if (control)
			message.notify_all();
		else
			Common::NotifyAllAfterStore(message);
		while (received.load(std::memory_order_acquire) < n && !missed.load()) {}
	}
	stop.store(true, std::memory_order_release);
	monitor.join();
	reader.join();
	// A receiver racing with failure detection might have started another wait.
	// Change the value as well as waking it so failure cleanup cannot park forever.
	message.store(rounds + 1, std::memory_order_release);
	Common::NotifyAllAfterStore(message);
	receiver.join();
	if (missed.load()) {
		std::fprintf(stderr, "AtomicNotifyTests: publication completed but its wake was lost\n");
		return 1;
	}
	std::printf("AtomicNotifyTests: %u publications completed\n", rounds);
	return 0;
}
