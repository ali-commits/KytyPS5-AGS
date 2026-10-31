#include <SDL3/SDL.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
namespace {
std::vector<const char *> names{"Desktop", "Kyty private game sink"};
std::string error;
int enumerations = 0;
void Check(bool value, const char *message) {
  if (!value) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::abort();
  }
}
SDL_AudioDeviceID *Playback(int *count) {
  *count = names.size();
  enumerations++;
  auto *ids = static_cast<SDL_AudioDeviceID *>(
      std::malloc(sizeof(SDL_AudioDeviceID) * names.size()));
  for (size_t i = 0; i < names.size(); i++)
    ids[i] = i + 1;
  return ids;
}
const char *Name(SDL_AudioDeviceID id) {
  return id > 0 && id <= names.size() ? names[id - 1] : nullptr;
}
bool Error(const char *, const char *requested) {
  error = requested;
  return false;
}
void Free(void *pointer) { std::free(pointer); }
} // namespace
#define SDL_GetAudioPlaybackDevices Playback
#define SDL_GetAudioDeviceName Name
#define SDL_SetError Error
#define SDL_free Free
#include "libs/audioOutputDevice.h"
#undef SDL_GetAudioPlaybackDevices
#undef SDL_GetAudioDeviceName
#undef SDL_SetError
#undef SDL_free
int main() {
  unsetenv("KYTY_AUDIO_OUTPUT_DEVICE");
  Check(Libs::Audio::OutputDevice() == SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK &&
            enumerations == 0,
        "absent selector retains default without enumeration");
  setenv("KYTY_AUDIO_OUTPUT_DEVICE", "Kyty private game sink", 1);
  Check(Libs::Audio::OutputDevice() == 2, "exact explicit sink selected");
  setenv("KYTY_AUDIO_OUTPUT_DEVICE", "private", 1);
  Check(Libs::Audio::OutputDevice() == 0 && !error.empty(),
        "missing explicit sink refuses desktop fallback");
  names.push_back("Kyty private game sink");
  setenv("KYTY_AUDIO_OUTPUT_DEVICE", "Kyty private game sink", 1);
  Check(Libs::Audio::OutputDevice() == 0,
        "ambiguous explicit sink refuses fallback");
  setenv("KYTY_AUDIO_OUTPUT_DEVICE", "", 1);
  Check(Libs::Audio::OutputDevice() == SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
        "empty selector retains default");
  unsetenv("KYTY_AUDIO_OUTPUT_DEVICE");
  std::puts("AudioOutputDeviceTests passed");
}
