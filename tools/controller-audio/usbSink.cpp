#include "usbSink.h"

#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <sstream>
namespace ControllerAudio::Receiver {
std::optional<UsbDevice> ResolveUsbBinding(const std::vector<std::string>& controllers,
                                           const std::vector<UsbNode>&     hids,
                                           const std::vector<UsbNode>&     audio,
                                           const std::string&              usb_selector,
                                           const std::string&              audio_selector) {
	std::vector<std::string> selected;
	for (auto& c: controllers)
		if (usb_selector.empty() || usb_selector == c ||
		    c.substr(c.find_last_of('/') + 1) == usb_selector)
			selected.push_back(c);
	if (selected.size() != 1) return {};
	UsbDevice result {selected.front(), {}, {}};
	size_t    hid_count = 0, audio_count = 0;
	for (auto& h: hids)
		if (h.usb_path == result.usb_path) {
			result.hid_path = h.path;
			hid_count++;
		}
	for (auto& a: audio)
		if (a.usb_path == result.usb_path) {
			result.audio_name = a.path;
			audio_count++;
		}
	if (hid_count != 1 || audio_count != 1 ||
	    (!audio_selector.empty() && audio_selector != result.audio_name))
		return {};
	return result;
}
std::optional<AlsaPlayback> ParseAlsaPlayback(const std::string& parameters,
                                              const std::string& status, uint32_t owner_pid,
                                              uint32_t sdl_period) {
	if (parameters.size() > 4096 || status.size() > 4096) return {};
	auto trim = [](std::string v) {
		auto first = v.find_first_not_of(" \t\r");
		if (first == std::string::npos) return std::string {};
		auto last = v.find_last_not_of(" \t\r");
		return v.substr(first, last - first + 1);
	};
	auto fields = [&](const std::string& text) {
		std::map<std::string, std::string> result;
		std::istringstream                 in(text);
		std::string                        line;
		while (std::getline(in, line)) {
			auto colon = line.find(':');
			if (colon == std::string::npos) continue;
			auto key = trim(line.substr(0, colon));
			if (!result.emplace(key, trim(line.substr(colon + 1))).second)
				return std::map<std::string, std::string> {};
		}
		return result;
	};
	auto p = fields(parameters), s = fields(status);
	auto number = [](const std::map<std::string, std::string>& data,
	                 const char*                               key) -> std::optional<uint64_t> {
		auto it = data.find(key);
		if (it == data.end()) return {};
		uint64_t n  = 0;
		auto parsed = std::from_chars(it->second.data(), it->second.data() + it->second.size(), n);
		if (parsed.ec != std::errc {} || parsed.ptr != it->second.data() + it->second.size())
			return {};
		return n;
	};
	auto channels = number(p, "channels"), period = number(p, "period_size"),
	     buffer = number(p, "buffer_size"), pid = number(s, "owner_pid"),
	     played = number(s, "hw_ptr");
	if (!channels || *channels != 4 || !period || !*period || *period != sdl_period || !buffer ||
	    *buffer < *period || *buffer > 1920 || *period > 1920 - *buffer || !pid ||
	    *pid != owner_pid || !played || *played > UINT64_MAX / 1000000)
		return {};
	if (p["rate"] != "48000" && p["rate"] != "48000 (48000/1)") return {};
	if (p["format"] != "S16_LE" || (s["state"] != "RUNNING" && s["state"] != "PREPARED")) return {};
	return AlsaPlayback {*buffer, *period, *played,
	                     ((*buffer + *period) * 1000000 + 47999) / 48000};
}
bool IsOwnedReport(const UsbReport& r) {
	if (r == SpeakerReport(true) || r == SpeakerReport(false)) return true;
	UsbReport rumble {};
	rumble[0] = 2;
	if (r[1] == 3 && r[3] <= 127 && r[4] <= 127) {
		rumble[1] = 3;
		rumble[3] = r[3];
		rumble[4] = r[4];
	}
	return r == rumble;
}
UsbReport SpeakerReport(bool enabled) {
	UsbReport out {};
	out[0] = 2;
	out[1] = enabled ? 0xa0 : 0x80;
	out[2] = 0x80;
	if (enabled) {
		out[6]  = 100;
		out[8]  = 0x30;
		out[38] = 2;
	}
	return out;
}
UsbReport RumbleReport(uint16_t large, uint16_t small) {
	UsbReport out {};
	out[0] = 2;
	// Classic USB emulation matches SDL's legacy firmware mode (half-scale motors).
	if (large || small) {
		out[1] = 3;
		out[3] = uint8_t(small >> 9);
		out[4] = uint8_t(large >> 9);
	}
	return out;
}
std::vector<float> MapQuad(Kind kind, std::span<const uint8_t> p) {
	if (p.size() % 8 || (kind != Kind::Speaker && kind != Kind::Actuator)) return {};
	std::vector<float> out(p.size() / 8 * 4, 0);
	size_t             offset = kind == Kind::Speaker ? 0 : 2;
	for (size_t i = 0; i < p.size() / 4; i++) {
		uint32_t b = 0;
		for (unsigned j = 0; j < 4; j++)
			b |= uint32_t(p[i * 4 + j]) << (8 * j);
		out[(i / 2) * 4 + offset + i % 2] = std::bit_cast<float>(b);
	}
	return out;
}
UsbSink::~UsbSink() {
	Reset();
}
bool UsbSink::Prepare() {
	if (prepared_) return true;
	prepared_ = output_->Prepare();
	if (!prepared_) output_->Shutdown();
	return prepared_;
}
bool UsbSink::Open(const Header& h) {
	if (!prepared_ || ports_.contains(h.port) || !output_->Open(h.port, h.rate)) return false;
	if (h.kind == Kind::Speaker && !output_->Report(SpeakerReport(true))) {
		output_->Close(h.port);
		return false;
	}
	ports_[h.port] = h;
	if (h.kind == Kind::Speaker) {
		last_ = {};
		return Apply(NowUs());
	}
	return true;
}
float UsbSink::FrequencyRatio(uint64_t queued_us) {
	return std::clamp(1.0f + (float(queued_us) - 15000.f) / 1000000.f, .995f, 1.005f);
}
bool UsbSink::Pcm(const Header& h, std::span<const uint8_t> p, uint64_t now) {
	if (!prepared_ || !ports_.contains(h.port) || !output_->Alive()) return false;
	auto queue    = output_->QueuedUs(h.port);
	auto duration = (p.size() / 8 * 1000000ULL + h.rate - 1) / h.rate;
	if (queue > 20000 || duration > 20000 - queue) return false;
	auto quad = MapQuad(h.kind, p);
	if (h.kind == Kind::Actuator && std::any_of(quad.begin(), quad.end(), [](float sample) {
		    return std::fabs(sample) > 1.f / 1024.f;
	    })) {
		pcm_until_ = std::max(pcm_until_, now + std::max<uint64_t>(queue + duration, 250000));
		if (!Apply(now)) return false;
	}
	return output_->Write(h.port, quad, FrequencyRatio(queue + duration));
}
bool UsbSink::Close(uint32_t port) {
	auto found = ports_.find(port);
	if (found == ports_.end()) return true;
	auto kind = found->second.kind;
	output_->Close(port);
	ports_.erase(found);
	bool same = false, ok = true;
	for (auto& [id, h]: ports_)
		same |= h.kind == kind;
	if (!same && kind == Kind::Speaker) {
		ok    = output_->Report(SpeakerReport(false));
		last_ = {};
		ok    = Apply(NowUs()) && ok;
	}
	if (!same && kind == Kind::Actuator) {
		pcm_until_ = 0;
		ok         = Apply(NowUs()) && ok;
	}
	return ok;
}
void UsbSink::Reset() {
	if (!prepared_) return;
	for (auto& [id, h]: ports_)
		output_->Close(id);
	ports_.clear();
	pcm_until_ = rumble_until_ = 0;
	large_ = small_ = 0;
	(void)output_->Report(RumbleReport(0, 0));
	(void)output_->Report(SpeakerReport(false));
	output_->Shutdown();
	prepared_ = false;
	last_     = {};
}
bool UsbSink::Apply(uint64_t now) {
	auto next = now < pcm_until_ || now >= rumble_until_ ? RumbleReport(0, 0)
	                                                     : RumbleReport(large_, small_);
	if (next == last_) return true;
	if (!output_->Report(next)) return false;
	last_ = next;
	return true;
}
bool UsbSink::Rumble(uint16_t large, uint16_t small, uint32_t duration, uint64_t now) {
	if (!prepared_ || duration > 1000) return false;
	large_        = large;
	small_        = small;
	rumble_until_ = now + uint64_t(duration) * 1000;
	return Apply(now);
}
bool UsbSink::Tick(uint64_t now) {
	return prepared_ && output_->Alive() && Apply(now);
}
uint64_t UsbSink::PendingFrames(uint32_t port, uint32_t rate) {
	auto us = output_->QueuedUs(port);
	return us > 20000 ? UINT64_MAX : (us * rate + 999999) / 1000000;
}
uint64_t UsbSink::QueuedUs(uint32_t port) {
	auto queued = output_->QueuedUs(port), latency = output_->PlaybackLatencyUs();
	return queued > UINT64_MAX - latency ? UINT64_MAX : queued + latency;
}
} // namespace ControllerAudio::Receiver
