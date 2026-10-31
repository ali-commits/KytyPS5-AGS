#include "receiver.h"

#include <algorithm>
#include <chrono>
#include <fcntl.h>
#include <limits>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <unistd.h>
namespace ControllerAudio::Receiver {
uint64_t NowUs() {
	return std::chrono::duration_cast<std::chrono::microseconds>(
	           std::chrono::steady_clock::now().time_since_epoch())
	    .count();
}
bool ClockMap::Sample(uint64_t sent, uint64_t receive, uint64_t server_receive,
                      uint64_t server_send) {
	if (receive < sent || server_send < server_receive ||
	    receive - sent < server_send - server_receive || receive - sent > 1000000 ||
	    receive > INT64_MAX || server_send > INT64_MAX)
		return false;
	uint64_t rtt = receive - sent - (server_send - server_receive);
	if (!Fresh(receive) || receive - best_sample_at >= 5000000) best_rtt = UINT64_MAX;
	if (rtt <= best_rtt) {
		best_rtt       = rtt;
		uncertainty    = (rtt + 1) / 2;
		offset         = static_cast<int64_t>((static_cast<long double>(server_receive) - sent +
		                                       static_cast<long double>(server_send) - receive) /
		                                      2);
		best_sample_at = receive;
	}
	if (rtt <= 40000) sampled_at = receive;
	return true;
}
bool ClockMap::Fresh(uint64_t now) const {
	return uncertainty <= 20000 && now >= sampled_at && now - sampled_at <= 10000000;
}
PlaybackClockState PlaybackClockGate::Observe(uint64_t                             now,
                                              std::optional<PlaybackClockProgress> progress) {
	if (state_ != PlaybackClockState::Waiting) return state_;
	if (now < start_ || now - start_ >= BudgetUs || !progress)
		return state_ = PlaybackClockState::Failed;
	if (previous_) {
		if (progress->hardware_frames < previous_->hardware_frames ||
		    progress->application_frames < previous_->application_frames ||
		    (!progress->running && previous_->running))
			return state_ = PlaybackClockState::Failed;
		if (progress->running && previous_->running &&
		    progress->hardware_frames > previous_->hardware_frames &&
		    progress->application_frames > 0)
			return state_ = PlaybackClockState::Ready;
	}
	previous_ = progress;
	return state_;
}
struct NullSink::Data {
	std::array<EVP_MD_CTX*, 2> hashes {};
	std::array<uint64_t, 2>    bytes {};
	int                        raw = -1;
	Data(bool diagnostics) {
		if (diagnostics)
			for (auto& h: hashes) {
				h = EVP_MD_CTX_new();
				if (!h || EVP_DigestInit_ex(h, EVP_sha256(), nullptr) != 1)
					throw std::runtime_error("hash initialization failed");
			}
	}
	~Data() {
		for (auto* h: hashes)
			EVP_MD_CTX_free(h);
		if (raw >= 0) ::close(raw);
	}
};
NullSink::NullSink(const std::string& raw_path, bool diagnostics)
    : data_(std::make_unique<Data>(diagnostics)) {
	if (!raw_path.empty()) {
		data_->raw =
		    ::open(raw_path.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0600);
		if (data_->raw < 0)
			throw std::runtime_error("raw diagnostics path unavailable (must be new)");
	}
}
NullSink::~NullSink() = default;
bool NullSink::Prepare() {
	return true;
}
bool NullSink::Open(const Header&) {
	return true;
}
bool NullSink::Pcm(const Header& h, std::span<const uint8_t> p, uint64_t) {
	auto i = h.kind == Kind::Speaker ? 1 : 0;
	if (data_->hashes[i] && EVP_DigestUpdate(data_->hashes[i], p.data(), p.size()) != 1)
		return false;
	data_->bytes[i] += p.size();
	if (data_->raw >= 0) {
		auto   encoded = Encode({h, {p.begin(), p.end()}});
		size_t at      = 0;
		while (at < encoded.size()) {
			auto n = ::write(data_->raw, encoded.data() + at, encoded.size() - at);
			if (n <= 0) return false;
			at += n;
		}
	}
	return true;
}
bool NullSink::Close(uint32_t) {
	return true;
}
void NullSink::Reset() {}
bool NullSink::Rumble(uint16_t, uint16_t, uint32_t, uint64_t) {
	return true;
}
bool NullSink::Tick(uint64_t) {
	return true;
}
uint64_t NullSink::QueuedUs(uint32_t) {
	return 0;
}
std::string NullSink::Hash(Kind kind) const {
	if (!data_->hashes[kind == Kind::Speaker ? 1 : 0]) return {};
	auto* ctx = EVP_MD_CTX_new();
	if (!ctx) return {};
	std::array<unsigned char, 32> digest {};
	unsigned                      length = 0;
	bool ok = EVP_MD_CTX_copy_ex(ctx, data_->hashes[kind == Kind::Speaker ? 1 : 0]) == 1 &&
	          EVP_DigestFinal_ex(ctx, digest.data(), &length) == 1;
	EVP_MD_CTX_free(ctx);
	if (!ok) return {};
	std::string out;
	for (auto v: digest) {
		out.push_back("0123456789abcdef"[v >> 4]);
		out.push_back("0123456789abcdef"[v & 15]);
	}
	return out;
}
uint64_t NullSink::Bytes(Kind kind) const {
	return data_->bytes[kind == Kind::Speaker ? 1 : 0];
}
bool Session::Fail(const char* text) {
	error_ = text;
	Disconnect();
	return false;
}
void Session::Disconnect() {
	sink_.Reset();
	ports_          = {};
	delivery_count_ = 0;
	ready_ = active_ = false;
	generation_ = epoch_ = control_sequence_ = 0;
}
bool Session::Handle(const Frame& frame, uint64_t now) {
	// Every caller, including direct fixtures, receives full wire validation.
	if (Encode(frame).empty()) return Fail("invalid frame");
	const auto& h = frame.header;
	if (h.type == Type::State) {
		auto json =
		    nlohmann::json::parse(frame.payload.begin(), frame.payload.end(), nullptr, false);
		if (json.is_discarded() || !json.is_object() || !json.contains("state") ||
		    !json["state"].is_string())
			return Fail("invalid state");
		auto state = json["state"].get<std::string>();
		if (json.contains("reason") && (!json["reason"].is_string() ||
		                                json["reason"].get_ref<const std::string&>().size() > 128))
			return Fail("invalid state reason");
		if (state == "waiting" || state == "reset" || state == "error") {
			Disconnect();
			return true;
		}
		if (state == "active") {
			if (!ready_ || h.generation != generation_ || h.producer_epoch != epoch_)
				return Fail("active lease mismatch");
			active_ = true;
			return true;
		}
		// Durable generations order leases; a new producer process may restart its local epoch.
		if (state != "preparing" || !h.generation || !h.producer_epoch ||
		    h.generation <= last_generation_)
			return Fail("replayed lease");
		// Readiness commits the fresh lease only after both clock and output validate.
		if (!clock.Fresh(now)) return Fail("clock uncertainty or expiry");
		Disconnect();
		last_generation_ = generation_ = h.generation;
		epoch_                         = h.producer_epoch;
		if (!sink_.Prepare()) return Fail("output unavailable");
		ready_ = true;
		error_.clear();
		return true;
	}
	if (!ready_ || h.generation != generation_ || h.producer_epoch != epoch_)
		return Fail("lease mismatch");
	if (h.type == Type::Reset) {
		Disconnect();
		return true;
	}
	if (!active_) return Fail("PCM before active state");
	if (h.type == Type::Rumble) {
		if (h.sequence <= control_sequence_) return Fail("control sequence");
		control_sequence_ = h.sequence;
		auto&    p        = frame.payload;
		uint16_t large = (uint16_t(p[0]) << 8) | p[1], small = (uint16_t(p[2]) << 8) | p[3];
		uint32_t duration =
		    (uint32_t(p[4]) << 24) | (uint32_t(p[5]) << 16) | (uint32_t(p[6]) << 8) | p[7];
		return sink_.Rumble(large, small, duration, now) || Fail("rumble output failed");
	}
	if (h.type != Type::Open && h.type != Type::Pcm && h.type != Type::Close)
		return Fail("message direction");
	auto& port = ports_[h.port];
	if (h.type == Type::Open) {
		if (port.open || h.port_generation <= port.h.port_generation || h.first_frame != 0)
			return Fail("recycled port");
		if (!sink_.Open(h)) return Fail("port output failed");
		port = {true, h, 0, {}};
		return true;
	}
	if (!port.open || h.kind != port.h.kind || h.rate != port.h.rate ||
	    h.port_generation != port.h.port_generation || h.sequence <= port.h.sequence)
		return Fail("port metadata or sequence");
	if (h.type == Type::Close) {
		delivery_count_ -= port.pending.size();
		port.pending.clear();
		if (!sink_.Close(h.port)) return Fail("port close failed");
		port.open = false;
		port.h    = h;
		return true;
	}
	if (h.first_frame != port.next || h.first_frame > UINT64_MAX - frame.payload.size() / 8)
		return Fail("sample continuity");
	if (!clock.Fresh(now)) return Fail("clock expired");
	if (now > INT64_MAX || h.source_time_us > INT64_MAX) return Fail("clock overflow");
	const auto host_now = static_cast<long double>(now) + clock.offset;
	const auto age      = host_now - static_cast<long double>(h.source_time_us);
	auto       queue    = sink_.QueuedUs(h.port);
	const auto duration = (frame.payload.size() / 8 * 1000000ULL + h.rate - 1) / h.rate;
	if (age < -static_cast<long double>(clock.uncertainty) || queue > 60000 ||
	    age + clock.uncertainty + queue + duration > 80000)
		return Fail("delivery deadline or queue exceeded");
	if (delivery_count_ >= 4096) return Fail("delivery record limit");
	if (!sink_.Pcm(h, frame.payload, now)) return Fail("output failed");
	bytes_ += frame.payload.size();
	port.next += frame.payload.size() / 8;
	port.pending.push_back({port.next, h.source_time_us});
	delivery_count_++;
	port.h = h;
	return true;
}
uint64_t Session::QueuedUs() {
	uint64_t queued = 0;
	for (uint32_t i = 0; i < ports_.size(); i++)
		if (ports_[i].open) queued = std::max(queued, sink_.QueuedUs(i));
	return queued;
}
bool Session::Tick(uint64_t now) {
	if (!ready_) return true;
	if (!clock.Fresh(now)) return Fail("clock expired");
	if (!sink_.Tick(now)) return Fail("device lost");
	auto played = sink_.PlayedUs(), latency = sink_.PlaybackLatencyUs();
	if (played > UINT64_MAX - latency) return Fail("playback progress overflow");
	const auto host_now = static_cast<long double>(now) + clock.offset;
	for (uint32_t id = 0; id < ports_.size(); id++) {
		auto& port = ports_[id];
		if (!port.open || port.pending.empty()) continue;
		auto pending = sink_.PendingFrames(id, port.h.rate);
		if (pending == UINT64_MAX) return Fail("playback queue unavailable");
		auto consumed = port.next - std::min(port.next, pending);
		for (auto& delivery: port.pending) {
			if (delivery.end_frame > consumed) break;
			if (!delivery.transferred) {
				delivery.transferred      = true;
				delivery.retire_played_us = played + latency;
			}
		}
		while (!port.pending.empty() && port.pending.front().transferred &&
		       played >= port.pending.front().retire_played_us) {
			port.pending.pop_front();
			delivery_count_--;
		}
		for (const auto& delivery: port.pending) {
			auto remaining =
			    delivery.transferred ? delivery.retire_played_us - played : sink_.QueuedUs(id);
			if (remaining > 60000 ||
			    host_now - delivery.source_time_us + clock.uncertainty + remaining > 80000)
				return Fail("queued sample delivery deadline");
		}
	}
	return true;
}
bool Framer::Feed(std::span<const uint8_t>                 bytes,
                  const std::function<bool(const Frame&)>& consume) {
	// Copy at most one bounded frame; a large socket read cannot allocate an unbounded batch.
	while (!bytes.empty()) {
		size_t wanted = HeaderBytes;
		if (bytes_.size() >= HeaderBytes) {
			auto n = WireSize(bytes_);
			if (!n) {
				Clear();
				return false;
			}
			wanted = *n;
		}
		size_t n = std::min(bytes.size(), wanted - bytes_.size());
		bytes_.insert(bytes_.end(), bytes.begin(), bytes.begin() + n);
		bytes = bytes.subspan(n);
		if (bytes_.size() < HeaderBytes) continue;
		auto total = WireSize(bytes_);
		if (!total) {
			Clear();
			return false;
		}
		if (bytes_.size() == *total) {
			auto decoded = Decode(bytes_);
			Clear();
			if (!decoded || !consume(*decoded)) return false;
		}
	}
	return true;
}
} // namespace ControllerAudio::Receiver
