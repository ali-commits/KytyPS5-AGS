#include "graphics/host_gpu/renderer/upscaler.h"

#include "common/emulatorConfig.h"
#include "common/upscalerProfile.h"
#include "graphics/host_gpu/renderer/cache/textureCache.h"
#include "graphics/host_gpu/renderer/colorRenderTarget.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/host_gpu/renderer/image/image.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/vulkanCommon.h"

#ifndef KYTY_NGX
#define KYTY_NGX 0
#endif
#ifndef KYTY_FSR2
#define KYTY_FSR2 0
#endif

#if KYTY_NGX
#include <nvsdk_ngx_helpers_vk.h>
#include <nvsdk_ngx_vk.h>
#endif
#if KYTY_FSR2
#include <ffx_fsr2.h>
#include <ffx_fsr2_vk.h>
#include <kytyFsr2.h>
#endif

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <algorithm>
#include <array>
#include <cfloat>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

namespace Libs::Graphics {

namespace {

using Config::Upscaler;

enum class Fallback {
	InitFailed,
	Unavailable,
	Bindings,
	Sizes,
	NoDepth,
	NoMotionVectors,
	CreateFailed,
	EvaluateFailed,
	Count
};
constexpr std::array<const char*, static_cast<size_t>(Fallback::Count)> FALLBACK_NAMES {
    "init", "unavailable", "bindings", "sizes", "no-depth", "no-mv", "create", "evaluate"};

struct DepthPair {
	ImageId color;
	ImageId depth;
};

#if KYTY_FSR2
// One FSR2 context, for one input and output size.
struct FsrContext {
	std::vector<uint8_t> scratch;
	FfxFsr2Context       context {};
};
#endif

struct State {
	Upscaler                         upscaler = Upscaler::Off; // Off: inactive
	Common::UpscalerProfile::Profile profile;
	std::array<DepthPair, 16>        depth_pairs {};
	uint32_t                         depth_next = 0;
	bool                             init_tried = false;
	bool                             ready      = false;
#if KYTY_NGX
	NVSDK_NGX_Parameter* params  = nullptr;
	NVSDK_NGX_Handle*    feature = nullptr;
#endif
#if KYTY_FSR2
	std::unique_ptr<FsrContext> fsr;
	int64_t                     fsr_frame_ns = 0;
#endif
	bool                                                       created     = false;
	uint32_t                                                   in_w        = 0;
	uint32_t                                                   in_h        = 0;
	uint32_t                                                   out_w       = 0;
	uint32_t                                                   out_h       = 0;
	uint32_t                                                   last_frame  = 0;
	uint64_t                                                   evaluations = 0;
	std::array<uint64_t, static_cast<size_t>(Fallback::Count)> fallbacks {};
	int64_t                                                    print_ns = 0;
};

const char* Name(Upscaler upscaler) {
	return upscaler == Upscaler::DLSS ? "DLSS" : "FSR";
}

bool BuiltIn(Upscaler upscaler) {
	return upscaler == Upscaler::DLSS ? KYTY_NGX != 0 : KYTY_FSR2 != 0;
}

State& Get() {
	static State state = [] {
		State      s;
		const auto upscaler = Config::GetUpscaler();
		if (upscaler == Upscaler::Off) {
			return s;
		}
		const auto& path = Config::UpscalerProfilePath();
		std::string error;
		if (path.empty()) {
			std::printf("Upscaler: no upscaler profile for this game: the game's pass runs\n");
		} else if (!BuiltIn(upscaler)) {
			std::printf("Upscaler: this build has no %s%s: the game's pass runs\n", Name(upscaler),
			            upscaler == Upscaler::DLSS ? " (configured without KYTY_NGX_SDK_DIR)"
			                                       : " (configured with KYTY_FSR2=OFF)");
		} else if (!Common::UpscalerProfile::Load(path, s.profile, error)) {
			std::printf("Upscaler: %s: %s: the game's pass runs\n", path.string().c_str(),
			            error.c_str());
		} else {
			s.upscaler = upscaler;
			std::printf("Upscaler: %s for pass %016llx (%s)\n", Name(upscaler),
			            static_cast<unsigned long long>(s.profile.pixel_shader),
			            path.string().c_str());
		}
		std::fflush(stdout);
		return s;
	}();
	return state;
}

int64_t NowNs() {
	return std::chrono::duration_cast<std::chrono::nanoseconds>(
	           std::chrono::steady_clock::now().time_since_epoch())
	    .count();
}

void Summary(State& s) {
	const auto now = NowNs();
	if (s.print_ns == 0) {
		s.print_ns = now;
		return;
	}
	if (now - s.print_ns < 10'000'000'000LL) {
		return;
	}
	s.print_ns = now;
	std::string kept;
	for (size_t i = 0; i < s.fallbacks.size(); i++) {
		if (s.fallbacks[i] != 0) {
			kept += " " + std::string(FALLBACK_NAMES[i]) + "=" + std::to_string(s.fallbacks[i]);
		}
	}
	std::printf("Upscaler 10s: %s %llu draws (%ux%u -> %ux%u) | game pass kept:%s\n",
	            Name(s.upscaler), static_cast<unsigned long long>(s.evaluations), s.in_w, s.in_h,
	            s.out_w, s.out_h, kept.empty() ? " none" : kept.c_str());
	std::fflush(stdout);
	s.evaluations = 0;
	s.fallbacks   = {};
}

bool Fail(State& s, Fallback reason) {
	s.fallbacks[static_cast<size_t>(reason)]++;
	Summary(s);
	return false;
}

// The folder holding the emulator executable (empty when unknown).
[[maybe_unused]] std::filesystem::path ExecutableDir() {
#ifdef _WIN32
	std::wstring name(32768, L'\0');
	const auto   size = GetModuleFileNameW(nullptr, name.data(), static_cast<DWORD>(name.size()));
	if (size == 0 || size >= name.size()) {
		return {};
	}
	name.resize(size);
	return std::filesystem::path(name).parent_path();
#else
	std::error_code ec;
	const auto      exe = std::filesystem::read_symlink("/proc/self/exe", ec);
	return ec ? std::filesystem::path {} : exe.parent_path();
#endif
}

// What one evaluation of either upscaler reads and writes (upscalerProfile.h).
struct Frame {
	Image*   color          = nullptr;
	Image*   motion_vectors = nullptr;
	Image*   depth          = nullptr;
	Image*   output         = nullptr;
	uint32_t in_w           = 0;
	uint32_t in_h           = 0;
	uint32_t out_w          = 0;
	uint32_t out_h          = 0;
	float    jitter_x       = 0.0f; // input pixels
	float    jitter_y       = 0.0f;
	float mv_scale_x = 0.0f; // to input pixels, pointing from the current to the previous position
	float mv_scale_y = 0.0f;
	bool  reset      = false;
};

#if KYTY_NGX
NVSDK_NGX_PerfQuality_Value Quality() {
	switch (Config::GetDlssMode()) {
		case Config::DlssMode::Balanced: return NVSDK_NGX_PerfQuality_Value_Balanced;
		case Config::DlssMode::Performance: return NVSDK_NGX_PerfQuality_Value_MaxPerf;
		case Config::DlssMode::UltraPerformance:
			return NVSDK_NGX_PerfQuality_Value_UltraPerformance;
		case Config::DlssMode::DLAA: return NVSDK_NGX_PerfQuality_Value_DLAA;
		case Config::DlssMode::Quality:
		default: return NVSDK_NGX_PerfQuality_Value_MaxQuality;
	}
}

// NGX initialisation, once: whether DLSS Super Resolution is available. NGX looks for its
// runtime in <emulator>/ngx, then next to the emulator.
void InitDlss(State& s, GraphicContext& graphics) {
	const auto         dir     = ExecutableDir();
	const std::wstring ngx_dir = (dir / "ngx").wstring();
	const std::wstring exe_dir = dir.wstring();
	const wchar_t*     paths[] = {ngx_dir.c_str(), exe_dir.c_str()};
	std::error_code    ec;
	auto               data_path = std::filesystem::temp_directory_path(ec) / "kyty-ngx";
	std::filesystem::create_directories(data_path, ec);
	NVSDK_NGX_FeatureCommonInfo info {};
	info.PathListInfo.Path   = paths;
	info.PathListInfo.Length = dir.empty() ? 0 : 2;
	// The project id identifies KytyPS5 to NGX (a custom engine has no NVIDIA application id).
	const auto result = NVSDK_NGX_VULKAN_Init_with_ProjectID(
	    "5f1e6b0c-2b7d-4a52-9d3e-7c1a4e2f9b61", NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.0",
	    data_path.wstring().c_str(), static_cast<VkInstance>(graphics.instance),
	    static_cast<VkPhysicalDevice>(graphics.physical_device),
	    static_cast<VkDevice>(graphics.device), VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr,
	    VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr, &info);
	if (NVSDK_NGX_FAILED(result) ||
	    NVSDK_NGX_FAILED(NVSDK_NGX_VULKAN_GetCapabilityParameters(&s.params))) {
		s.params = nullptr;
		std::printf("Upscaler: NGX initialisation failed (0x%08x): the game's pass runs\n",
		            static_cast<unsigned>(result));
		return;
	}
	int available    = 0;
	int needs_driver = 0;
	NVSDK_NGX_Parameter_GetI(s.params, NVSDK_NGX_Parameter_SuperSampling_Available, &available);
	NVSDK_NGX_Parameter_GetI(s.params, NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver,
	                         &needs_driver);
	s.ready = available != 0;
	std::printf("Upscaler: DLSS Super Resolution %s%s\n", s.ready ? "available" : "unavailable",
	            needs_driver != 0 ? " (the driver needs an update)" : "");
}

bool CreateDlss(State& s, vk::CommandBuffer cmd) {
	if (s.feature != nullptr) {
		NVSDK_NGX_VULKAN_ReleaseFeature(s.feature);
		s.feature = nullptr;
	}
	const auto&                  p = s.profile;
	NVSDK_NGX_DLSS_Create_Params create {};
	create.Feature.InWidth            = s.in_w;
	create.Feature.InHeight           = s.in_h;
	create.Feature.InTargetWidth      = s.out_w;
	create.Feature.InTargetHeight     = s.out_h;
	create.Feature.InPerfQualityValue = Quality();
	create.InFeatureCreateFlags =
	    NVSDK_NGX_DLSS_Feature_Flags_MVLowRes | NVSDK_NGX_DLSS_Feature_Flags_MVJittered |
	    NVSDK_NGX_DLSS_Feature_Flags_AutoExposure |
	    (p.hdr ? NVSDK_NGX_DLSS_Feature_Flags_IsHDR : 0) |
	    (p.depth_inverted ? NVSDK_NGX_DLSS_Feature_Flags_DepthInverted : 0);
	const auto result = NGX_VULKAN_CREATE_DLSS_EXT(static_cast<VkCommandBuffer>(cmd), 1, 1,
	                                               &s.feature, s.params, &create);
	if (NVSDK_NGX_FAILED(result) || s.feature == nullptr) {
		std::printf("Upscaler: DLSS creation failed (0x%08x)\n", static_cast<unsigned>(result));
		s.feature = nullptr;
		return false;
	}
	return true;
}

bool EvaluateDlss(State& s, vk::CommandBuffer cmd, const Frame& f,
                  const std::array<vk::ImageView, 4>& views) {
	const auto resource = [](const Image& image, vk::ImageView view, vk::ImageAspectFlags aspect,
	                         bool write) {
		const VkImageSubresourceRange range {static_cast<VkImageAspectFlags>(aspect), 0, 1, 0, 1};
		return NVSDK_NGX_Create_ImageView_Resource_VK(
		    static_cast<VkImageView>(view), static_cast<VkImage>(image.backing.image), range,
		    static_cast<VkFormat>(image.backing.format), image.info.extent.width,
		    image.info.extent.height, write);
	};
	auto color_res = resource(*f.color, views[0], vk::ImageAspectFlagBits::eColor, false);
	auto mv_res    = resource(*f.motion_vectors, views[1], vk::ImageAspectFlagBits::eColor, false);
	auto depth_res = resource(*f.depth, views[2], vk::ImageAspectFlagBits::eDepth, false);
	auto out_res   = resource(*f.output, views[3], vk::ImageAspectFlagBits::eColor, true);
	NVSDK_NGX_VK_DLSS_Eval_Params eval {};
	eval.Feature.pInColor          = &color_res;
	eval.Feature.pInOutput         = &out_res;
	eval.pInDepth                  = &depth_res;
	eval.pInMotionVectors          = &mv_res;
	eval.InJitterOffsetX           = f.jitter_x;
	eval.InJitterOffsetY           = f.jitter_y;
	eval.InMVScaleX                = f.mv_scale_x;
	eval.InMVScaleY                = f.mv_scale_y;
	eval.InRenderSubrectDimensions = {f.in_w, f.in_h};
	eval.InReset                   = f.reset ? 1 : 0;
	return !NVSDK_NGX_FAILED(NGX_VULKAN_EVALUATE_DLSS_EXT(static_cast<VkCommandBuffer>(cmd),
	                                                      s.feature, s.params, &eval));
}
#endif

#if KYTY_FSR2
// FSR2 initialisation, once: its Vulkan functions and shaders (installed as <emulator>/fsr2).
void InitFsr(State& s, GraphicContext& graphics) {
	KytyFsr2::LoadVulkanFunctions(VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr,
	                              static_cast<VkInstance>(graphics.instance),
	                              VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr);
	std::string missing;
	if (!KytyFsr2::SetShaderDirectory(ExecutableDir() / "fsr2", missing)) {
		std::printf("Upscaler: FSR shader %s is missing: the game's pass runs\n", missing.c_str());
		return;
	}
	s.ready = true;
	std::printf("Upscaler: FSR 2 available\n");
}

// A context for the current sizes. One of other sizes is destroyed once the command buffer being
// recorded completed, after every earlier one that used it.
bool CreateFsr(State& s, GraphicContext& graphics, CommandScheduler& scheduler) {
	if (s.fsr != nullptr) {
		auto* old = s.fsr.release();
		scheduler.DeferOperation([old]() {
			ffxFsr2ContextDestroy(&old->context);
			delete old;
		});
	}
	const auto& p        = s.profile;
	const auto  physical = static_cast<VkPhysicalDevice>(graphics.physical_device);
	auto        fsr      = std::make_unique<FsrContext>();
	fsr->scratch.resize(ffxFsr2GetScratchMemorySizeVK(physical));
	FfxFsr2ContextDescription description {};
	if (ffxFsr2GetInterfaceVK(&description.callbacks, fsr->scratch.data(), fsr->scratch.size(),
	                          physical,
	                          VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr) != FFX_OK) {
		return false;
	}
	description.device        = ffxGetDeviceVK(static_cast<VkDevice>(graphics.device));
	description.maxRenderSize = {s.in_w, s.in_h};
	description.displaySize   = {s.out_w, s.out_h};
	// The profile's conventions; the depth's far plane is unknown, so it is taken as infinite.
	description.flags = FFX_FSR2_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION |
	                    FFX_FSR2_ENABLE_AUTO_EXPOSURE | FFX_FSR2_ENABLE_DEPTH_INFINITE |
	                    (p.hdr ? FFX_FSR2_ENABLE_HIGH_DYNAMIC_RANGE : 0) |
	                    (p.depth_inverted ? FFX_FSR2_ENABLE_DEPTH_INVERTED : 0);
	if (ffxFsr2ContextCreate(&fsr->context, &description) != FFX_OK) {
		return false;
	}
	s.fsr          = std::move(fsr);
	s.fsr_frame_ns = 0;
	return true;
}

bool EvaluateFsr(State& s, vk::CommandBuffer cmd, const Frame& f,
                 const std::array<vk::ImageView, 4>& views) {
	auto*      context  = &s.fsr->context;
	const auto resource = [&](const Image& image, vk::ImageView view, const wchar_t* name,
	                          FfxResourceStates state) {
		return ffxGetTextureResourceVK(context, static_cast<VkImage>(image.backing.image),
		                               static_cast<VkImageView>(view), image.info.extent.width,
		                               image.info.extent.height,
		                               static_cast<VkFormat>(image.backing.format), name, state);
	};
	const auto none = [&](const wchar_t* name) {
		return ffxGetTextureResourceVK(context, VK_NULL_HANDLE, VK_NULL_HANDLE, 1, 1,
		                               VK_FORMAT_UNDEFINED, name);
	};
	const auto now = NowNs();
	const auto delta =
	    s.fsr_frame_ns != 0 ? static_cast<float>(now - s.fsr_frame_ns) / 1e6f : 1000.0f / 30.0f;
	s.fsr_frame_ns = now;

	FfxFsr2DispatchDescription d {};
	d.commandList = ffxGetCommandListVK(static_cast<VkCommandBuffer>(cmd));
	d.color = resource(*f.color, views[0], L"FSR2_InputColor", FFX_RESOURCE_STATE_COMPUTE_READ);
	d.motionVectors = resource(*f.motion_vectors, views[1], L"FSR2_InputMotionVectors",
	                           FFX_RESOURCE_STATE_COMPUTE_READ);
	d.depth    = resource(*f.depth, views[2], L"FSR2_InputDepth", FFX_RESOURCE_STATE_COMPUTE_READ);
	d.output   = resource(*f.output, views[3], L"FSR2_Output", FFX_RESOURCE_STATE_UNORDERED_ACCESS);
	d.exposure = none(L"FSR2_EmptyExposure");
	d.reactive = none(L"FSR2_EmptyReactive");
	d.transparencyAndComposition = none(L"FSR2_EmptyTransparencyAndComposition");
	d.jitterOffset               = {f.jitter_x, f.jitter_y};
	d.motionVectorScale          = {f.mv_scale_x, f.mv_scale_y};
	d.renderSize                 = {f.in_w, f.in_h};
	d.enableSharpening           = false;
	d.frameTimeDelta             = delta;
	d.preExposure                = 1.0f;
	d.reset                      = f.reset;
	// Only the depth clip uses the camera; the game's is unknown, so these are typical values.
	d.cameraNear              = 0.1f;
	d.cameraFar               = FLT_MAX;
	d.cameraFovAngleVertical  = 1.0f;
	d.viewSpaceToMetersFactor = 1.0f;
	return ffxFsr2ContextDispatch(context, &d) == FFX_OK;
}
#endif

} // namespace

namespace Upscaler {

bool Active() {
	return Get().upscaler != Config::Upscaler::Off;
}

void RequiredExtensions(std::vector<const char*>& instance, std::vector<const char*>& device) {
#if KYTY_NGX
	if (Get().upscaler != Config::Upscaler::DLSS) {
		return;
	}
	unsigned int instance_count = 0;
	unsigned int device_count   = 0;
	const char** instance_names = nullptr;
	const char** device_names   = nullptr;
	if (NVSDK_NGX_VULKAN_RequiredExtensions(&instance_count, &instance_names, &device_count,
	                                        &device_names) != NVSDK_NGX_Result_Success) {
		std::printf("Upscaler: NGX did not report its Vulkan extensions\n");
		return;
	}
	instance.insert(instance.end(), instance_names, instance_names + instance_count);
	device.insert(device.end(), device_names, device_names + device_count);
#else
	(void)instance;
	(void)device;
#endif
}

} // namespace Upscaler

void RenderExecutor::NoteUpscalerTargets(std::span<const RenderColorInfo> colors, ImageId depth) {
	if (!depth) {
		return;
	}
	auto& s = Get();
	for (const auto& color: colors) {
		if (!color.image_id) {
			continue;
		}
		bool found = false;
		for (auto& pair: s.depth_pairs) {
			if (pair.color == color.image_id) {
				pair.depth = depth;
				found      = true;
				break;
			}
		}
		if (!found) {
			s.depth_pairs[s.depth_next] = {color.image_id, depth};
			s.depth_next                = (s.depth_next + 1) % s.depth_pairs.size();
		}
	}
}

bool RenderExecutor::TryUpscalePass(CommandBuffer& buffer, PreparedBindings& pixel,
                                    ImageId output) {
	auto& s = Get();
	if (pixel.runtime == nullptr || !*pixel.runtime ||
	    pixel.runtime->program->shader_hash != s.profile.pixel_shader) {
		return false;
	}
	const auto& p = s.profile;
	// The pass's colour and motion vectors, and the constants (upscalerProfile.h).
	if (pixel.images.size() <= std::max(p.color_image, p.motion_vector_image) || !output ||
	    pixel.buffer_sources.size() <= p.constant_buffer ||
	    pixel.buffer_sources[p.constant_buffer].address == 0 ||
	    pixel.buffer_sources[p.constant_buffer].size < p.ConstantBytes()) {
		return Fail(s, Fallback::Bindings);
	}
	const auto* cb =
	    reinterpret_cast<const uint32_t*>(pixel.buffer_sources[p.constant_buffer].address);
	if (p.motion_vectors_enabled.has_value() && cb[*p.motion_vectors_enabled] == 0) {
		return Fail(s, Fallback::NoMotionVectors);
	}
	Frame f;
	f.in_w              = cb[p.input_width];
	f.in_h              = cb[p.input_height];
	f.out_w             = cb[p.output_width];
	f.out_h             = cb[p.output_height];
	auto&      cache    = m_context.GetTextureCache();
	const auto mv_id    = pixel.images[p.motion_vector_image].image_id;
	const auto color_id = pixel.images[p.color_image].image_id;
	ImageId    depth_id {};
	for (const auto& pair: s.depth_pairs) {
		if (pair.color == mv_id) {
			depth_id = pair.depth;
		}
	}
	if (!depth_id || cache.TryGetImage(depth_id) == nullptr) {
		return Fail(s, Fallback::NoDepth);
	}
	f.motion_vectors = &cache.GetImage(mv_id);
	f.color          = &cache.GetImage(color_id);
	f.depth          = &cache.GetImage(depth_id);
	f.output         = &cache.GetImage(output);
	const auto sized = [](const Image& image, uint32_t w, uint32_t h) {
		return image.info.extent.width == w && image.info.extent.height == h &&
		       image.backing.image != nullptr && image.info.samples == 1;
	};
	if (!sized(*f.motion_vectors, f.in_w, f.in_h) || !sized(*f.color, f.in_w, f.in_h) ||
	    !sized(*f.depth, f.in_w, f.in_h) || !sized(*f.output, f.out_w, f.out_h) ||
	    !f.depth->info.IsDepth() || !(f.output->backing.usage & vk::ImageUsageFlagBits::eStorage)) {
		return Fail(s, Fallback::Sizes);
	}
	// Jitter in input pixels; motion vectors scaled to input pixels, pointing from the current
	// position to the previous one (the convention of both upscalers).
	float jitter[2];
	std::memcpy(&jitter[0], cb + p.jitter_x, sizeof(float));
	std::memcpy(&jitter[1], cb + p.jitter_y, sizeof(float));
	const bool  jitter_uv = p.jitter_units == Common::UpscalerProfile::Units::Uv;
	const bool  motion_uv = p.motion_units == Common::UpscalerProfile::Units::Uv;
	const float mv_sign   = p.motion_to_current ? -1.0f : 1.0f;
	f.jitter_x   = p.jitter_sign * jitter[0] * (jitter_uv ? static_cast<float>(f.in_w) : 1.0f);
	f.jitter_y   = p.jitter_sign * jitter[1] * (jitter_uv ? static_cast<float>(f.in_h) : 1.0f);
	f.mv_scale_x = mv_sign * (motion_uv ? static_cast<float>(f.in_w) : 1.0f);
	f.mv_scale_y = mv_sign * (motion_uv ? static_cast<float>(f.in_h) : 1.0f);
	if (p.frame_index.has_value()) {
		const uint32_t frame = cb[*p.frame_index];
		f.reset              = s.last_frame != 0 && frame != s.last_frame + 1;
		s.last_frame         = frame;
	}

	auto& graphics = m_context.GetGraphics();
	if (!s.init_tried) {
		s.init_tried = true;
#if KYTY_NGX
		if (s.upscaler == Config::Upscaler::DLSS) {
			InitDlss(s, graphics);
		}
#endif
#if KYTY_FSR2
		if (s.upscaler == Config::Upscaler::FSR) {
			InitFsr(s, graphics);
		}
#endif
		std::fflush(stdout);
	}
	if (!s.ready) {
		return Fail(s, Fallback::Unavailable);
	}

	// Both upscalers record through the native handle, outside rendering, and bind their own
	// pipelines and descriptors.
	buffer.EndRendering();
	const auto cmd = buffer.Handle();
	if (!s.created || s.in_w != f.in_w || s.in_h != f.in_h || s.out_w != f.out_w ||
	    s.out_h != f.out_h) {
		s.in_w    = f.in_w;
		s.in_h    = f.in_h;
		s.out_w   = f.out_w;
		s.out_h   = f.out_h;
		s.created = false;
#if KYTY_NGX
		if (s.upscaler == Config::Upscaler::DLSS) {
			s.created = CreateDlss(s, cmd);
			buffer.ForgetBoundState();
		}
#endif
#if KYTY_FSR2
		if (s.upscaler == Config::Upscaler::FSR) {
			s.created = CreateFsr(s, graphics, m_context.GetCommandScheduler());
		}
#endif
		if (!s.created) {
			s.ready = false;
			std::printf("Upscaler: %s %ux%u -> %ux%u could not be created: the game's pass runs\n",
			            Name(s.upscaler), f.in_w, f.in_h, f.out_w, f.out_h);
			std::fflush(stdout);
			return Fail(s, Fallback::CreateFailed);
		}
		std::printf("Upscaler: %s %ux%u -> %ux%u\n", Name(s.upscaler), f.in_w, f.in_h, f.out_w,
		            f.out_h);
		std::fflush(stdout);
	}

	// Inputs sampled, output storage; the upscalers keep these layouts.
	f.motion_vectors->Transit(vk::ImageLayout::eShaderReadOnlyOptimal,
	                          vk::AccessFlagBits2::eShaderSampledRead, {}, cmd);
	f.color->Transit(vk::ImageLayout::eShaderReadOnlyOptimal,
	                 vk::AccessFlagBits2::eShaderSampledRead, {}, cmd);
	f.depth->Transit(vk::ImageLayout::eShaderReadOnlyOptimal,
	                 vk::AccessFlagBits2::eShaderSampledRead, {}, cmd);
	f.output->Transit(vk::ImageLayout::eGeneral,
	                  vk::AccessFlagBits2::eShaderStorageRead |
	                      vk::AccessFlagBits2::eShaderStorageWrite,
	                  {}, cmd);

	// Views for this evaluation (colour, motion vectors, depth, output), destroyed once the
	// command buffer completed.
	const auto                   device = graphics.device;
	std::array<vk::ImageView, 4> views {};
	const auto                   make_view = [&](const Image& image, vk::ImageAspectFlags aspect) {
		vk::ImageViewCreateInfo view_info {};
		view_info.image            = image.backing.image;
		view_info.viewType         = vk::ImageViewType::e2D;
		view_info.format           = image.backing.format;
		view_info.subresourceRange = vk::ImageSubresourceRange {aspect, 0, 1, 0, 1};
		vk::ImageView view;
		return device.createImageView(&view_info, nullptr, &view) == vk::Result::eSuccess
		           ? view
				   : vk::ImageView {};
	};
	views[0] = make_view(*f.color, vk::ImageAspectFlagBits::eColor);
	views[1] = make_view(*f.motion_vectors, vk::ImageAspectFlagBits::eColor);
	views[2] = make_view(*f.depth, vk::ImageAspectFlagBits::eDepth);
	views[3] = make_view(*f.output, vk::ImageAspectFlagBits::eColor);
	m_context.GetCommandScheduler().DeferOperation([device, views]() {
		for (const auto view: views) {
			if (view) {
				device.destroyImageView(view);
			}
		}
	});
	if (std::any_of(views.begin(), views.end(), [](vk::ImageView v) { return !v; })) {
		return Fail(s, Fallback::EvaluateFailed);
	}

	bool evaluated = false;
#if KYTY_NGX
	if (s.upscaler == Config::Upscaler::DLSS) {
		evaluated = EvaluateDlss(s, cmd, f, views);
	}
#endif
#if KYTY_FSR2
	if (s.upscaler == Config::Upscaler::FSR) {
		evaluated = EvaluateFsr(s, cmd, f, views);
	}
#endif
	buffer.ForgetBoundState();
	if (!evaluated) {
		return Fail(s, Fallback::EvaluateFailed);
	}
	// The output now holds the upscaler's image: a GPU write of the target, as the draw's would be.
	f.output->NoteContentWrite();
	cache.CommitGpuWrite(*f.output);
	s.evaluations++;
	Summary(s);
	return true;
}

} // namespace Libs::Graphics
