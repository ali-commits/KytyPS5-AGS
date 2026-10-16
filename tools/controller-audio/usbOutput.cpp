#include "usbSink.h"

#include <SDL3/SDL.h>
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <linux/hidraw.h>
#include <poll.h>
#include <set>
#include <sound/asound.h>
#include <sstream>
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
std::string ProcValue(const std::string& text, const std::string& field) {
	std::istringstream in(text);
	std::string        line;
	while (std::getline(in, line)) {
		auto colon = line.find(':');
		if (colon == std::string::npos) continue;
		auto key = line.substr(0, colon);
		key.erase(
		    std::remove_if(key.begin(), key.end(), [](char c) { return c == ' ' || c == '\t'; }),
		    key.end());
		if (key != field) continue;
		auto value = line.substr(colon + 1);
		auto first = value.find_first_not_of(" \t");
		return first == std::string::npos ? "?" : value.substr(first, 32);
	}
	return "?";
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
	std::string name, identity, alsa_spec;
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
		auto        pcm  = ParseAlsaPlaybackNode(node);
		if (!pcm || UsbParent(entry.path() / "device") != selected.usb) continue;
		auto [card, device] = *pcm;
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
		auto* id  = reinterpret_cast<const char*>(ci.id);
		auto spec = ExplicitAlsaPlaybackSpec(std::string(id, ::strnlen(id, sizeof(ci.id))), device);
		if (!spec)
			throw std::runtime_error(
			    "verified ALSA card ID is not a safe explicit hardware identifier");
		selected.alsa_spec = *spec;
		selected.proc      = fs::path("/proc/asound") / ("card" + std::to_string(card)) /
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
	int               hid_             = -1;
	SDL_AudioDeviceID device_          = 0;
	bool              initialized_     = false;
	uint64_t          next_diagnostic_ = 0;
	std::string       error_;
	bool              RuntimeFailure(const char* stage, const std::string& detail) {
		error_ = std::string(stage) + ": " + detail;
		for (auto& c: error_)
			if (c == '\n' || c == '\r' || c == '\t') c = ' ';
		if (error_.size() > 128) error_.resize(128);
		auto now = NowUs();
		if (now >= next_diagnostic_) {
			std::fprintf(stderr, "controller audio USB %.128s\n", error_.c_str());
			next_diagnostic_ = now + 2000000;
		}
		return false;
	}
	bool PrepareFailure(const char* stage, const char* detail) {
		RuntimeFailure((std::string("prepare ") + stage).c_str(), detail);
		Shutdown();
		return false;
	}

	struct Stream {
		SDL_AudioStream* stream;
		uint32_t         rate;
	};
	std::map<uint32_t, Stream> streams_;

public:
	explicit SdlOutput(Config c): config_(std::move(c)) {}
	~SdlOutput() override { Shutdown(); }
	bool Prepare() override {
		error_.clear();
		const char* stage = "discovery";
		try {
			selected_ = Discover(config_);
			// SDL ALSA enumerates card:name:PCM names. This process-specific driver selection
			// avoids any Pulse/PipeWire default/profile changes and gives an auditable mapping.
			if (!SDL_SetHintWithPriority(SDL_HINT_AUDIO_DRIVER, "alsa", SDL_HINT_OVERRIDE) ||
			    !SDL_InitSubSystem(SDL_INIT_AUDIO))
				return PrepareFailure("SDL audio initialization", SDL_GetError());
			initialized_ = true;
			auto* driver = SDL_GetCurrentAudioDriver();
			if (!driver || std::string(driver) != "alsa")
				return PrepareFailure("SDL driver", "required explicit ALSA backend unavailable");
			stage         = "SDL endpoint selection";
			int   count   = 0;
			auto* devices = SDL_GetAudioPlaybackDevices(&count);
			int   matches = 0;
			for (int i = 0; i < count; i++) {
				auto* name = SDL_GetAudioDeviceName(devices[i]);
				if (name && selected_.name == name) {
					matches++;
				}
			}
			SDL_free(devices);
			SDL_AudioSpec actual {};
			if (matches != 1) {
				auto detail = "expected one exact ancestry-bound SDL name; matches=" +
				              std::to_string(matches) + " name=" + selected_.name;
				return PrepareFailure(stage, detail.c_str());
			}
			// ALSA's unopened SDL device uses a stereo placeholder; validate the negotiated
			// format after opening the explicit ancestry-bound device.
			SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "240");
			const SDL_AudioSpec desired {SDL_AUDIO_F32, 4, 48000};
			// SDL's named ALSA path guesses a system prefix that may not accept DEV.
			// The playback override is process-local and pins its generic handle to the
			// exact verified USB card ID/device; it does not alter the system default.
			stage = "SDL exact hardware selection";
			if (!SDL_SetHintWithPriority(SDL_HINT_AUDIO_ALSA_DEFAULT_PLAYBACK_DEVICE,
			                             selected_.alsa_spec.c_str(), SDL_HINT_OVERRIDE))
				return PrepareFailure(stage, SDL_GetError());
			stage   = "SDL explicit output open";
			device_ = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &desired);
			if (!device_) return PrepareFailure(stage, SDL_GetError());
			int sample_frames = 0;
			if (!SDL_GetAudioDeviceFormat(device_, &actual, &sample_frames) ||
			    actual.channels != 4 || actual.freq != 48000 || sample_frames <= 0 ||
			    sample_frames > 960) {
				auto detail =
				    "expected quad48000/period1..960; channels=" + std::to_string(actual.channels) +
				    " rate=" + std::to_string(actual.freq) +
				    " period=" + std::to_string(sample_frames);
				return PrepareFailure("SDL negotiated format", detail.c_str());
			}
			stage          = "owned ALSA parameter/progress verification";
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
				auto detail = "require one PID-owned quadS16LE48k substream with verified "
				              "ring/progress; matches=" +
				              std::to_string(owned);
				return PrepareFailure(stage, detail.c_str());
			}
			stage = "hidraw open/USB identity";
			hid_  = ::open(selected_.hid.c_str(), O_WRONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
			hidraw_devinfo info {};
			struct stat    st {};
			if (hid_ < 0 || fstat(hid_, &st) || ioctl(hid_, HIDIOCGRAWINFO, &info))
				return PrepareFailure(stage, std::strerror(errno));
			if (!S_ISCHR(st.st_mode) || info.bustype != 3 || info.vendor != 0x054c ||
			    info.product != 0x0ce6)
				return PrepareFailure(stage, "expected USB character node for054c:0ce6");
			if (!Alive())
				return PrepareFailure("final USB liveness",
				                      "USB identity or owned ALSA progress unavailable/regressed");
			return true;
		} catch (const std::exception& error) {
			return PrepareFailure(stage, error.what());
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
		if (it == streams_.end()) return RuntimeFailure("PCM stream lookup", "port not bound");
		if (!SDL_SetAudioStreamFrequencyRatio(it->second.stream, ratio))
			return RuntimeFailure("SDL frequency ratio", SDL_GetError());
		if (!SDL_PutAudioStreamData(it->second.stream, data.data(),
		                            static_cast<int>(data.size_bytes())))
			return RuntimeFailure("SDL PCM write", SDL_GetError());
		return true;
	}
	std::string QueueState(uint32_t port) override {
		auto found = streams_.find(port);
		if (found == streams_.end()) return "stream missing";
		auto          status = ReadText(status_path_);
		SDL_AudioSpec source {};
		SDL_GetAudioStreamFormat(found->second.stream, &source, nullptr);
		std::string_view format = SDL_GetAudioFormatName(source.format);
		if (format.starts_with("SDL_AUDIO_")) format.remove_prefix(10);
		auto summary =
		    "state=" + ProcValue(status, "state") + " hw=" + ProcValue(status, "hw_ptr") +
		    " appl=" + ProcValue(status, "appl_ptr") +
		    " b=" + std::to_string(SDL_GetAudioStreamQueued(found->second.stream)) +
		    " src=" + std::to_string(source.freq) + "/" + std::to_string(source.channels) + "/" +
		    std::string(format) +
		    " r=" + std::to_string(SDL_GetAudioStreamFrequencyRatio(found->second.stream));
		RuntimeFailure("queue snapshot", summary);
		return summary;
	}
	uint64_t PlaybackLatencyUs() override { return hardware_latency_us_; }
	uint64_t PlayedUs() override { return played_frames_ * 1000000 / 48000; }
	uint64_t QueuedUs(uint32_t port) override {
		auto it = streams_.find(port);
		if (it == streams_.end()) return 0;
		int queued = SDL_GetAudioStreamQueued(it->second.stream);
		if (queued < 0) {
			RuntimeFailure("SDL queued input", SDL_GetError());
			return UINT64_MAX;
		}
		return uint64_t(queued) * 1000000 / (uint64_t(it->second.rate) * 4 * sizeof(float));
	}
	bool Report(const UsbReport& report) override {
		// Only compiled, narrow encoders can reach this boundary. Refuse trigger/LED masks.
		if (hid_ < 0) return RuntimeFailure("hidraw report", "descriptor closed");
		if (!IsOwnedReport(report))
			return RuntimeFailure("hidraw report", "encoding outside helper-owned fields");
		auto n = ::write(hid_, report.data(), report.size());
		if (n != static_cast<ssize_t>(report.size()))
			return RuntimeFailure("hidraw write",
			                      "bytes=" + std::to_string(n) + " " + std::strerror(errno));
		return true;
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
	std::string Error() const override { return error_; }
	bool        Alive() override {
		auto status = ReadText(status_path_);
		auto bounds = ParseAlsaPlayback(parameters_, status, getpid(), period_frames_);
		if (!bounds)
			return RuntimeFailure("ALSA proc validation",
			                      status.empty() ? "status unavailable" : status);
		if (bounds->played_frames < played_frames_)
			return RuntimeFailure("ALSA progress regression",
			                      "previous=" + std::to_string(played_frames_) +
			                          " current=" + std::to_string(bounds->played_frames));
		if (bounds->latency_us != hardware_latency_us_)
			return RuntimeFailure("ALSA playback bound", "latency changed");
		played_frames_ = bounds->played_frames;
		std::error_code ec;
		if (hid_ < 0 || !fs::exists(selected_.usb, ec))
			return RuntimeFailure("USB identity", "device path or descriptor unavailable");
		if (Read(selected_.usb / "busnum") + ":" + Read(selected_.usb / "devnum") !=
		    selected_.identity)
			return RuntimeFailure("USB identity", "bus/device number changed");
		if (UsbParent(fs::path("/sys/class/hidraw") / selected_.hid.filename() / "device") !=
		    selected_.usb)
			return RuntimeFailure("hidraw ancestry", "USB parent changed");
		pollfd fd {hid_, POLLOUT, 0};
		auto   n = ::poll(&fd, 1, 0);
		if (n < 0) return RuntimeFailure("hidraw poll", std::strerror(errno));
		if (fd.revents & (POLLERR | POLLHUP | POLLNVAL))
			return RuntimeFailure("hidraw poll", "revents=" + std::to_string(fd.revents));
		return true;
	}
};
} // namespace
std::unique_ptr<UsbOutput> MakeSdlUsbOutput(const Config& config) {
	return std::make_unique<SdlOutput>(config);
}
} // namespace ControllerAudio::Receiver
