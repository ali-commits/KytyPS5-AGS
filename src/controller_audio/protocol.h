#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>
namespace ControllerAudio {
enum class Kind : uint8_t { Control = 0, Actuator = 1, Speaker = 2 };
enum class Type : uint8_t {
	Hello  = 1,
	Open   = 2,
	Pcm    = 3,
	Close  = 4,
	Reset  = 5,
	Ready  = 6,
	State  = 7,
	Rumble = 8,
	Ping   = 9,
	Pong   = 10,
	Error  = 11,
	Status = 12
};
struct Header {
	Type     type;
	Kind     kind;
	uint8_t  channels;
	uint64_t generation, producer_epoch;
	uint32_t port, rate;
	uint64_t port_generation, sequence, first_frame, source_time_us;
};
struct Frame {
	Header               header;
	std::vector<uint8_t> payload;
};
constexpr size_t     HeaderBytes = 72, PayloadLimit = 16384;
std::vector<uint8_t> Encode(const Frame&);
std::optional<Frame> Decode(std::span<const uint8_t>);
// Checks the fixed header and returns the bounded total wire size before payload allocation.
std::optional<size_t> WireSize(std::span<const uint8_t>);
} // namespace ControllerAudio
