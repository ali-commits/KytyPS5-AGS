#include "streaming/aspectMode.h"

#include <cstdio>
#include <cstdlib>

using Streaming::AspectMode;
using Streaming::BlitRect;
using Streaming::ComputeBlitRect;

namespace {
int g_failures = 0;
void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "AspectModeTests FAIL: %s\n", message);
		g_failures++;
	}
}

int DstW(const BlitRect& r) { return r.dst_x1 - r.dst_x0; }
int DstH(const BlitRect& r) { return r.dst_y1 - r.dst_y0; }
int SrcW(const BlitRect& r) { return r.src_x1 - r.src_x0; }
int SrcH(const BlitRect& r) { return r.src_y1 - r.src_y0; }

// A destination rectangle must stay inside the surface and be centred (<=1px slack from rounding).
void InsideAndCentred(const BlitRect& r, int dstW, int dstH, const char* who) {
	Check(r.dst_x0 >= 0 && r.dst_y0 >= 0 && r.dst_x1 <= dstW && r.dst_y1 <= dstH,
	      who); // within surface
	Check((r.dst_x0 - (dstW - DstW(r) - r.dst_x0)) <= 1 &&
	          (r.dst_x0 - (dstW - DstW(r) - r.dst_x0)) >= -1,
	      who); // horizontally centred
	Check((r.dst_y0 - (dstH - DstH(r) - r.dst_y0)) <= 1 &&
	          (r.dst_y0 - (dstH - DstH(r) - r.dst_y0)) >= -1,
	      who); // vertically centred
}
void SrcInside(const BlitRect& r, int srcW, int srcH, const char* who) {
	Check(r.src_x0 >= 0 && r.src_y0 >= 0 && r.src_x1 <= srcW && r.src_y1 <= srcH, who);
}
} // namespace

int main() {
	// Parse + round-trip of the public names.
	Check(Streaming::ParseAspectMode("fit") == AspectMode::Fit, "parse fit");
	Check(Streaming::ParseAspectMode("fill") == AspectMode::Fill, "parse fill");
	Check(Streaming::ParseAspectMode("stretch") == AspectMode::Stretch, "parse stretch");
	Check(Streaming::ParseAspectMode("") == AspectMode::Fit, "empty defaults to fit");
	Check(Streaming::ParseAspectMode("bogus") == AspectMode::Fit, "unknown defaults to fit");
	Check(Streaming::AspectModeName(AspectMode::Fit) == "fit", "name fit");
	Check(Streaming::AspectModeName(AspectMode::Fill) == "fill", "name fill");
	Check(Streaming::AspectModeName(AspectMode::Stretch) == "stretch", "name stretch");

	// Equal aspect (16:9 -> 16:9): every mode is a full-to-full blit, no bars.
	for (const auto mode : {AspectMode::Fit, AspectMode::Fill, AspectMode::Stretch}) {
		const auto r = ComputeBlitRect(1920, 1080, 2560, 1440, mode);
		Check(!r.clear_background, "equal aspect never clears");
		Check(DstW(r) == 2560 && DstH(r) == 1440, "equal aspect covers full dst");
		Check(SrcW(r) == 1920 && SrcH(r) == 1080, "equal aspect uses whole src");
	}

	// The real target: a 16:9 game frame on a 20:9 (2400x1080) phone panel.
	// Fit: pillarbox (surface is WIDER than content -> bars left/right), correct geometry.
	{
		const auto r = ComputeBlitRect(1920, 1080, 2400, 1080, AspectMode::Fit);
		Check(r.clear_background, "fit on wider surface letter/pillarboxes");
		Check(DstH(r) == 1080, "fit keeps full height here");
		Check(DstW(r) == 1920, "fit width = 1080 * 16/9 = 1920"); // 1080*1920/1080
		InsideAndCentred(r, 2400, 1080, "fit 20:9 centred");
		Check(SrcW(r) == 1920 && SrcH(r) == 1080, "fit shows the whole frame");
	}
	// Fill: crop top/bottom of the frame so it covers the whole 20:9 surface.
	{
		const auto r = ComputeBlitRect(1920, 1080, 2400, 1080, AspectMode::Fill);
		Check(!r.clear_background, "fill never needs a background");
		Check(DstW(r) == 2400 && DstH(r) == 1080, "fill covers the whole surface");
		Check(SrcW(r) == 1920, "fill keeps full src width");
		// cropped height = 1920 * 1080 / 2400 = 864, centred -> y0 = (1080-864)/2 = 108
		Check(SrcH(r) == 864 && r.src_y0 == 108, "fill crops src height symmetrically");
		SrcInside(r, 1920, 1080, "fill src inside");
	}
	// Stretch: distort to fill, whole src, whole dst.
	{
		const auto r = ComputeBlitRect(1920, 1080, 2400, 1080, AspectMode::Stretch);
		Check(!r.clear_background, "stretch never clears");
		Check(DstW(r) == 2400 && DstH(r) == 1080 && SrcW(r) == 1920 && SrcH(r) == 1080,
		      "stretch is full-to-full");
	}

	// Tall surface (portrait phone 1080x2400) from a 16:9 frame.
	// Fit here fits WIDTH -> letterbox top/bottom.
	{
		const auto r = ComputeBlitRect(1920, 1080, 1080, 2400, AspectMode::Fit);
		Check(r.clear_background, "fit tall surface boxes");
		Check(DstW(r) == 1080, "fit keeps full width");
		Check(DstH(r) == 607, "fit height = 1080 * 1080/1920 = 607"); // integer trunc
		InsideAndCentred(r, 1080, 2400, "fit portrait centred");
	}

	// Many sizes: Fit and Fill must always stay in-bounds and never produce an empty rect.
	const int sizes[] = {360, 480, 540, 720, 1080, 1200, 1440, 2160, 2412};
	for (int sw : sizes)
		for (int sh : sizes)
			for (int dw : sizes)
				for (int dh : sizes)
					for (const auto mode : {AspectMode::Fit, AspectMode::Fill}) {
						const auto r = ComputeBlitRect(sw, sh, dw, dh, mode);
						Check(DstW(r) > 0 && DstH(r) > 0 && SrcW(r) > 0 && SrcH(r) > 0,
						      "non-empty blit for every size");
						InsideAndCentred(r, dw, dh, "dst in-bounds for every size");
						SrcInside(r, sw, sh, "src in-bounds for every size");
						if (mode == AspectMode::Fill)
							Check(DstW(r) == dw && DstH(r) == dh, "fill always covers surface");
					}

	// Degenerate inputs never divide by zero and yield an all-zero (skip) rect.
	for (const auto mode : {AspectMode::Fit, AspectMode::Fill, AspectMode::Stretch}) {
		const auto r = ComputeBlitRect(0, 1080, 2400, 1080, mode);
		Check(DstW(r) == 0 && DstH(r) == 0, "zero src width -> skip");
		const auto r2 = ComputeBlitRect(1920, 1080, 0, 0, mode);
		Check(DstW(r2) == 0 && DstH(r2) == 0, "zero dst -> skip");
	}

	if (g_failures == 0) std::puts("AspectModeTests PASS");
	return g_failures == 0 ? 0 : 1;
}
