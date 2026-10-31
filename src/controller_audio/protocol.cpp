#include "controller_audio/protocol.h"

#include <bit>
#include <cmath>
#include <cstring>
namespace ControllerAudio {
namespace {
uint64_t Read(std::span<const uint8_t> data, size_t at, size_t n) {
	uint64_t value = 0;
	for (size_t i = 0; i < n; i++)
		value = (value << 8) | data[at + i];
	return value;
}
void Write(std::vector<uint8_t>& data, size_t at, uint64_t value, size_t n) {
	for (size_t i = 0; i < n; i++) {
		data[at + n - 1 - i] = uint8_t(value);
		value >>= 8;
	}
}
bool Valid(const Header& h, size_t bytes) {
	const auto type = static_cast<unsigned>(h.type);
	if (type < 1 || type > 12 || bytes > PayloadLimit) return false;
	if (h.type == Type::Open || h.type == Type::Pcm || h.type == Type::Close) {
		if ((h.kind != Kind::Actuator && h.kind != Kind::Speaker) || h.channels != 2 ||
		    h.port >= 32 || h.rate < 1000 || h.rate > 192000 || !h.port_generation ||
		    !h.generation || !h.producer_epoch || !h.sequence)
			return false;
		if (h.type != Type::Pcm) return bytes == 0;
		return bytes && bytes % 8 == 0 && bytes / 8 <= (h.rate + 99) / 100 && h.source_time_us;
	}
	if (h.kind != Kind::Control || h.channels || h.port || h.rate || h.port_generation ||
	    h.first_frame)
		return false;
	if (h.type == Type::Rumble) return bytes == 8 && h.sequence;
	if (h.type == Type::Reset || h.type == Type::Ready) return bytes == 0;
	return bytes <= 4096;
}
Header ReadHeader(std::span<const uint8_t> d) {
	return {Type(d[5]),
	        Kind(d[6]),
	        d[7],
	        Read(d, 16, 8),
	        Read(d, 24, 8),
	        uint32_t(Read(d, 32, 4)),
	        uint32_t(Read(d, 36, 4)),
	        Read(d, 40, 8),
	        Read(d, 48, 8),
	        Read(d, 56, 8),
	        Read(d, 64, 8)};
}
bool PayloadValid(const Frame& f) {
	if (!Valid(f.header, f.payload.size())) return false;
	if (f.header.type == Type::Rumble) return Read(f.payload, 4, 4) <= 1000;
	if (f.header.type == Type::Pcm)
		for (size_t i = 0; i < f.payload.size(); i += 4) {
			uint32_t bits = 0;
			for (unsigned j = 0; j < 4; j++)
				bits |= uint32_t(f.payload[i + j]) << (8 * j);
			if (!std::isfinite(std::bit_cast<float>(bits))) return false;
		}
	return true;
}
} // namespace
std::optional<size_t> WireSize(std::span<const uint8_t> d) {
	if (d.size() < HeaderBytes || d[0] != 'K' || d[1] != 'C' || d[2] != 'P' || d[3] != 'A' ||
	    d[4] != 1 || Read(d, 8, 4) != HeaderBytes)
		return {};
	const auto bytes = Read(d, 12, 4);
	if (!Valid(ReadHeader(d), bytes)) return {};
	return HeaderBytes + size_t(bytes);
}
std::vector<uint8_t> Encode(const Frame& f) {
	if (!PayloadValid(f)) return {};
	std::vector<uint8_t> d(HeaderBytes + f.payload.size());
	d[0] = 'K';
	d[1] = 'C';
	d[2] = 'P';
	d[3] = 'A';
	d[4] = 1;
	d[5] = uint8_t(f.header.type);
	d[6] = uint8_t(f.header.kind);
	d[7] = f.header.channels;
	Write(d, 8, HeaderBytes, 4);
	Write(d, 12, f.payload.size(), 4);
	const auto& h = f.header;
	Write(d, 16, h.generation, 8);
	Write(d, 24, h.producer_epoch, 8);
	Write(d, 32, h.port, 4);
	Write(d, 36, h.rate, 4);
	Write(d, 40, h.port_generation, 8);
	Write(d, 48, h.sequence, 8);
	Write(d, 56, h.first_frame, 8);
	Write(d, 64, h.source_time_us, 8);
	std::copy(f.payload.begin(), f.payload.end(), d.begin() + HeaderBytes);
	return d;
}
std::optional<Frame> Decode(std::span<const uint8_t> d) {
	const auto size = WireSize(d);
	if (!size || *size != d.size()) return {};
	Frame f {ReadHeader(d), {d.begin() + HeaderBytes, d.end()}};
	if (!PayloadValid(f)) return {};
	return f;
}
} // namespace ControllerAudio
