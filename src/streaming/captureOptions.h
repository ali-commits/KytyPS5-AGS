#ifndef KYTY_STREAMING_CAPTURE_OPTIONS_H
#define KYTY_STREAMING_CAPTURE_OPTIONS_H
#include <cstdint>

namespace Streaming {
// Zero retains the legacy default. All explicit managed rates share this contract.
constexpr bool IsCaptureFrameRateSupported(std::uint32_t fps) {
	return fps == 0 || fps == 30 || fps == 60 || fps == 90 || fps == 120;
}
} // namespace Streaming
#endif
