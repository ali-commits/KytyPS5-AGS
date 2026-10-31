// Hardware-free primary SDL behavior check. Every device open is guarded by an
// immediate assertion of the forced dummy driver; no USB output is constructed.
#include "usbSink.h"

#include <SDL3/SDL.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
using namespace ControllerAudio::Receiver;
namespace {
void Check(bool yes, const char* message) {
	if (!yes) {
		std::fprintf(stderr, "SDL receiver test: %s (%s)\n", message, SDL_GetError());
		std::abort();
	}
}
struct SilentMix {
	std::atomic_uint callbacks {0};
	std::atomic_bool nonzero {false};
};
void SDLCALL ObserveSilentMix(void* userdata, const SDL_AudioSpec*, float* samples, int bytes) {
	auto& mix = *static_cast<SilentMix*>(userdata);
	for (int i = 0; i < bytes / static_cast<int>(sizeof(float)); i++)
		if (samples[i] != 0) mix.nonzero.store(true);
	mix.callbacks.fetch_add(1);
}
SDL_AudioDeviceID OpenDummy(const SDL_AudioSpec& format) {
	Check(SDL_GetCurrentAudioDriver() && std::strcmp(SDL_GetCurrentAudioDriver(), "dummy") == 0,
	      "refusing device open unless driver is dummy");
	return SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &format);
}
} // namespace
int main() {
	Check(SDL_SetHintWithPriority(SDL_HINT_AUDIO_DRIVER, "dummy", SDL_HINT_OVERRIDE) &&
	          SDL_InitSubSystem(SDL_INIT_AUDIO),
	      "dummy audio initialization");
	Check(SDL_GetCurrentAudioDriver() && std::strcmp(SDL_GetCurrentAudioDriver(), "dummy") == 0,
	      "driver must be dummy");
	const SDL_AudioSpec     format {SDL_AUDIO_F32, 4, 48000};
	std::array<float, 960>  data {};
	std::array<float, 4096> output {};
	auto*                   memory = SDL_CreateAudioStream(&format, &format);
	Check(memory, "memory stream creation");
	int max_residual = 0;
	for (int block = 0; block < 1000; block++) {
		auto queued = SDL_GetAudioStreamQueued(memory);
		Check(queued >= 0, "memory queue query");
		auto pending_us = uint64_t(queued) * 1000000 / (48000 * 16);
		Check(SDL_SetAudioStreamFrequencyRatio(memory, UsbSink::FrequencyRatio(pending_us + 5000)),
		      "continuous real SDL ratio changes");
		Check(SDL_PutAudioStreamData(memory, data.data(), sizeof(data)), "memory input");
		auto available = SDL_GetAudioStreamAvailable(memory);
		Check(available >= 0, "available output");
		while (available > 0) {
			int got = SDL_GetAudioStreamData(memory, output.data(),
			                                 std::min<int>(available, sizeof(output)));
			Check(got > 0, "memory consumer progress");
			available -= got;
		}
		auto residual = SDL_GetAudioStreamQueued(memory);
		max_residual  = std::max(max_residual, residual);
		Check(residual >= 0 && residual < 48000 * 16 / 1000,
		      "ratio/resampler padding remains below one millisecond");
	}
	SDL_DestroyAudioStream(memory);
	// The actual dummy callback must consume data eventually; startup timing is
	// measured, not asserted against a real-device cold-start budget.
	SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "240");
	auto device = OpenDummy(format);
	Check(device, "dummy device open");
	SilentMix silent;
	Check(SDL_SetAudioPostmixCallback(device, ObserveSilentMix, &silent),
	      "observe silent no-stream callback");
	auto silent_start = SDL_GetTicks();
	while (silent.callbacks.load() < 2 && SDL_GetTicks() - silent_start < 500)
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	Check(silent.callbacks.load() >= 2 && !silent.nonzero.load(),
	      "unbound SDL output progresses with silence and no source submissions");
	Check(SDL_SetAudioPostmixCallback(device, nullptr, nullptr), "remove silent observer");
	auto* stream = SDL_CreateAudioStream(&format, &format);
	Check(stream && SDL_BindAudioStream(device, stream), "dummy stream binding");
	Check(SDL_PutAudioStreamData(stream, data.data(), sizeof(data)), "dummy input");
	auto start = SDL_GetTicks();
	while (SDL_GetAudioStreamQueued(stream) == sizeof(data) && SDL_GetTicks() - start < 1000)
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	Check(SDL_GetAudioStreamQueued(stream) < static_cast<int>(sizeof(data)),
	      "actual dummy callback consumed source data");
	auto first_drain_ms = SDL_GetTicks() - start;
	SDL_ClearAudioStream(stream);
	SDL_DestroyAudioStream(stream);
	SDL_CloseAudioDevice(device);
	SDL_QuitSubSystem(SDL_INIT_AUDIO);
	std::printf("ControllerAudioReceiverSdlTests passed: dummy only; max memory residual=%d bytes; "
	            "first dummy drain=%llu ms\n",
	            max_residual, static_cast<unsigned long long>(first_drain_ms));
}
