#pragma once
#include <SDL3/SDL_audio.h>
#include <SDL3/SDL_error.h>
#include <SDL3/SDL_stdinc.h>
#include <cstdlib>
#include <cstring>
namespace Libs::Audio {
// An explicit private sink must never silently fall back to the desktop mix.
inline SDL_AudioDeviceID OutputDevice() {
	const char* requested = std::getenv("KYTY_AUDIO_OUTPUT_DEVICE");
	if (requested == nullptr || *requested == '\0') return SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;
	int                count    = 0;
	SDL_AudioDeviceID* devices  = SDL_GetAudioPlaybackDevices(&count);
	SDL_AudioDeviceID  selected = 0;
	for (int i = 0; devices != nullptr && i < count; i++) {
		const char* name = SDL_GetAudioDeviceName(devices[i]);
		if (name != nullptr && std::strcmp(name, requested) == 0) {
			if (selected != 0) {
				selected = 0;
				break;
			}
			selected = devices[i];
		}
	}
	SDL_free(devices);
	if (selected == 0)
		SDL_SetError("Explicit Kyty audio output device is missing or ambiguous: %s", requested);
	return selected;
}
} // namespace Libs::Audio
