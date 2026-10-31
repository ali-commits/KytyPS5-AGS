#include "controller_audio/pcm.h"

#include <cmath>
#include <cstring>
#include <limits>
namespace ControllerAudio {
bool ConvertStereo(const PcmView& view, std::span<float> output) noexcept {
	if (!view.data || !view.volume || !view.frames || !view.channels || view.channels > 12 ||
	    view.rate < 1000 || view.rate > 192000 || !std::isfinite(view.gain) ||
	    view.frames > output.size() / 2)
		return false;
	float scale[2] {};
	for (unsigned ch = 0; ch < 2; ch++) {
		const auto source = view.channels == 1 ? 0 : ch;
		scale[ch] =
		    (view.volume[source] / 32768.0f) * view.gain * (view.gains ? view.gains[source] : 1.0f);
		if (!std::isfinite(scale[ch])) return false;
	}
	for (uint32_t frame = 0; frame < view.frames; frame++)
		for (unsigned ch = 0; ch < 2; ch++) {
			const size_t index = size_t(frame) * view.channels + (view.channels == 1 ? 0 : ch);
			float        value;
			if (view.is_float)
				std::memcpy(&value, static_cast<const uint8_t*>(view.data) + index * 4, 4);
			else {
				int16_t sample;
				std::memcpy(&sample, static_cast<const uint8_t*>(view.data) + index * 2, 2);
				value = sample / 32768.0f;
			}
			value *= scale[ch];
			if (!std::isfinite(value)) return false;
			output[size_t(frame) * 2 + ch] = value;
		}
	return true;
}
} // namespace ControllerAudio
