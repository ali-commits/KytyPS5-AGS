#ifdef NDEBUG
#undef NDEBUG
#endif
#include "controller_audio/pcm.h"
#include "controller_audio/protocol.h"

#include <array>
#include <bit>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <limits>
using namespace ControllerAudio;
int main() {
	const int            volume[] {16384, 32768};
	const int16_t        mono[] {-32768, 16384};
	std::array<float, 4> out {};
	assert(ConvertStereo({mono, 2, 1, 48000, false, volume, 0.5f, nullptr}, out));
	assert((out == std::array<float, 4> {-0.25f, -0.25f, 0.125f, 0.125f}));
	const float stereo[] {0.5f, -0.25f};
	const float gains[] {0.5f, 0.25f};
	assert(ConvertStereo({stereo, 1, 2, 44100, true, volume, 0.5f, gains}, out));
	assert(out[0] == 0.0625f && out[1] == -0.03125f);
	const float nan[] {std::numeric_limits<float>::quiet_NaN()};
	assert(!ConvertStereo({nan, 1, 1, 48000, true, volume, 1, nullptr}, out));
	assert(!ConvertStereo({mono, 2, 1, 0, false, volume, 1, nullptr}, out));
	Header h {
	    Type::Pcm,      Kind::Actuator, 2, UINT64_MAX, UINT64_MAX - 1, 31, 192000, UINT64_MAX - 2,
	    UINT64_MAX - 3, UINT64_MAX - 4, 1};
	Frame frame {h, {0, 0, 0, 0x3f, 0, 0, 0x80, 0xbe}};
	auto  wire = Encode(frame);
	assert(wire.size() == 80 && wire[0] == 'K' && wire[8] == 0 && wire[11] == 72);
	auto decoded = Decode(wire);
	assert(decoded && decoded->payload == frame.payload);
	assert(decoded->header.generation == h.generation &&
	       decoded->header.first_frame == h.first_frame);
	assert(decoded->header.port_generation == h.port_generation &&
	       decoded->header.sequence == h.sequence);
	auto bad = wire;
	bad[5]   = 0;
	assert(!Decode(bad));
	bad    = wire;
	bad[6] = 0;
	assert(!Decode(bad));
	bad     = wire;
	bad[15] = 9;
	assert(!Decode(bad));
	bad        = wire;
	bad.back() = 0x7f;
	bad[78]    = 0xc0;
	assert(!Decode(bad));
	frame.payload.resize(PayloadLimit + 8);
	assert(Encode(frame).empty());
	frame.payload.resize(8 * 1921);
	assert(Encode(frame).empty());
	Frame hello {{Type::Hello, Kind::Control, 0, 1, 2, 0, 0, 0, 0, 0, 0},
	             std::vector<uint8_t>(32, 7)};
	assert(Decode(Encode(hello)));
	hello.header.port = 1;
	assert(Encode(hello).empty());
	Frame rumble {{Type::Rumble, Kind::Control, 0, 1, 2, 0, 0, 0, 1, 0, 0},
	              {0, 1, 0, 2, 0, 0, 3, 0xe8}};
	assert(Decode(Encode(rumble)));
	rumble.payload.back() = 0xe9;
	assert(Encode(rumble).empty());
	std::puts("ControllerAudioTests: all assertions passed");
}
