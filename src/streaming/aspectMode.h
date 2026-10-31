#ifndef KYTY_STREAMING_ASPECT_MODE_H
#define KYTY_STREAMING_ASPECT_MODE_H
#include <cstdint>
#include <string_view>

// Pure geometry for presenting a game frame (src) into a stream/window surface (dst) whose
// aspect ratio may differ (e.g. a 16:9 render shown on a 20:9 phone). No Vulkan here so the
// rectangle math is unit-testable on its own.
namespace Streaming {

enum class AspectMode : uint8_t {
	Fit,     // letter/pillarbox: whole frame visible, correct geometry, black bars
	Fill,    // crop the frame to cover the whole surface, correct geometry, no bars
	Stretch, // distort the frame to fill the surface exactly (legacy behaviour)
};

// Inclusive-min, exclusive-max rectangle in integer pixels.
struct BlitRect {
	int32_t src_x0 = 0, src_y0 = 0, src_x1 = 0, src_y1 = 0;
	int32_t dst_x0 = 0, dst_y0 = 0, dst_x1 = 0, dst_y1 = 0;
	// True when the destination does not cover the whole surface, so the caller must clear it
	// to black first (letter/pillarboxing).
	bool clear_background = false;
};

constexpr AspectMode ParseAspectMode(std::string_view text) {
	if (text == "fill") return AspectMode::Fill;
	if (text == "stretch") return AspectMode::Stretch;
	return AspectMode::Fit; // default and unknown -> correct geometry with bars
}

constexpr std::string_view AspectModeName(AspectMode mode) {
	switch (mode) {
		case AspectMode::Fill: return "fill";
		case AspectMode::Stretch: return "stretch";
		case AspectMode::Fit: break;
	}
	return "fit";
}

// Map src (srcW x srcH) onto dst (dstW x dstH). Degenerate inputs clamp to a full stretch so the
// caller never divides by zero or emits an empty blit. Rounding keeps the scaled extent within
// the surface (so a one-pixel overspill never reads past the source or writes past the dst).
constexpr BlitRect ComputeBlitRect(int32_t srcW, int32_t srcH, int32_t dstW, int32_t dstH,
                                   AspectMode mode) {
	BlitRect r;
	if (srcW <= 0 || srcH <= 0 || dstW <= 0 || dstH <= 0) {
		return r; // all-zero: caller treats as "nothing to blit"
	}
	r.src_x0 = 0;
	r.src_y0 = 0;
	r.src_x1 = srcW;
	r.src_y1 = srcH;
	r.dst_x0 = 0;
	r.dst_y0 = 0;
	r.dst_x1 = dstW;
	r.dst_y1 = dstH;

	if (mode == AspectMode::Stretch) {
		return r; // full source -> full dst, aspect ignored
	}

	// Compare aspect ratios with integer cross-products to avoid float rounding:
	// src is wider than dst  <=>  srcW*dstH > dstW*srcH
	const int64_t src_cross = int64_t(srcW) * dstH;
	const int64_t dst_cross = int64_t(dstW) * srcH;

	if (mode == AspectMode::Fit) {
		r.clear_background = (src_cross != dst_cross);
		if (src_cross > dst_cross) {
			// Source wider than dst: fit width, bars top/bottom (letterbox).
			const int32_t h = int32_t((int64_t(dstW) * srcH) / srcW);
			const int32_t y = (dstH - h) / 2;
			r.dst_y0        = y;
			r.dst_y1        = y + h;
		} else if (src_cross < dst_cross) {
			// Source taller than dst: fit height, bars left/right (pillarbox).
			const int32_t w = int32_t((int64_t(dstH) * srcW) / srcH);
			const int32_t x = (dstW - w) / 2;
			r.dst_x0        = x;
			r.dst_x1        = x + w;
		}
		return r;
	}

	// Fill: scale to cover, crop the overflowing src axis (centred).
	if (src_cross > dst_cross) {
		// Source wider than dst: full height, crop src width.
		const int32_t w = int32_t((int64_t(dstW) * srcH) / dstH);
		const int32_t x = (srcW - w) / 2;
		r.src_x0        = x;
		r.src_x1        = x + w;
	} else if (src_cross < dst_cross) {
		// Source taller than dst: full width, crop src height.
		const int32_t h = int32_t((int64_t(dstH) * srcW) / dstW);
		const int32_t y = (srcH - h) / 2;
		r.src_y0        = y;
		r.src_y1        = y + h;
	}
	return r;
}

} // namespace Streaming
#endif
