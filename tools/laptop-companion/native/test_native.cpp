#include "kyty_bt.h"
#include "packets.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_hidapi.h>
#include <opus.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>
namespace {
std::mutex output_mutex;
std::vector<std::vector<uint8_t>> writes, effects;
uint16_t last_large = 0, last_small = 0;
int speaker_rate = 0, haptic_rate = 0;
bool fail_write = false;
void Check(bool ok, const char* message) { if (!ok) { std::fprintf(stderr, "FAIL: %s (%s)\n", message, kyty_bt_error()); std::exit(1); } }
bool Wait(auto predicate, int milliseconds = 1000) {
    for (int i = 0; i < milliseconds; i += 5) {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return predicate();
}
uint32_t Crc(const uint8_t* bytes, size_t length) {
    uint32_t crc = ~0U;
    const auto next = [&](uint8_t byte) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
    };
    next(0xa2);
    for (size_t i = 0; i < length; ++i) next(bytes[i]);
    return ~crc;
}
}
extern "C" {
void __wrap_SDL_LockJoysticks() {}
void __wrap_SDL_UnlockJoysticks() {}
SDL_JoystickID* __wrap_SDL_GetGamepads(int* count) {
    *count = 1;
    auto* ids = static_cast<SDL_JoystickID*>(SDL_malloc(2 * sizeof(SDL_JoystickID)));
    ids[0] = 7; ids[1] = 0; return ids;
}
SDL_Gamepad* __wrap_SDL_GetGamepadFromID(SDL_JoystickID id) { return id == 7 ? reinterpret_cast<SDL_Gamepad*>(7) : nullptr; }
SDL_GamepadType __wrap_SDL_GetGamepadType(SDL_Gamepad*) { return SDL_GAMEPAD_TYPE_PS5; }
SDL_JoystickConnectionState __wrap_SDL_GetGamepadConnectionState(SDL_Gamepad*) { return SDL_JOYSTICK_CONNECTION_WIRELESS; }
Uint16 __wrap_SDL_GetGamepadVendor(SDL_Gamepad*) { return 0x054c; }
Uint16 __wrap_SDL_GetGamepadProduct(SDL_Gamepad*) { return 0x0ce6; }
const char* __wrap_SDL_GetGamepadSerial(SDL_Gamepad*) { return "aA:bB:cC:11:22:33"; }
bool __wrap_SDL_SendGamepadEffect(SDL_Gamepad*, const void* data, int size) {
    std::lock_guard lock(output_mutex);
    auto* bytes = static_cast<const uint8_t*>(data);
    effects.emplace_back(bytes, bytes + size); return true;
}
bool __wrap_SDL_RumbleGamepad(SDL_Gamepad*, Uint16 large, Uint16 small, Uint32) {
    std::lock_guard lock(output_mutex); last_large = large; last_small = small; return true;
}
SDL_hid_device_info* __wrap_SDL_hid_enumerate(unsigned short, unsigned short) {
    static SDL_hid_device_info info {};
    static char path[] = "fake Bluetooth only";
    static wchar_t serial[] = L"AA-BB-CC-11-22-33";
    info.path = path; info.serial_number = serial;
    info.bus_type = SDL_HID_API_BUS_BLUETOOTH; info.usage_page = 1; info.usage = 5;
    return &info;
}
void __wrap_SDL_hid_free_enumeration(SDL_hid_device_info*) {}
SDL_hid_device* __wrap_SDL_hid_open_path(const char*) { return reinterpret_cast<SDL_hid_device*>(42); }
void __wrap_SDL_hid_close(SDL_hid_device*) {}
int __wrap_SDL_hid_write(SDL_hid_device*, const unsigned char* data, size_t size) {
    std::lock_guard lock(output_mutex);
    if (fail_write) return -1;
    writes.emplace_back(data, data + size);
    return int(size);
}
SDL_AudioStream* __real_SDL_CreateAudioStream(const SDL_AudioSpec*, const SDL_AudioSpec*);
SDL_AudioStream* __wrap_SDL_CreateAudioStream(const SDL_AudioSpec* source, const SDL_AudioSpec* target) {
    if (target->freq == 45000) speaker_rate = target->freq;
    if (target->freq == 3000) haptic_rate = target->freq;
    return __real_SDL_CreateAudioStream(source, target);
}
}
extern "C" int kyty_bt_hid_write(SDL_hid_device*, const unsigned char*, size_t);
int main() {
    Check(SDL_Init(0), "SDL init without any physical devices");
    Check(std::strstr(kyty_bt_version(), "0x39") != nullptr, "version provenance");
    const auto route = Companion::SpeakerRoute(true);
    Check(route[0] == 0xa0 && route[1] == 0x80 && route[5] == 0x64 && route[7] == 0x30 && route[37] == 2, "exact emulator speaker route");
    const auto neutral = Companion::SpeakerRoute(false);
    Check(neutral[0] == 0x80 && std::count(neutral.begin(), neutral.end(), 0) == 37, "speaker route restoration");
    Check(kyty_bt_find_controller("AA-BB-CC-11-22-33") == 7, "normalized exact serial selection");
    Check(kyty_bt_find_controller("AA-BB-CC-11-22-34") == -1, "wrong serial rejected");
    Check(kyty_bt_find_controller("*") == -1, "invalid serial rejected");
    const uint8_t neutral_trigger[11] = {5};
    Check(kyty_bt_controls(7, neutral_trigger, neutral_trigger, 12, 0, 0, 0, 0) == 1, "explicit initial stop succeeds");
    Check(kyty_bt_controls(7, neutral_trigger, neutral_trigger, 12, 0, 0, 0, 0) == 1, "repeated explicit stop succeeds");
    Check(kyty_bt_renew(7, kyty_bt_now_us() + 1500000), "lease establish");
    auto* speaker = kyty_bt_open(48000, 1, 7);
    auto* haptics = kyty_bt_open(48000, 0, 7);
    Check(speaker && haptics, "original source opens fake Bluetooth HID");
    Check(speaker_rate == 45000 && haptic_rate == 3000, "original speaker compensation and haptic resampling rates");
    const uint8_t left[11] = {1,2,3,4,5,6,7,8,9,10,11};
    const uint8_t right[11] = {11,10,9,8,7,6,5,4,3,2,1};
    Check(kyty_bt_controls(7, left, right, 12, 1234, 5678, 1000, kyty_bt_now_us() + 1500000), "SDK controls accepted");
    {
        std::lock_guard lock(output_mutex);
        const auto& effect = effects.back();
        Check(effect.size() == 47 && effect[0] == 12 && std::equal(right, right + 11, effect.begin() + 10) && std::equal(left, left + 11, effect.begin() + 21), "SDK trigger layout");
        Check(last_large == 1234 && last_small == 5678, "upstream rumble preserved");
    }
    std::vector<float> pcm(3840 * 2);
    for (size_t i = 0; i < pcm.size(); ++i) pcm[i] = 0.25f * std::sin(float(i) * 0.03f);
    Check(kyty_bt_queue(haptics, pcm.data(), 3840) > 0, "haptic PCM queued");
    Check(kyty_bt_queue(speaker, pcm.data(), 3840) > 0, "speaker PCM queued");
    Check(kyty_bt_queued_us(speaker) > 0, "queued estimate exposed");
    {
        std::lock_guard lock(output_mutex);
        Check(last_large == 0 && last_small == 0, "PCM suppresses legacy rumble");
    }
    Check(Wait([] { return kyty_bt_accepted_reports() >= 2; }), "original sender accepts reports");
    {
        std::lock_guard lock(output_mutex);
        Check(!writes.empty(), "captured HID reports");
        for (const auto& report : writes) {
            Check(report.size() == 547 && report[0] == 0x39, "native report ID and size");
            Check(report[2] == 0x91 && report[3] == 6 && report[4] == 0x7e && report[10] == 0xd2 && report[11] == 64, "native haptic subpacket layout");
            Check(report[140] == 0xd3 && report[141] == 200, "two CBR speaker packets");
            uint32_t expected = Crc(report.data(), 543), actual = 0;
            for (int i = 0; i < 4; ++i) actual |= uint32_t(report[543+i]) << (i*8);
            Check(actual == expected, "Bluetooth output CRC");
            int err = 0;
            auto* decoder = opus_decoder_create(48000, 2, &err);
            std::array<float, 960> decoded {};
            Check(decoder && opus_decode_float(decoder, report.data() + 142, 200, decoded.data(), 480, 0) == 480, "first original CBR Opus packet decodes");
            Check(opus_decode_float(decoder, report.data() + 342, 200, decoded.data(), 480, 0) == 480, "second original CBR Opus packet decodes");
            opus_decoder_destroy(decoder);
        }
    }
    Check(Wait([] { std::lock_guard lock(output_mutex); return last_large == 1234; }), "rumble resumes after PCM tail");
    Check(kyty_bt_renew(7, kyty_bt_now_us() + 30000), "short watchdog lease");
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    Check(kyty_bt_queue(speaker, pcm.data(), 3840) == 0, "native watchdog makes queued handles inactive");
    {
        std::lock_guard lock(output_mutex);
        Check(last_large == 0 && last_small == 0, "watchdog independently zeros motors");
        const auto& off = effects.back();
        Check(off.size() == 38 && off[0] == 0x80, "watchdog restores speaker routing after close barrier");
        Check(std::any_of(effects.begin(), effects.end(), [](const auto& effect) { return effect.size() == 47 && effect[0] == 12 && effect[10] == 5 && effect[21] == 5; }), "watchdog neutral triggers");
    }
    const auto stopped_count = kyty_bt_accepted_reports();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    Check(kyty_bt_accepted_reports() == stopped_count, "no writes after watchdog close barrier");
    std::array<uint8_t, 547> forbidden {}; forbidden[0] = 0x39;
    Check(kyty_bt_hid_write(reinterpret_cast<SDL_hid_device*>(42), forbidden.data(), forbidden.size()) == -1, "expired or unregistered HID cannot initiate a native write");
    Check(kyty_bt_accepted_reports() == stopped_count, "expired raw attempt not accepted");
    Check(kyty_bt_controls(7, neutral_trigger, neutral_trigger, 12, 0, 0, 0, 0) == 1, "explicit stop after expiry succeeds");
    kyty_bt_close(speaker); kyty_bt_close(speaker); kyty_bt_close(haptics);
    Check(kyty_bt_renew(7, kyty_bt_now_us() + 1000000), "renew after explicit stop");
    auto* reacquired = kyty_bt_open(48000, 1, 7);
    Check(reacquired, "reacquire after explicit stop");
    Check(reacquired != speaker && reacquired != haptics, "opaque tokens never reuse prior handle identities");
    kyty_bt_close(speaker);
    Check(kyty_bt_queue(speaker, pcm.data(), 3840) == 0, "stale closed token cannot queue onto a new stream");
    Check(kyty_bt_queue(reacquired, pcm.data(), 3840) > 0, "stale close cannot affect reacquired stream");
    Check(kyty_bt_controls(7, neutral_trigger, neutral_trigger, 12, 0, 0, 0, 0) == 1, "explicit active stop succeeds");
    Check(kyty_bt_queue(reacquired, pcm.data(), 3840) == 0, "explicit stop invalidates core but preserves handle");
    kyty_bt_close(reacquired);
    kyty_bt_shutdown();
    Check(kyty_bt_renew(7, kyty_bt_now_us() + 1000000), "restart after shutdown");
    auto* failure = kyty_bt_open(48000, 1, 7);
    Check(failure, "restart opens stream");
    { std::lock_guard lock(output_mutex); fail_write = true; }
    Check(kyty_bt_queue(failure, pcm.data(), 3840) > 0, "queue before write failure");
    const auto failures_before = kyty_bt_failed_reports();
    Check(Wait([&] { return kyty_bt_failed_reports() > failures_before; }), "failed native writes counted");
    Check(kyty_bt_queue(failure, pcm.data(), 3840) == 0, "failed native session refuses further PCM");
    kyty_bt_close(failure); kyty_bt_shutdown(); SDL_Quit();
    std::puts("PASS: original 0x39 sender, 45k speaker resampling, Opus CBR, CRC, SDK routes, rumble suppression, watchdog, close barrier, safe handles, write failure");
}
