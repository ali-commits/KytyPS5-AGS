#include "kyty_bt.h"
#include "packets.h"
#include "libs/dualSenseBluetooth.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_hidapi.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace Core = Libs::Controller::DualSenseBluetooth;
namespace {
struct Handle {
    Core::Stream* core;
    int controller;
    bool speaker;
    uint64_t queued_at = 0, queued_us = 0;
};
struct Lease { std::atomic<uint64_t> deadline {0}; };
struct Controller {
    std::shared_ptr<Lease> lease = std::make_shared<Lease>();
    uint64_t deadline = 0, rumble_until = 0, haptics_until = 0;
    uint16_t large = 0, small = 0;
    bool suppressed = false, speaker = false;
};
std::mutex mutex;
std::mutex shutdown_mutex;
std::condition_variable wake;
std::thread watchdog;
bool stopping = false;
std::map<int, Controller> controllers;
std::map<uintptr_t, std::unique_ptr<Handle>> handles;
uintptr_t next_handle = 1;
Handle* Find(void* token) {
    const auto it = handles.find(reinterpret_cast<uintptr_t>(token));
    return it == handles.end() ? nullptr : it->second.get();
}
struct Cleanup { ~Cleanup(); };
std::atomic<uint64_t> accepted {0}, failed {0};
thread_local std::string error;
thread_local std::shared_ptr<Lease> opening_lease;
std::mutex registry_mutex;
std::map<SDL_hid_device*, std::shared_ptr<Lease>> device_leases;
uint64_t Now() { return SDL_GetTicksNS() / 1000; }
int Fail(const char* message) { error = message; return 0; }
std::string Address(const char* serial) {
    std::string out;
    if (!serial) return out;
    for (; *serial; ++serial) {
        if (*serial >= '0' && *serial <= '9') out += *serial;
        else if (*serial >= 'a' && *serial <= 'f') out += *serial;
        else if (*serial >= 'A' && *serial <= 'F') out += char(*serial - 'A' + 'a');
        else if (*serial != ':' && *serial != '-') return {};
    }
    return out.size() == 12 ? out : std::string {};
}
bool Route(int id, bool enabled) {
    const auto packet = Companion::SpeakerRoute(enabled);
    SDL_LockJoysticks();
    auto* pad = SDL_GetGamepadFromID(static_cast<SDL_JoystickID>(id));
    const bool ok = pad && SDL_SendGamepadEffect(pad, packet.data(), int(packet.size()));
    SDL_UnlockJoysticks();
    return ok;
}
bool Rumble(int id, Controller& controller, uint64_t now, bool force = false) {
    const bool muted = controller.haptics_until > now || controller.rumble_until <= now;
    SDL_LockJoysticks();
    auto* pad = SDL_GetGamepadFromID(static_cast<SDL_JoystickID>(id));
    if (pad && force && !muted) SDL_RumbleGamepad(pad, 0, 0, 0);
    const bool ok = pad && SDL_RumbleGamepad(pad, muted ? 0 : controller.large,
        muted ? 0 : controller.small, muted ? 0 : uint32_t((controller.rumble_until - now + 999) / 1000));
    SDL_UnlockJoysticks();
    controller.suppressed = controller.haptics_until > now;
    return ok;
}
bool Effects(int id, const uint8_t* left, const uint8_t* right, uint8_t flags) {
    const auto packet = Companion::Triggers(left, right, flags);
    SDL_LockJoysticks();
    auto* pad = SDL_GetGamepadFromID(static_cast<SDL_JoystickID>(id));
    const bool ok = pad && SDL_SendGamepadEffect(pad, packet.data(), int(packet.size()));
    SDL_UnlockJoysticks();
    return ok;
}
void Expire(int id, Controller& state) {
    state.lease->deadline.store(0);
    // Original Close invalidates prepared reports and drains any in-flight HID write.
    for (auto& [token, owned] : handles) {
        auto* handle = owned.get();
        if (handle->controller == id && handle->core) {
            Core::Close(handle->core);
            handle->core = nullptr;
            handle->queued_us = 0;
        }
    }
    const uint8_t off[11] = {5};
    Effects(id, off, off, 12);
    state.large = state.small = 0;
    state.rumble_until = state.haptics_until = state.deadline = 0;
    Rumble(id, state, Now());
    if (state.speaker) Route(id, false);
    state.speaker = false;
}
void Start() {
    static Cleanup cleanup;
    if (watchdog.joinable()) return;
    stopping = false;
    watchdog = std::thread([] {
        std::unique_lock lock(mutex);
        while (!stopping) {
            const auto now = Now();
            for (auto& [id, state] : controllers) {
                if (state.deadline && now >= state.deadline) Expire(id, state);
                else if (state.suppressed && state.haptics_until <= now) Rumble(id, state, now);
            }
            wake.wait_for(lock, std::chrono::milliseconds(10));
        }
    });
}
Controller& State(int controller) {
    auto [it, inserted] = controllers.try_emplace(controller);
    if (inserted) {
        it->second.deadline = Now() + 1000000;
        it->second.lease->deadline.store(it->second.deadline);
    }
    Start();
    return it->second;
}
}
extern "C" {
const char* kyty_bt_version() { return "kyty-emulator-bluetooth-0x39-abi1"; }
const char* kyty_bt_error() { return error.c_str(); }
uint64_t kyty_bt_now_us() { return Now(); }
uint64_t kyty_bt_accepted_reports() { return accepted.load(); }
uint64_t kyty_bt_failed_reports() { return failed.load(); }
uint64_t kyty_bt_accepted_bytes() { return accepted.load() * 547; }
const char* kyty_bt_source_sha256() { return KYTY_BT_SOURCE_SHA256; }
SDL_hid_device* kyty_bt_hid_open_path(const char* path) {
    auto* device = SDL_hid_open_path(path);
    if (device) {
        std::lock_guard lock(registry_mutex);
        device_leases[device] = opening_lease;
    }
    return device;
}
void kyty_bt_hid_close(SDL_hid_device* device) {
    { std::lock_guard lock(registry_mutex); device_leases.erase(device); }
    SDL_hid_close(device);
}
int kyty_bt_hid_write(SDL_hid_device* device, const unsigned char* data, size_t length) {
    std::shared_ptr<Lease> lease;
    {
        std::lock_guard lock(registry_mutex);
        const auto found = device_leases.find(device);
        if (found != device_leases.end()) lease = found->second;
    }
    if (!lease || !lease->deadline.load() || Now() >= lease->deadline.load()) {
        failed++;
        SDL_SetError("Bluetooth output lease expired before HID write");
        return -1;
    }
    const int result = SDL_hid_write(device, data, length);
    if (length == 547 && data && data[0] == 0x39) {
        if (result == int(length)) accepted++;
        else failed++;
    }
    return result;
}
int kyty_bt_find_controller(const char* serial) {
    error.clear();
    const auto wanted = Address(serial);
    if (wanted.empty()) { Fail("A complete 12-digit Bluetooth serial is required"); return -1; }
    int count = 0, found = -1;
    SDL_LockJoysticks();
    auto* ids = SDL_GetGamepads(&count);
    for (int i = 0; ids && i < count; ++i) {
        auto* pad = SDL_GetGamepadFromID(ids[i]);
        if (pad && SDL_GetGamepadType(pad) == SDL_GAMEPAD_TYPE_PS5 &&
            SDL_GetGamepadConnectionState(pad) == SDL_JOYSTICK_CONNECTION_WIRELESS &&
            SDL_GetGamepadVendor(pad) == 0x054c &&
            (SDL_GetGamepadProduct(pad) == 0x0ce6 || SDL_GetGamepadProduct(pad) == 0x0df2) &&
            Address(SDL_GetGamepadSerial(pad)) == wanted) {
            if (found != -1) { found = -2; break; }
            found = int(ids[i]);
        }
    }
    SDL_free(ids);
    SDL_UnlockJoysticks();
    if (found < 0) { Fail(found == -2 ? "Ambiguous opened Bluetooth controller" : "Matching Bluetooth controller must already be opened in the shared SDL3 runtime"); return -1; }
    return found;
}
void* kyty_bt_open(uint32_t rate, int speaker, int controller) {
    std::lock_guard lock(mutex);
    error.clear();
    if (stopping) { Fail("Native Bluetooth shutdown in progress"); return 0; }
    auto& state = State(controller);
    if (state.deadline <= Now()) { Fail("Controller output lease expired; renew before opening"); return nullptr; }
    opening_lease = state.lease;
    auto* core = Core::Open(rate, speaker != 0, controller, nullptr);
    opening_lease.reset();
    if (!core) { Fail(SDL_GetError()[0] ? SDL_GetError() : "Original emulator Bluetooth Open failed"); return nullptr; }
    if (speaker && !state.speaker && !Route(controller, true)) {
        Core::Close(core); Fail(SDL_GetError()); return nullptr;
    }
    if (speaker) { state.speaker = true; Rumble(controller, state, Now(), true); }
    if (!next_handle || next_handle == UINTPTR_MAX) {
        Core::Close(core); Fail("Native stream token space exhausted"); return nullptr;
    }
    const auto token = next_handle++;
    handles.emplace(token, std::make_unique<Handle>(Handle {core, controller, speaker != 0}));
    return reinterpret_cast<void*>(token);
}
uint64_t kyty_bt_queue(void* opaque, const float* stereo, uint32_t frames) {
    std::lock_guard lock(mutex);
    auto* handle = Find(opaque);
    if (stopping || !handle || !handle->core || !stereo || !frames) return Fail("Inactive audio stream");
    auto& state = controllers.at(handle->controller);
    const auto now = Now();
    if (!state.deadline || now >= state.deadline) { Expire(handle->controller, state); return Fail("Controller output lease expired"); }
    const auto queued = Core::Queue(handle->core, stereo, frames);
    if (!queued) return Fail("Original emulator Bluetooth queue failed");
    handle->queued_us = queued;
    handle->queued_at = now;
    if (!handle->speaker && std::any_of(stereo, stereo + size_t(frames) * 2,
            [](float sample) { return std::isfinite(sample) && sample != 0.0f; })) {
        state.haptics_until = std::max(state.haptics_until, now + std::max<uint64_t>(queued, 250000));
        Rumble(handle->controller, state, now);
    }
    return queued;
}
uint64_t kyty_bt_queued_us(void* opaque) {
    std::lock_guard lock(mutex);
    auto* handle = Find(opaque);
    if (!handle || !handle->core) return 0;
    const auto elapsed = Now() - handle->queued_at;
    return elapsed < handle->queued_us ? handle->queued_us - elapsed : 0;
}
void kyty_bt_close(void* opaque) {
    std::lock_guard lock(mutex);
    const auto found = handles.find(reinterpret_cast<uintptr_t>(opaque));
    if (found == handles.end()) return;
    auto owned = std::move(found->second);
    handles.erase(found);
    auto* handle = owned.get();
    if (handle->core) Core::Close(handle->core);
    if (handle->speaker && controllers.contains(handle->controller)) {
        const bool another = std::any_of(handles.begin(), handles.end(), [&](const auto& item) {
            const auto* other = item.second.get();
            return other->controller == handle->controller && other->speaker && other->core;
        });
        auto& state = controllers.at(handle->controller);
        if (!another && state.speaker) {
            Route(handle->controller, false);
            state.speaker = false;
            Rumble(handle->controller, state, Now(), true);
        }
    }
}
int kyty_bt_set_speaker(int controller, int enabled) {
    std::lock_guard lock(mutex);
    if (stopping) { Fail("Native Bluetooth shutdown in progress"); return 0; }
    auto& state = State(controller);
    if (state.deadline <= Now()) return Fail("Controller output lease expired");
    if (!Route(controller, enabled != 0)) return Fail(SDL_GetError());
    state.speaker = enabled != 0;
    Rumble(controller, state, Now(), true);
    return 1;
}
int kyty_bt_renew(int controller, uint64_t deadline) {
    std::lock_guard lock(mutex);
    if (stopping) { Fail("Native Bluetooth shutdown in progress"); return 0; }
    auto& state = State(controller);
    if (deadline <= Now()) { Expire(controller, state); return Fail("Controller output lease expired"); }
    state.deadline = deadline;
    state.lease->deadline.store(deadline);
    wake.notify_all();
    return 1;
}
int kyty_bt_controls(int controller, const uint8_t* left, const uint8_t* right, uint8_t flags,
                     uint16_t large, uint16_t small, uint32_t duration, uint64_t deadline) {
    std::lock_guard lock(mutex);
    if (!left || !right || (flags & ~12)) return Fail("Invalid trigger payload");
    if (stopping) { Fail("Native Bluetooth shutdown in progress"); return 0; }
    auto& state = State(controller);
    const auto now = Now();
    if (deadline <= now) {
        Expire(controller, state);
        return deadline == 0 ? 1 : Fail("Controller output lease expired");
    }
    state.deadline = deadline;
    state.lease->deadline.store(deadline);
    state.large = large; state.small = small;
    state.rumble_until = now + uint64_t(duration) * 1000;
    if (!Effects(controller, left, right, flags) || !Rumble(controller, state, now)) return Fail(SDL_GetError());
    return 1;
}
void kyty_bt_shutdown() {
    std::lock_guard shutdown_lock(shutdown_mutex);
    {
        std::lock_guard lock(mutex);
        stopping = true;
        wake.notify_all();
    }
    if (watchdog.joinable()) watchdog.join();
    {
        std::lock_guard lock(mutex);
        for (auto& [id, state] : controllers) Expire(id, state);
        handles.clear();
        controllers.clear();
        Core::Shutdown();
        stopping = false;
    }
}
}

namespace {
Cleanup::~Cleanup() { kyty_bt_shutdown(); }
}
