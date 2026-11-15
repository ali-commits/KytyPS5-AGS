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

#if KYTY_NGX
#include <nvsdk_ngx_helpers_vk.h>
#include <nvsdk_ngx_vk.h>
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
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>

namespace Libs::Graphics {

namespace {

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

struct State {
	bool                             active = false;
	Common::UpscalerProfile::Profile profile;
	std::array<DepthPair, 16>        depth_pairs {};
	uint32_t                         depth_next = 0;
	bool                             init_tried = false;
	bool                             ready      = false;
#if KYTY_NGX
	NVSDK_NGX_Parameter* params  = nullptr;
	NVSDK_NGX_Handle*    feature = nullptr;
#endif
	uint32_t                                                   feature_in_w  = 0;
	uint32_t                                                   feature_in_h  = 0;
	uint32_t                                                   feature_out_w = 0;
	uint32_t                                                   feature_out_h = 0;
	uint32_t                                                   last_frame    = 0;
	uint64_t                                                   evaluations   = 0;
	std::array<uint64_t, static_cast<size_t>(Fallback::Count)> fallbacks {};
	int64_t                                                    print_ns = 0;
};

State& Get() {
	static State state = [] {
		State s;
		if (Config::GetUpscaler() == Config::Upscaler::Off) {
			return s;
		}
		const auto& path = Config::UpscalerProfilePath();
		std::string error;
		if (path.empty()) {
			std::printf("Upscaler: no upscaler profile for this game: the game's pass runs\n");
		} else if (!KYTY_NGX) {
			std::printf("Upscaler: this build has no DLSS (configured without KYTY_NGX_SDK_DIR): "
			            "the game's pass runs\n");
		} else if (!Common::UpscalerProfile::Load(path, s.profile, error)) {
			std::printf("Upscaler: %s: %s: the game's pass runs\n", path.string().c_str(),
			            error.c_str());
		} else {
			s.active = true;
			std::printf("Upscaler: DLSS for pass %016llx (%s)\n",
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
	std::printf("Upscaler 10s: DLSS %llu draws (%ux%u -> %ux%u) | game pass kept:%s\n",
	            static_cast<unsigned long long>(s.evaluations), s.feature_in_w, s.feature_in_h,
	            s.feature_out_w, s.feature_out_h, kept.empty() ? " none" : kept.c_str());
	std::fflush(stdout);
	s.evaluations = 0;
	s.fallbacks   = {};
}

bool Fail(State& s, Fallback reason) {
	s.fallbacks[static_cast<size_t>(reason)]++;
	Summary(s);
	return false;
}

#if KYTY_NGX
// The folder holding the emulator executable (empty when unknown).
std::filesystem::path ExecutableDir() {
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

// NGX initialisation, once: whether DLSS Super Resolution is available.
void Init(State& s, GraphicContext& graphics) {
	s.init_tried = true;
	// NGX's runtime search paths; its log and cache folder.
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
		std::fflush(stdout);
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
	std::fflush(stdout);
}
#endif

} // namespace

namespace Upscaler {

bool Active() {
	return Get().active;
}

void RequiredExtensions(std::vector<const char*>& instance, std::vector<const char*>& device) {
#if KYTY_NGX
	if (!Active()) {
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
#if !KYTY_NGX
	(void)buffer;
	(void)output;
	return false;
#else
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
	float jitter[2];
	std::memcpy(&jitter[0], cb + p.jitter_x, sizeof(float));
	std::memcpy(&jitter[1], cb + p.jitter_y, sizeof(float));
	const uint32_t out_w = cb[p.output_width];
	const uint32_t out_h = cb[p.output_height];
	const uint32_t in_w  = cb[p.input_width];
	const uint32_t in_h  = cb[p.input_height];
	if (p.motion_vectors_enabled.has_value() && cb[*p.motion_vectors_enabled] == 0) {
		return Fail(s, Fallback::NoMotionVectors);
	}
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
	auto&      mv    = cache.GetImage(mv_id);
	auto&      color = cache.GetImage(color_id);
	auto&      depth = cache.GetImage(depth_id);
	auto&      out   = cache.GetImage(output);
	const auto sized = [](const Image& image, uint32_t w, uint32_t h) {
		return image.info.extent.width == w && image.info.extent.height == h &&
		       image.backing.image != nullptr && image.info.samples == 1;
	};
	if (!sized(mv, in_w, in_h) || !sized(color, in_w, in_h) || !sized(depth, in_w, in_h) ||
	    !sized(out, out_w, out_h) || !depth.info.IsDepth() ||
	    !(out.backing.usage & vk::ImageUsageFlagBits::eStorage)) {
		return Fail(s, Fallback::Sizes);
	}
	auto& graphics = m_context.GetGraphics();
	if (!s.init_tried) {
		Init(s, graphics);
	}
	if (s.params == nullptr) {
		return Fail(s, Fallback::InitFailed);
	}
	if (!s.ready) {
		return Fail(s, Fallback::Unavailable);
	}

	// NGX records through the native handle, outside rendering.
	buffer.EndRendering();
	const auto cmd = buffer.Handle();
	if (s.feature == nullptr || s.feature_in_w != in_w || s.feature_in_h != in_h ||
	    s.feature_out_w != out_w || s.feature_out_h != out_h) {
		if (s.feature != nullptr) {
			NVSDK_NGX_VULKAN_ReleaseFeature(s.feature);
			s.feature = nullptr;
		}
		NVSDK_NGX_DLSS_Create_Params create {};
		create.Feature.InWidth            = in_w;
		create.Feature.InHeight           = in_h;
		create.Feature.InTargetWidth      = out_w;
		create.Feature.InTargetHeight     = out_h;
		create.Feature.InPerfQualityValue = Quality();
		create.InFeatureCreateFlags =
		    NVSDK_NGX_DLSS_Feature_Flags_MVLowRes | NVSDK_NGX_DLSS_Feature_Flags_MVJittered |
		    NVSDK_NGX_DLSS_Feature_Flags_AutoExposure |
		    (p.hdr ? NVSDK_NGX_DLSS_Feature_Flags_IsHDR : 0) |
		    (p.depth_inverted ? NVSDK_NGX_DLSS_Feature_Flags_DepthInverted : 0);
		const auto result = NGX_VULKAN_CREATE_DLSS_EXT(static_cast<VkCommandBuffer>(cmd), 1, 1,
		                                               &s.feature, s.params, &create);
		buffer.ForgetBoundState();
		if (NVSDK_NGX_FAILED(result) || s.feature == nullptr) {
			std::printf("Upscaler: DLSS creation failed (0x%08x) for %ux%u -> %ux%u: the game's "
			            "pass runs\n",
			            static_cast<unsigned>(result), in_w, in_h, out_w, out_h);
			std::fflush(stdout);
			s.ready   = false;
			s.feature = nullptr;
			return Fail(s, Fallback::CreateFailed);
		}
		s.feature_in_w  = in_w;
		s.feature_in_h  = in_h;
		s.feature_out_w = out_w;
		s.feature_out_h = out_h;
		std::printf("Upscaler: DLSS %ux%u -> %ux%u\n", in_w, in_h, out_w, out_h);
		std::fflush(stdout);
	}

	// Inputs sampled, output storage.
	mv.Transit(vk::ImageLayout::eShaderReadOnlyOptimal, vk::AccessFlagBits2::eShaderSampledRead, {},
	           cmd);
	color.Transit(vk::ImageLayout::eShaderReadOnlyOptimal, vk::AccessFlagBits2::eShaderSampledRead,
	              {}, cmd);
	depth.Transit(vk::ImageLayout::eShaderReadOnlyOptimal, vk::AccessFlagBits2::eShaderSampledRead,
	              {}, cmd);
	out.Transit(vk::ImageLayout::eGeneral,
	            vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eShaderStorageWrite,
	            {}, cmd);

	// Views for this evaluation, destroyed once the command buffer completed.
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
	views[0]            = make_view(color, vk::ImageAspectFlagBits::eColor);
	views[1]            = make_view(mv, vk::ImageAspectFlagBits::eColor);
	views[2]            = make_view(depth, vk::ImageAspectFlagBits::eDepth);
	views[3]            = make_view(out, vk::ImageAspectFlagBits::eColor);
	const auto resource = [](const Image& image, vk::ImageView view, vk::ImageAspectFlags aspect,
	                         bool write) {
		const VkImageSubresourceRange range {static_cast<VkImageAspectFlags>(aspect), 0, 1, 0, 1};
		return NVSDK_NGX_Create_ImageView_Resource_VK(
		    static_cast<VkImageView>(view), static_cast<VkImage>(image.backing.image), range,
		    static_cast<VkFormat>(image.backing.format), image.info.extent.width,
		    image.info.extent.height, write);
	};
	auto color_res = resource(color, views[0], vk::ImageAspectFlagBits::eColor, false);
	auto mv_res    = resource(mv, views[1], vk::ImageAspectFlagBits::eColor, false);
	auto depth_res = resource(depth, views[2], vk::ImageAspectFlagBits::eDepth, false);
	auto out_res   = resource(out, views[3], vk::ImageAspectFlagBits::eColor, true);

	// Jitter in input pixels. DLSS wants motion vectors pointing from the current position to
	// the previous one, in input pixels after scaling.
	const auto                    uv_x      = static_cast<float>(in_w);
	const auto                    uv_y      = static_cast<float>(in_h);
	const bool                    jitter_uv = p.jitter_units == Common::UpscalerProfile::Units::Uv;
	const bool                    motion_uv = p.motion_units == Common::UpscalerProfile::Units::Uv;
	const float                   mv_sign   = p.motion_to_current ? -1.0f : 1.0f;
	NVSDK_NGX_VK_DLSS_Eval_Params eval {};
	eval.Feature.pInColor          = &color_res;
	eval.Feature.pInOutput         = &out_res;
	eval.pInDepth                  = &depth_res;
	eval.pInMotionVectors          = &mv_res;
	eval.InJitterOffsetX           = p.jitter_sign * jitter[0] * (jitter_uv ? uv_x : 1.0f);
	eval.InJitterOffsetY           = p.jitter_sign * jitter[1] * (jitter_uv ? uv_y : 1.0f);
	eval.InMVScaleX                = mv_sign * (motion_uv ? uv_x : 1.0f);
	eval.InMVScaleY                = mv_sign * (motion_uv ? uv_y : 1.0f);
	eval.InRenderSubrectDimensions = {in_w, in_h};
	if (p.frame_index.has_value()) {
		const uint32_t frame = cb[*p.frame_index];
		eval.InReset         = s.last_frame != 0 && frame != s.last_frame + 1 ? 1 : 0;
		s.last_frame         = frame;
	}
	const auto result =
	    NGX_VULKAN_EVALUATE_DLSS_EXT(static_cast<VkCommandBuffer>(cmd), s.feature, s.params, &eval);
	buffer.ForgetBoundState();
	m_context.GetCommandScheduler().DeferOperation([device, views]() {
		for (const auto view: views) {
			if (view) {
				device.destroyImageView(view);
			}
		}
	});
	if (NVSDK_NGX_FAILED(result)) {
		return Fail(s, Fallback::EvaluateFailed);
	}
	// The output now holds DLSS's image: a GPU write of the target, as the draw's would be.
	out.NoteContentWrite();
	cache.CommitGpuWrite(out);
	s.evaluations++;
	Summary(s);
	return true;
#endif
}

} // namespace Libs::Graphics
