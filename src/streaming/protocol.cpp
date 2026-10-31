#include "streaming/protocol.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace Streaming {
namespace {
bool Unsigned(const Json& value, uint64_t minimum, uint64_t maximum) {
	if (!value.is_number_unsigned() && !value.is_number_integer()) return false;
	if (value.is_number_integer() && !value.is_number_unsigned() && value.get<int64_t>() < 0)
		return false;
	const auto n = value.get<uint64_t>();
	return n >= minimum && n <= maximum;
}
bool Signed(const Json& value, int64_t minimum, int64_t maximum) {
	if (!value.is_number_integer()) return false;
	if (value.is_number_unsigned() && value.get<uint64_t>() > uint64_t(maximum)) return false;
	const auto n = value.get<int64_t>();
	return n >= minimum && n <= maximum;
}
} // namespace
bool ValidateHello(const Json& p, size_t fd_count, PoolDescriptor& output) noexcept {
	for (const char* key:
	     {"type", "version", "epoch", "width", "height", "stride", "fourcc", "gpu_uuid", "slots",
	      "allocation_sizes", "offset", "dedicated", "transport"})
		if (!p.is_object() || !p.contains(key)) return false;
	if (!p.is_object() || fd_count != 9 || p.at("type") != "hello" ||
	    !Unsigned(p.at("version"), 1, 1) || !Unsigned(p.at("epoch"), 1, UINT64_MAX) ||
	    !Unsigned(p.at("width"), 1, 4096) || !Unsigned(p.at("height"), 1, 4096) ||
	    !Unsigned(p.at("stride"), 4096, 268435456) || p.at("fourcc") != "BGRA" ||
	    !Unsigned(p.at("slots"), 3, 3) || !Unsigned(p.at("offset"), 0, 0) ||
	    !p.at("dedicated").is_boolean() || p.at("dedicated") != true ||
	    p.at("transport") != "vulkan-buffer-fd")
		return false;
	PoolDescriptor d;
	d.epoch  = p.at("epoch");
	d.width  = p.at("width");
	d.height = p.at("height");
	d.stride = p.at("stride");
	if (d.stride % 4096 != 0 || uint64_t(d.stride) < uint64_t(d.width) * 4) return false;
	if (!p.at("gpu_uuid").is_string()) return false;
	d.gpu_uuid = p.at("gpu_uuid").get<std::string>();
	if (d.gpu_uuid.size() != 32 || !std::all_of(d.gpu_uuid.begin(), d.gpu_uuid.end(), [](char c) {
		    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
	    }))
		return false;
	auto& sizes = p.at("allocation_sizes");
	if (!sizes.is_array() || sizes.size() != 3) return false;
	for (size_t i = 0; i < SlotCount; i++) {
		if (!Unsigned(sizes[i], uint64_t(d.stride) * d.height, 268435456)) return false;
		d.allocation_sizes[i] = sizes[i];
	}
	output = std::move(d);
	return true;
}
bool ParseInput(const Json& p, Input& output) noexcept {
	for (const char* key: {"type", "controller", "sequence", "connected", "buttons", "lx", "ly",
	                       "rx", "ry", "lt", "rt"})
		if (!p.is_object() || !p.contains(key)) return false;
	if (!p.is_object() || p.at("type") != "input" || !Unsigned(p.at("controller"), 0, 0) ||
	    !Unsigned(p.at("sequence"), 1, UINT64_MAX) || !p.at("connected").is_boolean())
		return false;
	Input i;
	i.sequence  = p.at("sequence");
	i.connected = p.at("connected");
	if (p.contains("controller_type") || p.contains("controller_capabilities") ||
	    p.contains("supported_buttons")) {
		if (!p.contains("controller_type") || !p.contains("controller_capabilities") ||
		    !p.contains("supported_buttons") || !Unsigned(p.at("controller_type"), 0, 255) ||
		    !Unsigned(p.at("controller_capabilities"), 0, 65535) ||
		    !Unsigned(p.at("supported_buttons"), 0, UINT32_MAX))
			return false;
		i.controller_type         = p.at("controller_type");
		i.controller_capabilities = p.at("controller_capabilities");
		i.supported_buttons       = p.at("supported_buttons");
	}
	constexpr std::array names {"a",     "b",    "x",  "y",  "up", "down", "left", "right",
	                            "start", "back", "lb", "rb", "ls", "rs",   "guide"};
	constexpr std::array<uint32_t, 15> bits {0x4000, 0x2000, 0x8000, 0x1000, 0x10,
	                                         0x40,   0x80,   0x20,   0x8,    0x100000,
	                                         0x400,  0x800,  0x2,    0x4,    0x10000};
	const auto&                        buttons = p.at("buttons");
	if (!buttons.is_array() || buttons.size() > names.size()) return false;
	uint32_t seen = 0;
	for (const auto& b: buttons) {
		if (!b.is_string()) return false;
		const auto name = b.get<std::string>();
		const auto it   = std::find(names.begin(), names.end(), name);
		if (it == names.end()) return false;
		const auto bit = bits[it - names.begin()];
		if (seen & bit) return false;
		seen |= bit;
	}
	i.buttons = seen;
	constexpr std::array axes {"lx", "ly", "rx", "ry", "lt", "rt"};
	for (size_t n = 0; n < 6; n++) {
		const auto& v = p.at(axes[n]);
		if (!Signed(v, n < 4 ? -32768 : 0, n < 4 ? 32767 : 255)) return false;
		const auto value = v.get<int>();
		i.axes[n]        = n < 4 ? (255 * (value + 32768) + 32767) / 65535 : value;
	}
	if (!i.connected) {
		i.buttons = 0;
		i.axes    = {128, 128, 128, 128, 0, 0};
	}
	output = i;
	return true;
}
bool HasCapability(const Json& p, std::string_view name) noexcept {
	if (!p.is_object() || !p.contains("capabilities")) return false;
	const auto& caps = p.at("capabilities");
	if (!caps.is_array() || caps.size() > 16) return false;
	for (const auto& cap: caps)
		if (!cap.is_string()) return false;
	return std::find(caps.begin(), caps.end(), name) != caps.end();
}
bool ParseMotion(const Json& p, Motion& output) noexcept {
	for (const auto* key:
	     {"type", "controller", "sensor", "sequence", "timestamp_us", "x", "y", "z"})
		if (!p.is_object() || !p.contains(key)) return false;
	if (p.at("type") != "motion" || !Unsigned(p.at("controller"), 0, 0) ||
	    !Unsigned(p.at("sequence"), 1, UINT64_MAX) ||
	    !Unsigned(p.at("timestamp_us"), 1, INT64_MAX) ||
	    (p.at("sensor") != "accel" && p.at("sensor") != "gyro"))
		return false;
	Motion candidate;
	candidate.sensor   = p.at("sensor") == "gyro" ? Motion::Sensor::Gyro : Motion::Sensor::Accel;
	candidate.sequence = p.at("sequence");
	candidate.timestamp_us     = p.at("timestamp_us");
	const double         limit = candidate.sensor == Motion::Sensor::Gyro ? 40.0 : 160.0;
	constexpr std::array names {"x", "y", "z"};
	for (size_t i = 0; i < names.size(); i++) {
		const auto& value = p.at(names[i]);
		if (!value.is_number()) return false;
		const double n = value.get<double>();
		if (!std::isfinite(n) || std::abs(n) > limit) return false;
		candidate.value[i] = static_cast<float>(n);
	}
	output = candidate;
	return true;
}
bool ParseControllerAudioState(const Json& p, ControllerAudioState& output) noexcept {
	if (!p.is_object() || p.value("type", Json()) != "controller-audio-state" ||
	    !p.contains("ready") || !p["ready"].is_boolean() || !p.contains("generation") ||
	    !Unsigned(p["generation"], 1, UINT64_MAX))
		return false;
	ControllerAudioState state;
	state.ready      = p["ready"];
	state.generation = p["generation"];
	if (state.ready) {
		if (!p.contains("producer_epoch") || !Unsigned(p["producer_epoch"], 1, UINT64_MAX) ||
		    !p.contains("socket") || !p["socket"].is_string() || !p.contains("nonce") ||
		    !p["nonce"].is_string())
			return false;
		state.producer_epoch = p["producer_epoch"];
		state.path           = p["socket"].get<std::string>();
		state.nonce_hex      = p["nonce"].get<std::string>();
		if (state.path.empty() || state.path[0] != '/' || state.path.size() >= 108 ||
		    state.path.find('\0') != std::string::npos || state.nonce_hex.size() != 64 ||
		    state.nonce_hex.find_first_not_of("0123456789abcdef") != std::string::npos)
			return false;
	}
	output = std::move(state);
	return true;
}
std::optional<Lease> LeaseRing::TryAcquire() {
	std::unique_lock lock(m_mutex, std::try_to_lock);
	if (!lock.owns_lock()) return {};
	if (!m_connected || m_sequence == UINT64_MAX) return {};
	for (uint32_t i = 0; i < SlotCount; i++)
		if (m_leased[i] == 0) {
			m_leased[i] = ++m_sequence;
			Lease lease {m_epoch, m_sequence, i, m_used[i]};
			m_used[i] = true;
			return lease;
		}
	return {};
}
bool LeaseRing::Release(uint64_t epoch, uint32_t slot, uint64_t sequence) {
	std::lock_guard lock(m_mutex);
	if (!m_connected || epoch != m_epoch || slot >= SlotCount || sequence == 0 ||
	    m_leased[slot] != sequence)
		return false;
	m_leased[slot] = 0;
	return true;
}
void LeaseRing::Disconnect() {
	std::lock_guard lock(m_mutex);
	m_connected = false;
}
} // namespace Streaming
