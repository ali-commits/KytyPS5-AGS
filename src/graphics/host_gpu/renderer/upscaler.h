#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_UPSCALER_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_UPSCALER_H_

#include <vector>

// Upscaler (Config::GetUpscaler, --upscaler): replaces a game's own temporal upscale pass,
// described by its upscaler profile (Config::UpscalerProfilePath, common/upscalerProfile.h),
// with another upscaler. RenderExecutor::NoteUpscalerTargets records which depth attachment
// each colour target was rendered with; RenderExecutor::TryUpscalePass (upscaler.cpp) records
// the replacement in place of the pass's draw.
//
// FSR: AMD FidelityFX Super Resolution 2 (3rdparty/fidelityfx-fsr2, built unless KYTY_FSR2=OFF),
// with the same inputs and conventions; its compiled shaders are installed as <emulator>/fsr2.
//
// DLSS: NVIDIA DLSS Super Resolution through NGX, in builds configured with KYTY_NGX_SDK_DIR.
// It is created for the pass's own input and output sizes (it writes the game's output target,
// which every later pass reads), with the profile's jitter, motion-vector and depth conventions,
// auto exposure and the --dlss-mode quality mode. NGX looks for its runtime
// (libnvidia-ngx-dlss.so / nvngx_dlss.dll) in an "ngx" folder next to the emulator, then next to
// the emulator itself.
//
// Anything missing or inconsistent (no profile, a build without the upscaler, no supported GPU or
// runtime, other sizes, no depth, the game not using motion vectors this frame) leaves the game's
// pass to run as before. One "Upscaler 10s:" line every 10 s gives the replaced draws and why
// others were kept.
namespace Libs::Graphics::Upscaler {

// An upscaler built into this executable is selected and a valid profile is loaded (read once).
[[nodiscard]] bool Active();
// Device creation: the instance and device extensions the upscaler needs (none when inactive).
void RequiredExtensions(std::vector<const char*>& instance, std::vector<const char*>& device);

} // namespace Libs::Graphics::Upscaler

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_UPSCALER_H_
