#include "usbSink.h"

#include <SDL3/SDL.h>
#include <algorithm>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <linux/hidraw.h>
#include <poll.h>
#include <set>
#include <sound/asound.h>
#include <stdexcept>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
namespace ControllerAudio::Receiver {
namespace {
namespace fs = std::filesystem;
std::string ReadText(const fs::path& path) {
	std::ifstream in(path, std::ios::binary);
	std::string   text(4097, '\0');
	in.read(text.data(), text.size());
	text.resize(in.gcount());
	return text.size() > 4096 ? std::string {} : text;
}
std::string Read(const fs::path& path) {
	std::ifstream in(path);
	std::string   text;
	std::getline(in, text);
	return text;
}
fs::path UsbParent(fs::path path) {
	std::error_code ec;
	path = fs::canonical(path, ec);
	if (ec) return {};
	while (path != path.root_path()) {
		if (Read(path / "idVendor") == "054c" && Read(path / "idProduct") == "0ce6") return path;
		path = path.parent_path();
	}
	return {};
}
struct Selection {
	fs::path    usb, hid, proc;
	std::string name, identity;
};
Selection Discover(const Config& config) {
	std::set<fs::path> controllers;
	for (const auto& entry: fs::directory_iterator("/sys/bus/usb/devices")) {
		auto parent = UsbParent(entry.path());
		if (!parent.empty()) controllers.insert(parent);
	}
	if (!config.usb_path.empty()) {
		for (auto it = controllers.begin(); it != controllers.end();) {
			if (it->string() != config.usb_path && it->filename() != config.usb_path)
				it = controllers.erase(it);
			else
				++it;
		}
	}
	if (controllers.size() != 1)
		throw std::runtime_error(
		    "require one selected USB 054c:0ce6 controller (Bluetooth unsupported)");
	Selection selected;
	selected.usb = *controllers.begin();
	std::vector<fs::path> nodes;
	for (const auto& entry: fs::directory_iterator("/sys/class/hidraw"))
		if (UsbParent(entry.path() / "device") == selected.usb)
			nodes.push_back(fs::path("/dev") / entry.path().filename());
	if (nodes.size() != 1) throw std::runtime_error("USB controller hidraw association ambiguous");
	selected.hid = nodes.front();
	std::vector<std::string> endpoints;
	for (const auto& entry: fs::directory_iterator("/sys/class/sound")) {
		std::string node = entry.path().filename();
		int         card = -1, device = -1;
		char        tail = 0;
		if (std::sscanf(node.c_str(), "pcmC%dD%dp%c", &card, &device, &tail) != 2 ||
		    UsbParent(entry.path() / "device") != selected.usb)
			continue;
		// Control ioctls only identify the exact ALSA name SDL enumerates; no mixer/profile writes.
		auto ctl = "/dev/snd/controlC" + std::to_string(card);
		int  fd  = ::open(ctl.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
		if (fd < 0) throw std::runtime_error("cannot identify controller ALSA card");
		snd_ctl_card_info ci {};
		snd_pcm_info      pi {};
		pi.device = device;
		pi.stream = SNDRV_PCM_STREAM_PLAYBACK;
		bool ok   = ioctl(fd, SNDRV_CTL_IOCTL_CARD_INFO, &ci) == 0 &&
		            ioctl(fd, SNDRV_CTL_IOCTL_PCM_INFO, &pi) == 0;
		::close(fd);
		if (!ok) throw std::runtime_error("cannot identify controller ALSA playback endpoint");
		selected.proc = fs::path("/proc/asound") / ("card" + std::to_string(card)) /
		                ("pcm" + std::to_string(device) + "p");
		endpoints.emplace_back(std::string(reinterpret_cast<char*>(ci.name)) + ":" +
		                       reinterpret_cast<char*>(pi.name));
	}
	std::vector<UsbNode> audio_nodes, hid_nodes;
	for (auto& n: nodes)
		hid_nodes.push_back({selected.usb.string(), n.string()});
	for (auto& n: endpoints)
		audio_nodes.push_back({selected.usb.string(), n});
	auto binding = ResolveUsbBinding({selected.usb.string()}, hid_nodes, audio_nodes,
	                                 config.usb_path, config.audio_name);
	if (!binding)
		throw std::runtime_error("USB controller playback association ambiguous or name mismatch");
	selected.name     = binding->audio_name;
	selected.identity = Read(selected.usb / "busnum") + ":" + Read(selected.usb / "devnum");
	return selected;
}
class SdlOutput final: public UsbOutput {
	uint64_t          hardware_latency_us_ = 0, played_frames_ = 0;
	uint32_t          period_frames_ = 0;
	fs::path          status_path_;
	std::string       parameters_;
	Config            config_;
	Selection         selected_;
	int               hid_         = -1;
	SDL_AudioDeviceID device_      = 0;
	bool              initialized_ = false;
	struct Stream {
		SDL_AudioStream* stream;
		uint32_t         rate;
	};
	std::map<uint32_t, Stream> streams_;

public:
	explicit SdlOutput(Config c): config_(std::move(c)) {}
	~SdlOutput() override { Shutdown(); }
	bool Prepare() override {
		try {
			selected_ = Discover(config_);
			// SDL ALSA enumerates card:name:PCM names. This process-specific driver selection
			// avoids any Pulse/PipeWire default/profile changes and gives an auditable mapping.
			if (!SDL_SetHintWithPriority(SDL_HINT_AUDIO_DRIVER, "alsa", SDL_HINT_OVERRIDE) ||
			    !SDL_InitSubSystem(SDL_INIT_AUDIO))
				return false;
			initialized_ = true;
			if (std::string(SDL_GetCurrentAudioDriver()) != "alsa") {
				Shutdown();
				return false;
			}
			int               count     = 0;
			auto*             devices   = SDL_GetAudioPlaybackDevices(&count);
			SDL_AudioDeviceID candidate = 0;
			int               matches   = 0;
			for (int i = 0; i < count; i++) {
				auto* name = SDL_GetAudioDeviceName(devices[i]);
				if (name && selected_.name == name) {
					candidate = devices[i];
					matches++;
				}
			}
			SDL_free(devices);
			SDL_AudioSpec actual {};
			if (matches != 1) {
				Shutdown();
				return false;
			}
			// ALSA's unopened SDL device uses a stereo placeholder; validate the negotiated
			// format after opening the explicit ancestry-bound device.
			SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "240");
			const SDL_AudioSpec desired {SDL_AUDIO_F32, 4, 48000};
			device_           = SDL_OpenAudioDevice(candidate, &desired);
			int sample_frames = 0;
			if (!device_ || !SDL_GetAudioDeviceFormat(device_, &actual, &sample_frames) ||
			    actual.channels != 4 || actual.freq != 48000 || sample_frames <= 0 ||
			    sample_frames > 960) {
				Shutdown();
				return false;
			}
			period_frames_ = sample_frames;
			size_t owned   = 0;
			for (const auto& sub: fs::directory_iterator(selected_.proc)) {
				if (!sub.is_directory() || !sub.path().filename().string().starts_with("sub"))
					continue;
				auto params = ReadText(sub.path() / "hw_params"),
				     status = ReadText(sub.path() / "status");
				auto bounds = ParseAlsaPlayback(params, status, getpid(), period_frames_);
				if (!bounds) continue;
				owned++;
				parameters_          = std::move(params);
				status_path_         = sub.path() / "status";
				hardware_latency_us_ = bounds->latency_us;
				played_frames_       = bounds->played_frames;
			}
			if (owned != 1) {
				Shutdown();
				return false;
			}
			hid_ = ::open(selected_.hid.c_str(), O_WRONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
			hidraw_devinfo info {};
			struct stat    st {};
			if (hid_ < 0 || fstat(hid_, &st) || !S_ISCHR(st.st_mode) ||
			    ioctl(hid_, HIDIOCGRAWINFO, &info) || info.bustype != 3 || info.vendor != 0x054c ||
			    info.product != 0x0ce6) {
				Shutdown();
				return false;
			}
			if (!Alive()) {
				Shutdown();
				return false;
			}
			return true;
		} catch (const std::exception&) {
			Shutdown();
			return false;
		}
	}
	bool Open(uint32_t port, uint32_t rate) override {
		if (!device_ || streams_.contains(port)) return false;
		SDL_AudioSpec input {SDL_AUDIO_F32, 4, static_cast<int>(rate)},
		    output {SDL_AUDIO_F32, 4, 48000};
		auto* stream = SDL_CreateAudioStream(&input, &output);
		if (!stream) return false;
		if (!SDL_BindAudioStream(device_, stream)) {
			SDL_DestroyAudioStream(stream);
			return false;
		}
		streams_.emplace(port, Stream {stream, rate});
		return true;
	}
	bool Write(uint32_t port, std::span<const float> data, float ratio) override {
		auto it = streams_.find(port);
		return it != streams_.end() && SDL_SetAudioStreamFrequencyRatio(it->second.stream, ratio) &&
		       SDL_PutAudioStreamData(it->second.stream, data.data(),
		                              static_cast<int>(data.size_bytes()));
	}
	uint64_t PlaybackLatencyUs() override { return hardware_latency_us_; }
	uint64_t PlayedUs() override { return played_frames_ * 1000000 / 48000; }
	uint64_t QueuedUs(uint32_t port) override {
		auto it = streams_.find(port);
		if (it == streams_.end()) return 0;
		int queued = SDL_GetAudioStreamQueued(it->second.stream);
		if (queued < 0) return UINT64_MAX;
		return uint64_t(queued) * 1000000 / (uint64_t(it->second.rate) * 4 * sizeof(float));
	}
	bool Report(const UsbReport& report) override {
		// Only compiled, narrow encoders can reach this boundary. Refuse trigger/LED masks.
		if (hid_ < 0 || !IsOwnedReport(report)) return false;
		return ::write(hid_, report.data(), report.size()) == static_cast<ssize_t>(report.size());
	}
	void Close(uint32_t port) override {
		auto it = streams_.find(port);
		if (it != streams_.end()) {
			SDL_ClearAudioStream(it->second.stream);
			SDL_DestroyAudioStream(it->second.stream);
			streams_.erase(it);
		}
	}
	void Shutdown() override {
		while (!streams_.empty())
			Close(streams_.begin()->first);
		if (device_) {
			SDL_CloseAudioDevice(device_);
			device_ = 0;
		}
		if (hid_ >= 0) {
			::close(hid_);
			hid_ = -1;
		}
		hardware_latency_us_ = played_frames_ = period_frames_ = 0;
		status_path_.clear();
		parameters_.clear();
		if (initialized_) {
			SDL_QuitSubSystem(SDL_INIT_AUDIO);
			initialized_ = false;
		}
	}
	bool Alive() override {
		auto bounds =
		    ParseAlsaPlayback(parameters_, ReadText(status_path_), getpid(), period_frames_);
		if (!bounds || bounds->played_frames < played_frames_ ||
		    bounds->latency_us != hardware_latency_us_)
			return false;
		played_frames_ = bounds->played_frames;
		std::error_code ec;
		if (hid_ < 0 || !fs::exists(selected_.usb, ec) ||
		    Read(selected_.usb / "busnum") + ":" + Read(selected_.usb / "devnum") !=
		        selected_.identity)
			return false;
		if (UsbParent(fs::path("/sys/class/hidraw") / selected_.hid.filename() / "device") !=
		    selected_.usb)
			return false;
		pollfd fd {hid_, POLLOUT, 0};
		return ::poll(&fd, 1, 0) >= 0 && !(fd.revents & (POLLERR | POLLHUP | POLLNVAL));
	}
};
} // namespace
std::unique_ptr<UsbOutput> MakeSdlUsbOutput(const Config& config) {
	return std::make_unique<SdlOutput>(config);
}
} // namespace ControllerAudio::Receiver
