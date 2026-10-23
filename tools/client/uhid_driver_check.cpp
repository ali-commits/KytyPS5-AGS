// D2: the emulator's own Bluetooth DualSense driver must accept a uhid pad built the way the
// Sunshine fork's bridge builds it (bus Bluetooth, real descriptor, MAC as uniq, phys
// kyty-host-tunnel). Exit 0 = PASS, 1 = FAIL, 77 = skipped (no fixture or no /dev/uhid).
// The driver file is compiled into this TU unchanged so its private OpenDevice() is callable.
#include "libs/dualSenseBluetooth.cpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <linux/uhid.h>
#include <map>
#include <poll.h>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

std::vector<uint8_t> Unhex(const std::string& hex) {
	std::vector<uint8_t> out;
	for (size_t i = 0; i + 1 < hex.size(); i += 2) {
		out.push_back(static_cast<uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16)));
	}
	return out;
}

bool Write(int fd, const uhid_event& ev) {
	return ::write(fd, &ev, sizeof(ev)) == static_cast<ssize_t>(sizeof(ev));
}

int Fail(const char* why) {
	std::fprintf(stderr, "FAIL: %s\n", why);
	return 1;
}

} // namespace

int main() {
	const char* fixture_path = std::getenv("KYTY_DUALSENSE_FIXTURE");
	if (fixture_path == nullptr) {
		std::puts("SKIP: KYTY_DUALSENSE_FIXTURE not set");
		return 77;
	}
	std::ifstream     in(fixture_path);
	nlohmann::json    fixture = nlohmann::json::parse(in, nullptr, false);
	if (fixture.is_discarded()) {
		return Fail("fixture is not valid JSON");
	}
	const auto descriptor = Unhex(fixture["descriptor"].get<std::string>());
	const auto mac        = fixture["mac"].get<std::string>();
	std::map<uint8_t, std::vector<uint8_t>> features;
	for (auto& [key, value]: fixture["features"].items()) {
		features[static_cast<uint8_t>(std::stoi(key))] = Unhex(value.get<std::string>());
	}
	std::vector<std::vector<uint8_t>> inputs;
	for (auto& report: fixture["input"]) {
		inputs.push_back(Unhex(report.get<std::string>()));
	}
	if (descriptor.empty() || descriptor.size() > HID_MAX_DESCRIPTOR_SIZE || inputs.empty() || mac.empty()) {
		return Fail("fixture is missing descriptor, input or mac");
	}

	const int fd = ::open("/dev/uhid", O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		std::printf("SKIP: cannot open /dev/uhid: %s\n", std::strerror(errno));
		return 77;
	}
	uhid_event create {};
	create.type = UHID_CREATE2;
	std::snprintf(reinterpret_cast<char*>(create.u.create2.name), sizeof(create.u.create2.name), "%s",
	              fixture["name"].get<std::string>().c_str());
	std::snprintf(reinterpret_cast<char*>(create.u.create2.phys), sizeof(create.u.create2.phys), "kyty-host-tunnel");
	std::snprintf(reinterpret_cast<char*>(create.u.create2.uniq), sizeof(create.u.create2.uniq), "%s", mac.c_str());
	create.u.create2.rd_size = static_cast<uint16_t>(descriptor.size());
	create.u.create2.bus     = 0x05; // BUS_BLUETOOTH
	create.u.create2.vendor  = fixture["vid"].get<uint32_t>();
	create.u.create2.product = fixture["pid"].get<uint32_t>();
	std::memcpy(create.u.create2.rd_data, descriptor.data(), descriptor.size());
	if (!Write(fd, create)) {
		::close(fd);
		return Fail("UHID_CREATE2 failed");
	}

	// Kernel side: answer feature requests from the fixture, feed input at the 4 ms cadence.
	std::atomic<bool>     running {true};
	std::atomic<unsigned> get_reports {0};
	std::thread           worker([&] {
		size_t next  = 0;
		auto   due   = std::chrono::steady_clock::now();
		while (running) {
			pollfd pfd {fd, POLLIN, 0};
			if (::poll(&pfd, 1, 1) > 0) {
				uhid_event ev {};
				if (::read(fd, &ev, sizeof(ev)) > 0) {
					uhid_event reply {};
					if (ev.type == UHID_GET_REPORT) {
						get_reports++;
						reply.type                    = UHID_GET_REPORT_REPLY;
						reply.u.get_report_reply.id   = ev.u.get_report.id;
						auto it                       = features.find(ev.u.get_report.rnum);
						if (it == features.end()) {
							reply.u.get_report_reply.err = EIO;
						} else {
							reply.u.get_report_reply.size = static_cast<uint16_t>(it->second.size());
							std::memcpy(reply.u.get_report_reply.data, it->second.data(), it->second.size());
						}
						Write(fd, reply);
					} else if (ev.type == UHID_SET_REPORT) {
						reply.type                  = UHID_SET_REPORT_REPLY;
						reply.u.set_report_reply.id = ev.u.set_report.id;
						Write(fd, reply);
					}
				}
			}
			if (std::chrono::steady_clock::now() >= due) {
				uhid_event input {};
				input.type          = UHID_INPUT2;
				const auto& report  = inputs[next++ % inputs.size()];
				input.u.input2.size = static_cast<uint16_t>(report.size());
				std::memcpy(input.u.input2.data, report.data(), report.size());
				Write(fd, input);
				due += std::chrono::milliseconds(4);
			}
		}
	});

	int result = 0;
	SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5, "1");
	if (!SDL_Init(SDL_INIT_GAMEPAD)) {
		result = Fail(SDL_GetError());
	}
	SDL_Gamepad* pad = nullptr;
	SDL_JoystickID id = 0;
	for (int tries = 0; result == 0 && pad == nullptr && tries < 50; tries++) {
		SDL_PumpEvents();
		int   count = 0;
		auto* ids   = SDL_GetGamepads(&count);
		for (int i = 0; ids != nullptr && i < count; i++) {
			if (SDL_GetGamepadTypeForID(ids[i]) == SDL_GAMEPAD_TYPE_PS5) {
				id  = ids[i];
				pad = SDL_OpenGamepad(id);
				break;
			}
		}
		SDL_free(ids);
		if (pad == nullptr) {
			SDL_Delay(100);
		}
	}
	if (result == 0 && pad == nullptr) {
		result = Fail("no PS5 gamepad appeared within 5 s");
	}
	if (result == 0) {
		const char* serial = SDL_GetGamepadSerial(pad);
		std::printf("gamepad: %s serial=%s get_reports=%u\n", SDL_GetGamepadName(pad), serial ? serial : "(none)",
		            get_reports.load());
		if (SDL_GetGamepadConnectionState(pad) != SDL_JOYSTICK_CONNECTION_WIRELESS) {
			result = Fail("not reported as wireless");
		} else if (Libs::Controller::DualSenseBluetooth::Address(serial) !=
		           Libs::Controller::DualSenseBluetooth::Address(mac.c_str())) {
			result = Fail("serial does not match fixture MAC");
		} else if (!SDL_GamepadHasSensor(pad, SDL_SENSOR_GYRO) || !SDL_GamepadHasSensor(pad, SDL_SENSOR_ACCEL)) {
			result = Fail("gyro/accel sensors missing");
		} else if (auto* hid = Libs::Controller::DualSenseBluetooth::OpenDevice(static_cast<int>(id)); hid == nullptr) {
			result = Fail("emulator OpenDevice() refused the virtual pad");
		} else {
			SDL_hid_close(hid);
			std::puts("PASS");
		}
		SDL_CloseGamepad(pad);
	}
	SDL_Quit();
	running = false;
	worker.join();
	uhid_event destroy {};
	destroy.type = UHID_DESTROY;
	Write(fd, destroy);
	::close(fd);
	return result;
}
