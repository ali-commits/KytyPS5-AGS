#pragma once
#include <cstdint>
#include <span>
namespace ControllerAudio {
struct PcmView {
	const void*  data;
	uint32_t     frames, channels, rate;
	bool         is_float;
	const int*   volume;
	float        gain;
	const float* gains;
};
bool ConvertStereo(const PcmView&, std::span<float> output) noexcept;
} // namespace ControllerAudio
