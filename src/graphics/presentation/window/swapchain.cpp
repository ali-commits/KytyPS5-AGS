#include "common/assert.h"
#include "common/common.h"
#include "common/hangWatchdog.h"
#include "common/debugCounters.h"
#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "common/threads.h"
#include "gpu_blit_shaders/gpu_blit_fs_triangle_spv.h"
#include "gpu_blit_shaders/gpu_video_out_downscale_spv.h"
#include "gpu_blit_shaders/gpu_video_out_overlay_spv.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/cache/streamBuffer.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/presentation/openxrHeadset.h"
#include "graphics/presentation/performanceOverlay.h"
#include "graphics/presentation/presenter.h"
#include "graphics/presentation/systemOverlay.h"
#include "graphics/presentation/videoOut.h"
#include "graphics/presentation/window/lowLatency.h"
#include "graphics/presentation/window/windowInternal.h"
#include "libs/controller.h"
#include "streaming/captureOptions.h"
#include "streaming/exporter.h"
#include "streaming/metricsSource.h"
#include "streaming/sessionEnv.h"
#include "streaming/transport.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <charconv>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>
#include <vulkan/vk_platform.h>

// IWYU pragma: no_include <intrin.h>

namespace Libs::Graphics {

struct Presenter::Frame {
	VulkanImage   image;
	vk::ImageView view         = nullptr;
	uint64_t      present_tick = 0;
	bool          busy         = false;
	bool          hdr          = false;
	bool          blank        = false;

	void Configure(GraphicContext& graphics, vk::Extent2D extent, vk::Format format);
	void Transit(vk::CommandBuffer command, vk::ImageLayout layout, vk::AccessFlags2 access);
	void CopyFrom(CommandBuffer& command, Image& source);
	void Clear(CommandBuffer& command, const vk::ClearColorValue& color);
};

class FramePool final {
public:
	FramePool(WindowContext& window, CommandScheduler& scheduler)
	    : m_window(window), m_scheduler(scheduler) {}
	~FramePool() {
		m_scheduler.Wait(m_scheduler.CurrentTick() - 1);
		for (auto& frame: m_frames) {
			m_window.graphic_ctx.device.destroyImageView(frame->view, nullptr);
			if (frame->image.image != nullptr) {
				m_window.graphic_ctx.DeleteImage(frame->image);
			}
		}
	}
	KYTY_CLASS_NO_COPY(FramePool);

	void Initialize(uint32_t count, vk::Format format) {
		Common::LockGuard lock(m_mutex);
		m_format = format;
		m_frames.reserve(count);
		m_free.reserve(count);
		for (uint32_t i = 0; i < count; i++) {
			auto frame = std::make_unique<Presenter::Frame>();
			m_free.push_back(frame.get());
			m_frames.push_back(std::move(frame));
		}
	}

	void SetFormat(vk::Format format) {
		Common::LockGuard lock(m_mutex);
		m_format = format;
	}

	vk::Format GetFormat() {
		Common::LockGuard lock(m_mutex);
		return m_format;
	}

	Presenter::Frame* Acquire(vk::Extent2D extent, vk::Format format) {
		m_mutex.Lock();
		// A synchronized flip may need more frames than the swapchain has images.
		// Queue capacity bounds growth; waiting here can prevent its master from recording.
		if (m_free.empty()) {
			auto frame = std::make_unique<Presenter::Frame>();
			m_free.push_back(frame.get());
			m_frames.push_back(std::move(frame));
		}
		const auto compatible = std::ranges::find_if(m_free, [&](const auto* frame) {
			return frame->image.extent.width == extent.width &&
			       frame->image.extent.height == extent.height && frame->image.format == format;
		});
		auto*      frame      = compatible == m_free.end() ? m_free.back() : *compatible;
		if (compatible != m_free.end()) {
			*compatible = m_free.back();
		}
		m_free.pop_back();
		frame->busy = true;
		m_mutex.Unlock();

		m_scheduler.Wait(frame->present_tick);
		return frame;
	}

	void Release(Presenter::Frame* frame) {
		Common::LockGuard lock(m_mutex);
		if (!frame->busy) {
			EXIT("prepared frame was released twice\n");
		}
		frame->busy = false;
		m_free.push_back(frame);
	}

private:
	WindowContext&                                 m_window;
	CommandScheduler&                              m_scheduler;
	Common::Mutex                                  m_mutex;
	std::vector<std::unique_ptr<Presenter::Frame>> m_frames;
	std::vector<Presenter::Frame*>                 m_free;
	vk::Format                                     m_format = vk::Format::eUndefined;
};

void Presenter::Frame::Configure(GraphicContext& graphics, vk::Extent2D extent, vk::Format format) {
	if (extent.width == 0 || extent.height == 0 || format == vk::Format::eUndefined) {
		EXIT("unsupported prepared frame, extent=%ux%u format=%d\n", extent.width, extent.height,
		     static_cast<int>(format));
	}
	auto&      dst        = image;
	const bool compatible = dst.image != nullptr && dst.extent.width == extent.width &&
	                        dst.extent.height == extent.height && dst.format == format;
	if (compatible) {
		return;
	}

	const auto features = graphics.GetFormatProperties(format).optimalTilingFeatures;
	const auto required =
	    vk::FormatFeatureFlagBits::eBlitSrc | vk::FormatFeatureFlagBits::eSampledImageFilterLinear |
	    vk::FormatFeatureFlagBits::eSampledImage | vk::FormatFeatureFlagBits::eTransferSrc |
	    vk::FormatFeatureFlagBits::eTransferDst;
	if ((features & required) != required) {
		EXIT("prepared presentation format lacks optimal blit support: format=%d features=0x%x\n",
		     static_cast<int>(format), static_cast<vk::FormatFeatureFlags::MaskType>(features));
	}

	if (dst.image != nullptr) {
		graphics.device.destroyImageView(view, nullptr);
		view = nullptr;
		graphics.DeleteImage(dst);
	}

	vk::ImageCreateInfo create {};
	create.sType         = vk::StructureType::eImageCreateInfo;
	create.imageType     = vk::ImageType::e2D;
	create.extent        = {extent.width, extent.height, 1};
	create.mipLevels     = 1;
	create.arrayLayers   = 1;
	create.format        = format;
	create.tiling        = vk::ImageTiling::eOptimal;
	create.initialLayout = vk::ImageLayout::eUndefined;
	create.usage = vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst |
	               vk::ImageUsageFlagBits::eSampled;
	create.sharingMode = vk::SharingMode::eExclusive;
	create.samples     = vk::SampleCountFlagBits::e1;
	if (!graphics.CreateImage(create, dst)) {
		EXIT("failed to allocate prepared presentation image, extent=%ux%u format=%d\n",
		     extent.width, extent.height, static_cast<int>(format));
	}
}

void Presenter::Frame::Transit(vk::CommandBuffer command, vk::ImageLayout layout,
                               vk::AccessFlags2 access) {
	const auto     stage  = access == vk::AccessFlagBits2::eTransferRead ||
	                                access == vk::AccessFlagBits2::eTransferWrite
	                            ? vk::PipelineStageFlagBits2::eTransfer
	                            : vk::PipelineStageFlagBits2::eAllCommands;
	constexpr auto writes = vk::AccessFlagBits2::eTransferWrite |
	                        vk::AccessFlagBits2::eShaderWrite | vk::AccessFlagBits2::eMemoryWrite;
	if (image.state.layout == layout && image.state.access_mask == access &&
	    !static_cast<bool>(image.state.access_mask & writes)) {
		return;
	}
	vk::ImageMemoryBarrier2 barrier {};
	barrier.srcStageMask                    = image.state.pl_stage;
	barrier.srcAccessMask                   = image.state.access_mask;
	barrier.dstStageMask                    = stage;
	barrier.dstAccessMask                   = access;
	barrier.oldLayout                       = image.state.layout;
	barrier.newLayout                       = layout;
	barrier.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
	barrier.image                           = image.image;
	barrier.subresourceRange.aspectMask     = vk::ImageAspectFlagBits::eColor;
	barrier.subresourceRange.baseMipLevel   = 0;
	barrier.subresourceRange.levelCount     = VK_REMAINING_MIP_LEVELS;
	barrier.subresourceRange.baseArrayLayer = 0;
	barrier.subresourceRange.layerCount     = VK_REMAINING_ARRAY_LAYERS;
	vk::DependencyInfo dependency {};
	dependency.imageMemoryBarrierCount = 1;
	dependency.pImageMemoryBarriers    = &barrier;
	command.pipelineBarrier2(dependency);
	image.state = {stage, access, layout};
}

void Presenter::Frame::CopyFrom(CommandBuffer& command_buffer, Image& source) {
	command_buffer.EndRendering();
	auto command = command_buffer.Handle();
	source.Transit(vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead, {},
	               command);
	Transit(command, vk::ImageLayout::eTransferDstOptimal, vk::AccessFlagBits2::eTransferWrite);
	vk::ImageCopy copy {};
	copy.srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, source.backing.layers};
	copy.dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, image.layers};
	copy.extent         = {std::min(source.backing.extent.width, image.extent.width),
	                       std::min(source.backing.extent.height, image.extent.height), 1};
	EXIT_IF(copy.srcSubresource.layerCount != copy.dstSubresource.layerCount);
	command.copyImage(source.backing.image, vk::ImageLayout::eTransferSrcOptimal, image.image,
	                  vk::ImageLayout::eTransferDstOptimal, copy);
}

void Presenter::Frame::Clear(CommandBuffer& command_buffer, const vk::ClearColorValue& color) {
	command_buffer.EndRendering();
	auto command = command_buffer.Handle();
	Transit(command, vk::ImageLayout::eTransferDstOptimal, vk::AccessFlagBits2::eTransferWrite);
	const vk::ImageSubresourceRange range {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
	command.clearColorImage(image.image, vk::ImageLayout::eTransferDstOptimal, &color, 1, &range);
}

class Swapchain final {
public:
	enum class Status : uint8_t { Success, Recreate, SurfaceLost, Minimized };

	explicit Swapchain(WindowContext& window): m_window(window) {}
	~Swapchain();
	KYTY_CLASS_NO_COPY(Swapchain);

	/// Creates the Vulkan swapchain; sets m_minimized when the window or surface extent is zero.
	void Create();
	/// Destroys then re-creates the swapchain, optionally recreating the Vulkan surface first.
	void Recreate(bool surface_lost = false);
	/// Returns true when the swapchain must be recreated before the next present.
	[[nodiscard]] bool   NeedsResize() const;
	[[nodiscard]] Status AcquireNextImage(CommandScheduler& scheduler);
	[[nodiscard]] bool   PrepareSystemOverlay();
	void     RecordPresentCommands(CommandBuffer& command, Presenter::Frame* source,
	                               const Presenter::Layer& overlay, bool draw_system_overlay);
	uint64_t Submit(CommandScheduler& scheduler);
	[[nodiscard]] Status Present();

	[[nodiscard]] uint32_t ImageCount() const noexcept {
		return static_cast<uint32_t>(m_images.size());
	}
	[[nodiscard]] vk::Format Format() const noexcept { return m_format; }
	[[nodiscard]] bool       IsMinimized() const noexcept { return m_minimized; }
	[[nodiscard]] bool       IsHdrSupported() const noexcept { return m_hdr_supported.load(); }
	bool                     SetHdr(bool hdr) {
		const bool changed = m_hdr_active != (hdr && IsHdrSupported());
		m_hdr_requested    = hdr;
		return changed;
	}

private:
	vk::SurfaceFormatKHR SelectFormat();
	/// Destroys the swapchain Vulkan objects and resets all members.
	void Destroy();
	void DrawLayer(vk::CommandBuffer command, const Presenter::Layer& layer,
	               const vk::Rect2D& rect);

	WindowContext&   m_window;
	vk::SwapchainKHR m_handle = nullptr;
	vk::Format       m_format = vk::Format::eUndefined;
	std::atomic_bool m_hdr_supported {false};
	bool             m_hdr_requested = false;
	bool             m_hdr_active    = false;
	vk::Extent2D     m_extent {};
	// Drawable pixel size observed when this swapchain was created.
	vk::Extent2D m_window_extent {};
	bool         m_minimized = false;
	/// True when m_minimized was set because the surface/drawable extent was {0,0} but
	/// WindowContext::minimized is false (e.g. compositor not ready). Cleared on successful
	/// swapchain creation or Destroy().
	bool                           m_surface_extent_zero = false;
	bool                           m_suboptimal          = false;
	std::vector<vk::Image>         m_images;
	std::vector<vk::ImageView>     m_image_views;
	std::vector<vk::Semaphore>     m_image_acquired;
	std::vector<vk::Semaphore>     m_render_complete;
	std::vector<uint64_t>          m_frame_ticks;
	std::unique_ptr<SystemOverlay> m_system_overlay;
	vk::DescriptorSetLayout        m_overlay_descriptors = nullptr;
	vk::PipelineLayout             m_overlay_layout      = nullptr;
	vk::Pipeline                   m_overlay_pipeline    = nullptr;
	vk::Sampler                    m_overlay_sampler     = nullptr;
	PresentFilter                  m_filter; // KYTY_PRESENT_BOX_DOWNSCALE
	uint32_t                       m_image_index = static_cast<uint32_t>(-1);
	uint32_t                       m_frame_index = 0;
};

static bool PresentUnlocked();

// Optional producer is independent of swapchain acquisition and its physical extent.
class NativeStreamExport final {
public:
	NativeStreamExport(GraphicContext& graphics, CommandScheduler& scheduler, const char* socket,
	                   bool stream_only, bool metrics_fallback)
	    : m_graphics(graphics), m_scheduler(scheduler), m_stream_only(stream_only),
	      m_metrics_fallback(metrics_fallback) {
		const auto dimension = [](const char* name) -> uint32_t {
			const char* value = std::getenv(name);
			if (value == nullptr || *value == '\0') return 0;
			uint32_t   n      = 0;
			const auto parsed = std::from_chars(value, value + std::strlen(value), n);
			return parsed.ec == std::errc() && *parsed.ptr == '\0' && n >= 1 && n <= 4096
			           ? n
			           : UINT32_MAX;
		};
		m_width        = dimension("KYTY_STREAM_WIDTH");
		m_height       = dimension("KYTY_STREAM_HEIGHT");
		const auto fps = dimension("KYTY_STREAM_FPS");
		if (m_width == UINT32_MAX || m_height == UINT32_MAX || (m_width == 0) != (m_height == 0) ||
		    !Streaming::IsCaptureFrameRateSupported(fps)) {
			if (stream_only) EXIT("Streaming-only: invalid capture dimensions/FPS\n");
			LOGF("Streaming disabled: invalid capture dimensions/FPS\n");
			return;
		}
		if (stream_only) {
			m_system_overlay = std::make_unique<SystemOverlay>(graphics);
			SetSystemOverlayRemoteInputEnabled(true);
		}
		m_interval = std::chrono::nanoseconds(1000000000 / (fps == 0 ? 60 : fps));
		if (const char* aspect = std::getenv("KYTY_STREAM_ASPECT");
		    aspect != nullptr && *aspect != '\0') {
			m_aspect = Streaming::ParseAspectMode(aspect);
		}
		m_transport = std::make_unique<Streaming::Transport>(
		    socket,
		    [stream_only](const Streaming::Input* input) {
			    const bool connected = input != nullptr && input->connected;
			    const auto buttons   = input != nullptr ? input->buttons : 0;
			    const auto axes =
			        input != nullptr ? input->axes : std::array<int, 6> {128, 128, 128, 128, 0, 0};
			    if (stream_only)
				    SetRemoteSystemOverlayInput(connected, buttons, axes);
			    else
				    Libs::Controller::SetRemoteInput(connected, buttons, axes);
		    },
		    [&scheduler](uint64_t tick) { return scheduler.IsFree(tick); },
		    [](const Streaming::Motion* motion) { Libs::Controller::SetRemoteMotion(motion); },
		    Streaming::MakeEmulatorMetricsHooks());
	}
	// False when the transport could not be created (bad capture dimensions or FPS): the caller
	// then falls back to a metrics-only connection.
	bool HasTransport() const { return m_transport != nullptr; }
	~NativeStreamExport() {
		m_transport.reset();
		if (m_stream_only) SetSystemOverlayRemoteInputEnabled(false);
		m_scheduler.Wait(m_last_tick);
	}
	bool Capture(const std::array<Presenter::Layer, 2>& layers) {
		if (m_transport == nullptr || m_metrics_only) return false;
		// No PQ-to-SDR conversion exists in the native capture protocol. HDR is never
		// advertised while exporting; reject games that force a PQ source regardless.
		if (std::ranges::any_of(layers, [](const auto& layer) {
			    return layer.frame != nullptr && layer.frame->hdr;
		    })) {
			static std::atomic_flag warned = ATOMIC_FLAG_INIT;
			if (!warned.test_and_set(std::memory_order_relaxed))
				LOGF("Streaming: forced HDR/PQ source cannot be exported through the SDR capture "
				     "pool\n");
			return false;
		}
		if (layers[0].frame == nullptr && layers[1].frame == nullptr && !m_stream_only)
			return false;
		const auto generation = m_transport->Generation();
		if (generation == 0) return false;
		const auto*    source = layers[0].frame != nullptr ? layers[0].frame : layers[1].frame;
		const uint32_t width  = m_width != 0        ? m_width
		                        : source != nullptr ? source->image.extent.width
		                                            : m_graphics.screen_width;
		const uint32_t height = m_height != 0       ? m_height
		                        : source != nullptr ? source->image.extent.height
		                                            : m_graphics.screen_height;
		if (m_pool != nullptr && m_generation == generation &&
		    (m_pool->Descriptor().width != width || m_pool->Descriptor().height != height)) {
			m_transport->Reconnect();
			return false;
		}
		if (m_generation != generation) {
			// Retain at most two old pools plus the active pool across peer loss.
			if (m_pool != nullptr && m_retired.size() >= 2) {
				LOGF("Streaming disabled: retired GPU pool limit reached after peer loss/resize\n");
				m_transport.reset();
				return false;
			}
			if (m_pool != nullptr) m_retired.push_back(std::move(m_pool));
			std::string error;
			m_pool = Streaming::GpuPool::Create(m_graphics.physical_device, m_graphics.device,
			                                    m_graphics.queue_family, width, height, ++m_epoch,
			                                    error, m_aspect);
			if (m_pool == nullptr) {
				if (m_metrics_fallback) {
					// AGS: keep the connection for metrics; the host is told there are no frames.
					LOGF("Streaming frames disabled (metrics only): %s\n", error.c_str());
					m_metrics_only = true;
					m_transport->SetMetricsOnly();
					return false;
				}
				LOGF("Streaming disabled: %s\n", error.c_str());
				m_transport.reset();
				return false;
			}
			m_generation = generation;
			m_ring       = std::make_shared<Streaming::LeaseRing>(m_epoch);
			m_transport->SetPool(m_pool->Descriptor(), m_pool->Fds(), m_ring, generation);
			LOGF("Streaming GPU pool: %ux%u pitch=%u epoch=%llu UUID=%s\n", width, height,
			     m_pool->Descriptor().stride, static_cast<unsigned long long>(m_epoch),
			     m_pool->Descriptor().gpu_uuid.c_str());
		}
		if (!m_transport->Attached()) return false;
		const auto now = std::chrono::steady_clock::now();
		if (now - m_last_capture < m_interval) return false;
		const auto lease = m_ring->TryAcquire();
		if (!lease) {
			m_transport->Drop();
			return false;
		}
		m_last_capture = now;
		Streaming::GpuPool::OverlayRecorder draw_overlay;
		if (m_system_overlay != nullptr && GetSystemOverlayVisualState().active) {
			// ImGui owns transient vertex/index/texture uploads. Finish the previous use before
			// preparing the next frame; there is exactly one target and RenderDrawData per frame.
			m_scheduler.Wait(m_last_tick);
			if (m_system_overlay->PrepareFrame({width, height}, Streaming::GpuPool::CompositeFormat,
			                                   3)) {
				draw_overlay = [this](auto command, auto target) {
					m_system_overlay->Record(command, target);
				};
			}
		}
		std::optional<RenderLockGuard> render_lock;
		if (!PresentUnlocked()) render_lock.emplace(m_scheduler.Context().GetMutex());
		auto& wrapper = m_scheduler.BeginCommand();
		wrapper.EndRendering();
		const auto                             command = wrapper.Handle();
		std::array<Streaming::CaptureLayer, 2> capture {};
		for (size_t bus = 0; bus < layers.size(); bus++) {
			if (auto* frame = layers[bus].frame; frame != nullptr) {
				if (bus == 0)
					frame->Transit(command, vk::ImageLayout::eTransferSrcOptimal,
					               vk::AccessFlagBits2::eTransferRead);
				else {
					frame->Transit(command, vk::ImageLayout::eShaderReadOnlyOptimal,
					               vk::AccessFlagBits2::eShaderRead);
					if (frame->view == nullptr) {
						vk::ImageViewCreateInfo view {};
						view.image            = frame->image.image;
						view.viewType         = vk::ImageViewType::e2D;
						view.format           = frame->image.format;
						view.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
						RequireVulkanSuccess(
						    m_graphics.device.createImageView(&view, nullptr, &frame->view),
						    "create streaming source view");
					}
				}
				capture[bus] = {frame->image.image,
				                frame->view,
				                {frame->image.extent.width, frame->image.extent.height},
				                layers[bus].premultiplied_alpha};
			}
		}
		m_pool->Record(command, *lease, capture[0], capture[1], draw_overlay);
		SubmitInfo submit;
		if (lease->recycled)
			submit.AddWait(m_pool->Done(lease->slot), 0, vk::PipelineStageFlagBits::eTransfer);
		submit.AddSignal(m_pool->Ready(lease->slot), 0);
		const auto tick = m_scheduler.Submit(submit, true);
		m_last_tick     = tick;
		for (const auto& layer: layers)
			if (layer.frame != nullptr) layer.frame->present_tick = tick;
		m_transport->Publish(
		    {*lease, tick,
		     static_cast<uint64_t>(
		         std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch())
		             .count())});
		return true;
	}

private:
	GraphicContext&                                  m_graphics;
	CommandScheduler&                                m_scheduler;
	std::unique_ptr<Streaming::GpuPool>              m_pool;
	std::vector<std::unique_ptr<Streaming::GpuPool>> m_retired;
	std::shared_ptr<Streaming::LeaseRing>            m_ring;
	std::unique_ptr<Streaming::Transport>            m_transport;
	std::unique_ptr<SystemOverlay>                   m_system_overlay;
	bool                                             m_stream_only = false, m_metrics_only = false;
	bool                                             m_metrics_fallback = false;
	uint64_t                                         m_generation = 0, m_epoch = 0, m_last_tick = 0;
	uint32_t                                         m_width = 0, m_height = 0;
	Streaming::AspectMode                            m_aspect = Streaming::AspectMode::Fit;
	std::chrono::nanoseconds                         m_interval {};
	std::chrono::steady_clock::time_point            m_last_capture {};
};

struct Presenter::Impl {
	explicit Impl(WindowContext& owner)
	    : renderer(*owner.render_context), window(owner), swapchain(owner),
	      present_scheduler(renderer, owner.graphic_ctx, CommandScheduler::Role::Presenter),
	      frames(owner, present_scheduler) {
		EXIT_IF(owner.render_context == nullptr);
		if (!owner.stream_only) swapchain.Create();
		const uint32_t image_count = swapchain.ImageCount() > 0 ? swapchain.ImageCount() : 3;
		frames.Initialize(image_count, owner.stream_only ? Streaming::GpuPool::CompositeFormat
		                                                 : swapchain.Format());
#if defined(__linux__)
		if (owner.stream_only && !owner.graphic_ctx.streaming_external_enabled) {
			EXIT("Streaming-only: Vulkan external-memory streaming is unavailable\n");
		}
		if (const auto choice = Streaming::SessionSocketFromEnvironment(); choice.path != nullptr) {
			const char* socket          = choice.path;
			const bool  metrics_only_ok = Streaming::MetricsOnlyAllowed(choice.source);
			if (owner.graphic_ctx.streaming_external_enabled &&
			    Streaming::FramesRequested(std::getenv("KYTY_STREAM_FRAMES"), owner.stream_only,
			                               choice.source)) {
				stream_export = std::make_unique<NativeStreamExport>(
				    owner.graphic_ctx, present_scheduler, socket, owner.stream_only,
				    metrics_only_ok);
			}
			if (metrics_only_ok && (stream_export == nullptr || !stream_export->HasTransport())) {
				// No frame pool is possible (Gamescope capture, or Vulkan external memory is
				// missing): still connect so the client gets metrics. Never for the legacy
				// socket, whose host rejects a connection without a pool.
				stream_export.reset();
				metrics_only = std::make_unique<Streaming::Transport>(
				    socket, [](const Streaming::Input*) {}, [](uint64_t) { return true; },
				    Streaming::Transport::MotionSink {}, Streaming::MakeEmulatorMetricsHooks(),
				    true);
				LOGF("Session socket: metrics-only connection (no frame pool)\n");
			}
		}
#endif
	}

	void RecoverSwapchain(Swapchain::Status status) {
		LOGF("Recovering Vulkan swapchain%s\n",
		     status == Swapchain::Status::SurfaceLost ? " and surface" : "");
		Common::DebugCounters::Add(Common::DebugCounters::Counter::SwapchainRecreations);
		swapchain.Recreate(status == Swapchain::Status::SurfaceLost);
		if (!swapchain.IsMinimized()) {
			frames.SetFormat(swapchain.Format());
		}
	}

	Image& ResolveSurface(const ImageInfo& info) {
		TextureCache::ImageDesc desc {};
		desc.info                  = info;
		desc.view_info.format      = info.pixel_format;
		desc.view_info.type        = vk::ImageViewType::e2D;
		desc.view_info.aspect      = vk::ImageAspectFlagBits::eColor;
		desc.view_info.base_level  = 0;
		desc.view_info.level_count = 1;
		desc.view_info.base_layer  = 0;
		desc.view_info.layer_count = 1;
		desc.view_info.usage       = vk::ImageUsageFlagBits::eTransferSrc;
		desc.type                  = TextureCache::BindingType::VideoOut;

		auto&      cache      = renderer.GetTextureCache();
		const auto image_id   = cache.FindImage(desc);
		auto&      image      = cache.GetImage(image_id);
		image.usage.video_out = true;
		cache.UpdateImage(image_id);
		return image;
	}
	void Present();

	RenderContext&                      renderer;
	WindowContext&                      window;
	Swapchain                           swapchain;
	CommandScheduler                    present_scheduler;
	FramePool                           frames;
	std::unique_ptr<NativeStreamExport> stream_export;
	std::unique_ptr<Streaming::Transport> metrics_only;
	Common::Mutex                       present_mutex;
	std::array<Layer, 2>                layers {};
	std::atomic<uint64_t>               presented_overlay_revision {0};
};

/// Creates the Vulkan swapchain for the current window surface.
/// Sets m_minimized and returns early when the window or surface extent is zero.
/// Sets m_surface_extent_zero when the early return is due to a zero surface/drawable extent
/// and WindowContext::minimized is false, so NeedsResize() can poll instead of spinning.
vk::SurfaceFormatKHR Swapchain::SelectFormat() {
	const auto& formats    = m_window.surface_capabilities.formats;
	auto&       graphics   = m_window.graphic_ctx;
	const auto  hdr_format = std::ranges::find_if(formats, [&](const auto& candidate) {
		return candidate.colorSpace == vk::ColorSpaceKHR::eHdr10St2084EXT &&
		       IsHdrSwapchainFormat(candidate.format) &&
		       static_cast<bool>(
		           graphics.GetFormatProperties(candidate.format).optimalTilingFeatures &
		           vk::FormatFeatureFlagBits::eBlitDst);
	});
	m_hdr_supported        = Config::HdrEnabled() && hdr_format != formats.end();
	m_hdr_active           = m_hdr_requested && IsHdrSupported();
	if (m_hdr_active) {
		return *hdr_format;
	}
	if (Config::HdrEnabled() && !IsHdrSupported()) {
		LOGF("HDR10 surface unavailable; using SDR output\n");
	}
	if (formats.size() == 1 && formats.front().format == vk::Format::eUndefined) {
		return {vk::Format::eR8G8B8A8Unorm, vk::ColorSpaceKHR::eSrgbNonlinear};
	}
	for (const auto& candidate: formats) {
		if (candidate.colorSpace != vk::ColorSpaceKHR::eSrgbNonlinear) {
			continue;
		}
		switch (candidate.format) {
			case vk::Format::eB8G8R8A8Unorm:
			case vk::Format::eR8G8B8A8Unorm: return candidate;
			default: break;
		}
	}
	EXIT("no supported UNORM swapchain format\n");
}

void Swapchain::Create() {
	auto& graphics = m_window.graphic_ctx;
	EXIT_IF(graphics.device == nullptr);
	EXIT_IF(m_window.surface == nullptr);

	m_window.RefreshSurfaceCapabilities();
	const auto& surface = m_window.surface_capabilities;
	EXIT_NOT_IMPLEMENTED(surface.formats.empty());

	const auto format             = SelectFormat();
	m_format                      = format.format;
	const auto swapchain_features = graphics.GetFormatProperties(m_format).optimalTilingFeatures;
	if (!static_cast<bool>(swapchain_features & vk::FormatFeatureFlagBits::eBlitDst)) {
		EXIT("swapchain format cannot be a blit destination: format=%d\n",
		     static_cast<int>(m_format));
	}

	if (m_window.minimized.load(std::memory_order_acquire)) {
		// Window is genuinely minimized; m_surface_extent_zero is not the cause.
		m_minimized           = true;
		m_surface_extent_zero = false;
		return;
	}

	// Capture the size before querying surface capabilities. If the window changes
	// during creation, the next presentation will detect the newer size.
	{
		Common::LockGuard lock(m_window.mutex);
		m_window_extent = {graphics.screen_width, graphics.screen_height};
	}
	if (m_window_extent.width == 0 || m_window_extent.height == 0) {
		LOGF("Swapchain::Create(): drawable extent is zero while window is not minimized; "
		     "waiting for surface to become ready\n");
		m_minimized           = true;
		m_surface_extent_zero = true;
		return;
	}

	m_extent = surface.capabilities.currentExtent;
	if (m_extent.width == std::numeric_limits<uint32_t>::max()) {
		m_extent.width =
		    std::clamp(m_window_extent.width, surface.capabilities.minImageExtent.width,
		               surface.capabilities.maxImageExtent.width);
		m_extent.height =
		    std::clamp(m_window_extent.height, surface.capabilities.minImageExtent.height,
		               surface.capabilities.maxImageExtent.height);
	}
	if (m_extent.width == 0 || m_extent.height == 0) {
		LOGF("Swapchain::Create(): surface currentExtent is zero while window is not minimized; "
		     "waiting for surface to become ready\n");
		m_minimized           = true;
		m_surface_extent_zero = true;
		return;
	}

	m_minimized           = false;
	m_surface_extent_zero = false;
	m_suboptimal          = false;

	uint32_t image_count = surface.capabilities.minImageCount + 1;
	if (surface.capabilities.maxImageCount != 0) {
		image_count = std::min(image_count, surface.capabilities.maxImageCount);
	}
	const auto transform =
	    surface.capabilities.supportedTransforms & vk::SurfaceTransformFlagBitsKHR::eIdentity
	        ? vk::SurfaceTransformFlagBitsKHR::eIdentity
	        : surface.capabilities.currentTransform;
	const auto composite =
	    surface.capabilities.supportedCompositeAlpha & vk::CompositeAlphaFlagBitsKHR::eOpaque
	        ? vk::CompositeAlphaFlagBitsKHR::eOpaque
	        : vk::CompositeAlphaFlagBitsKHR::eInherit;

	vk::SwapchainCreateInfoKHR create_info {};
	create_info.sType            = vk::StructureType::eSwapchainCreateInfoKHR;
	create_info.surface          = m_window.surface;
	create_info.minImageCount    = image_count;
	create_info.imageFormat      = format.format;
	create_info.imageColorSpace  = format.colorSpace;
	create_info.imageExtent      = m_extent;
	create_info.imageArrayLayers = 1;
	create_info.imageUsage =
	    vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferDst;
	create_info.imageSharingMode = vk::SharingMode::eExclusive;
	create_info.preTransform     = transform;
	create_info.compositeAlpha   = composite;
	switch (Config::GetPresentMode()) {
		case Config::PresentMode::Mailbox:
			create_info.presentMode = vk::PresentModeKHR::eMailbox;
			break;
		case Config::PresentMode::Immediate:
			create_info.presentMode = vk::PresentModeKHR::eImmediate;
			break;
		case Config::PresentMode::Fifo:
		default: create_info.presentMode = vk::PresentModeKHR::eFifo; break;
	}
	if (std::find(surface.present_modes.begin(), surface.present_modes.end(),
	              create_info.presentMode) == surface.present_modes.end()) {
		LOGF("warning: requested present mode is unavailable; falling back to Fifo\n");
		create_info.presentMode = vk::PresentModeKHR::eFifo;
	}
	create_info.clipped = VK_TRUE;
#if defined(_WIN32)
	// KYTY_FULLSCREEN_EXCLUSIVE (GraphicContext::full_screen_exclusive): exclusive fullscreen
	// disallowed or allowed. (VkSurfaceFullScreenExclusiveInfoEXT is vulkan_win32.h's, not included
	// here: its layout.)
	struct {
		VkStructureType sType                 = VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_INFO_EXT;
		const void*     pNext                 = nullptr;
		int32_t         full_screen_exclusive = 0; // VkFullScreenExclusiveEXT: 1 allowed, 2 disallowed
	} exclusive;
	if (graphics.full_screen_exclusive >= 0) {
		exclusive.pNext                 = create_info.pNext;
		exclusive.full_screen_exclusive = graphics.full_screen_exclusive == 0 ? 2 : 1;
		create_info.pNext               = &exclusive;
	}
#endif
	create_info.pNext   = LowLatency::SwapchainCreateNext(create_info.pNext);
	RequireVulkanSuccess(graphics.device.createSwapchainKHR(&create_info, nullptr, &m_handle),
	                     "vkCreateSwapchainKHR");
	EXIT_IF(m_handle == nullptr);
	LowLatency::OnSwapchainCreated(static_cast<VkSwapchainKHR>(m_handle));
	LOGF("Vulkan presentation: %s\n", m_hdr_active ? "HDR10 (PQ)" : "SDR");

	m_images = EnumerateVulkan<vk::Image>(
	    "vkGetSwapchainImagesKHR", [&](uint32_t* count, vk::Image* images) {
		    return graphics.device.getSwapchainImagesKHR(m_handle, count, images);
	    });
	EXIT_NOT_IMPLEMENTED(m_images.empty());
	// Freeze reports depend on it (RTX 50 PCs froze in Mailbox, not in Fifo).
	LOGF("Swapchain: %ux%u, %zu images, present mode %s\n", m_extent.width, m_extent.height,
	     m_images.size(), vk::to_string(create_info.presentMode).c_str());

	m_image_views.resize(m_images.size());
	for (size_t i = 0; i < m_images.size(); i++) {
		vk::ImageViewCreateInfo view {};
		view.sType                           = vk::StructureType::eImageViewCreateInfo;
		view.image                           = m_images[i];
		view.viewType                        = vk::ImageViewType::e2D;
		view.format                          = m_format;
		view.components                      = {};
		view.subresourceRange.aspectMask     = vk::ImageAspectFlagBits::eColor;
		view.subresourceRange.baseArrayLayer = 0;
		view.subresourceRange.baseMipLevel   = 0;
		view.subresourceRange.layerCount     = 1;
		view.subresourceRange.levelCount     = 1;
		RequireVulkanSuccess(graphics.device.createImageView(&view, nullptr, &m_image_views[i]),
		                     "vkCreateImageView");
		EXIT_IF(m_image_views[i] == nullptr);
	}

	vk::SemaphoreCreateInfo semaphore_info {};
	semaphore_info.sType = vk::StructureType::eSemaphoreCreateInfo;
	m_image_acquired.resize(m_images.size());
	m_render_complete.resize(m_images.size());
	m_frame_ticks.assign(m_images.size(), 0);
	for (size_t i = 0; i < m_images.size(); i++) {
		RequireVulkanSuccess(
		    graphics.device.createSemaphore(&semaphore_info, nullptr, &m_image_acquired[i]),
		    "create swapchain image-acquired semaphore");
		RequireVulkanSuccess(
		    graphics.device.createSemaphore(&semaphore_info, nullptr, &m_render_complete[i]),
		    "create swapchain render-complete semaphore");
	}
}

Swapchain::~Swapchain() {
	Destroy();
}

/// Destroys all Vulkan swapchain objects and resets all members to their default state.
void Swapchain::Destroy() {
	m_minimized           = false;
	m_surface_extent_zero = false;
	m_suboptimal          = false;
	if (m_handle == nullptr && m_image_acquired.empty() && m_render_complete.empty() &&
	    m_image_views.empty()) {
		return;
	}
	auto& graphics = m_window.graphic_ctx;

	{
		Common::LockGuard queue_lock(graphics.queue_mutex);
		graphics.submission_queue.DrainPendingLocked();
		RequireVulkanSuccess(graphics.queue.waitIdle(), "wait for swapchain queue");
	}
	if (m_system_overlay != nullptr) {
		m_system_overlay->ReleaseVulkan();
	}
	graphics.device.destroyPipeline(m_overlay_pipeline, nullptr);
	graphics.device.destroyPipelineLayout(m_overlay_layout, nullptr);
	graphics.device.destroyDescriptorSetLayout(m_overlay_descriptors, nullptr);
	graphics.device.destroySampler(m_overlay_sampler, nullptr);
	m_overlay_pipeline    = nullptr;
	m_overlay_layout      = nullptr;
	m_overlay_descriptors = nullptr;
	m_overlay_sampler     = nullptr;
	m_filter.Release(graphics.device);

	for (const auto semaphore: m_image_acquired) {
		if (semaphore != nullptr) {
			graphics.device.destroySemaphore(semaphore, nullptr);
		}
	}
	for (const auto semaphore: m_render_complete) {
		if (semaphore != nullptr) {
			graphics.device.destroySemaphore(semaphore, nullptr);
		}
	}
	for (const auto view: m_image_views) {
		if (view != nullptr) {
			graphics.device.destroyImageView(view, nullptr);
		}
	}
	if (m_handle != nullptr) {
		LowLatency::OnSwapchainDestroyed(static_cast<VkSwapchainKHR>(m_handle));
		graphics.device.destroySwapchainKHR(m_handle, nullptr);
	}

	m_handle        = nullptr;
	m_format        = vk::Format::eUndefined;
	m_extent        = {};
	m_window_extent = {};
	m_frame_index   = 0;
	m_images.clear();
	m_image_views.clear();
	m_image_acquired.clear();
	m_render_complete.clear();
	m_frame_ticks.clear();
}

/// Destroys and re-creates the swapchain, optionally recreating the Vulkan surface first.
void Swapchain::Recreate(bool surface_lost) {
	Destroy();
	if (surface_lost) {
#if defined(__APPLE__)
		// Surface recreation goes through SDL_Vulkan_CreateSurface, which touches the
		// window's view/layer and must run on the main thread on macOS.
		EXIT_IF(!SDL_RunOnMainThread(
		    [](void* window) { static_cast<WindowContext*>(window)->RecreateSurface(); }, &m_window,
		    true));
#else
		m_window.RecreateSurface();
#endif
	}
	Create();
}

/// Returns true when the swapchain must be recreated before the next present.
/// In the m_surface_extent_zero case (compositor not ready, window not minimized) this
/// re-queries the surface capabilities and only returns true when the extent is usable,
/// preventing a recreate-every-frame busy-loop.
bool Swapchain::NeedsResize() const {
	const bool window_minimized = m_window.minimized.load(std::memory_order_acquire);
	if (m_minimized) {
		if (window_minimized) {
			// OS window is minimized; nothing to do yet.
			return false;
		}
		if (!m_surface_extent_zero) {
			// Swapchain was marked minimized because WindowContext::minimized was true at
			// creation time, but it has since been cleared -> recreate now.
			return true;
		}
		// m_surface_extent_zero: the OS window exists but the compositor has not yet
		// assigned a real extent. Re-query and only trigger a recreate once the extent
		// is non-zero; otherwise return false to avoid a spinning recreate loop.
		m_window.RefreshSurfaceCapabilities();
		const auto& caps   = m_window.surface_capabilities.capabilities;
		const auto& extent = caps.currentExtent;
		// Extent 0xFFFFFFFF means the surface size is determined by the swapchain;
		// in that case read the cached drawable size under the window mutex.
		if (extent.width == std::numeric_limits<uint32_t>::max()) {
			Common::LockGuard lock(m_window.mutex);
			const bool        drawable_ready =
			    m_window.graphic_ctx.screen_width > 0 && m_window.graphic_ctx.screen_height > 0;
			return drawable_ready;
		}
		Common::LockGuard lock(m_window.mutex);
		const bool        drawable_ready =
		    m_window.graphic_ctx.screen_width > 0 && m_window.graphic_ctx.screen_height > 0;
		return drawable_ready && extent.width > 0 && extent.height > 0;
	}
	if (window_minimized) {
		return true;
	}
	Common::LockGuard lock(m_window.mutex);
	return m_window_extent.width != m_window.graphic_ctx.screen_width ||
	       m_window_extent.height != m_window.graphic_ctx.screen_height;
}

Swapchain::Status Swapchain::AcquireNextImage(CommandScheduler& scheduler) {
	if (m_minimized || m_handle == nullptr) {
		return Status::Minimized;
	}
	if (m_frame_index < m_frame_ticks.size() && m_frame_ticks[m_frame_index] != 0) {
		scheduler.Wait(m_frame_ticks[m_frame_index]);
		m_frame_ticks[m_frame_index] = 0;
	}
	vk::Result result;
	{
		// Hang reports (issue #62, Linux): shows a present thread blocked in the driver's acquire.
		HangWatchdog::Scope acquire("vkAcquireNextImageKHR",
		                            reinterpret_cast<uint64_t>(static_cast<VkSwapchainKHR>(m_handle)),
		                            m_frame_index);
		result = m_window.graphic_ctx.device.acquireNextImageKHR(
		    m_handle, std::numeric_limits<uint64_t>::max(), m_image_acquired[m_frame_index], nullptr,
		    &m_image_index);
	}
	switch (result) {
		case vk::Result::eSuccess: break;
		case vk::Result::eSuboptimalKHR:
			LOGF("vkAcquireNextImageKHR returned vk::Result::eSuboptimalKHR\n");
			m_suboptimal = true;
			break;
		case vk::Result::eErrorOutOfDateKHR:
			LOGF("vkAcquireNextImageKHR returned vk::Result::eErrorOutOfDateKHR\n");
			return Status::Recreate;
		case vk::Result::eErrorUnknown:
			LOGF("vkAcquireNextImageKHR returned vk::Result::eErrorUnknown\n");
			return Status::Recreate;
		case vk::Result::eErrorSurfaceLostKHR:
			LOGF("vkAcquireNextImageKHR returned vk::Result::eErrorSurfaceLostKHR\n");
			return Status::SurfaceLost;
		default: EXIT("vkAcquireNextImageKHR failed: %s\n", vk::to_string(result).c_str());
	}
	return Status::Success;
}

bool Swapchain::PrepareSystemOverlay() {
	if (m_system_overlay == nullptr) {
		m_system_overlay = std::make_unique<SystemOverlay>(m_window.graphic_ctx);
	}
	return m_system_overlay->PrepareFrame(m_extent, m_format, ImageCount());
}

// KYTY_PRESENT_BOX_DOWNSCALE=1: present 1-2x downscales with PresentFilter (windowInternal.h).
static bool PresentBoxDownscaleEnabled() {
	static const bool enabled = [] {
		const auto* value = std::getenv("KYTY_PRESENT_BOX_DOWNSCALE");
		return value != nullptr && std::strcmp(value, "1") == 0;
	}();
	return enabled;
}

// A pipeline that draws the fullscreen triangle with `fragment_code` into one `format` attachment.
static vk::Pipeline CreateFullscreenPipeline(vk::Device device, vk::PipelineLayout layout,
                                             std::span<const uint32_t> fragment_code,
                                             vk::Format                format) {
	const auto vertex   = CompileSPV(GPU_BLIT_FS_TRIANGLE_SPV, device);
	const auto fragment = CompileSPV(fragment_code, device);
	std::array<vk::PipelineShaderStageCreateInfo, 2> stages {};
	stages[0].stage  = vk::ShaderStageFlagBits::eVertex;
	stages[0].module = vertex;
	stages[0].pName  = "main";
	stages[1].stage  = vk::ShaderStageFlagBits::eFragment;
	stages[1].module = fragment;
	stages[1].pName  = "main";
	vk::PipelineVertexInputStateCreateInfo   vertex_input {};
	vk::PipelineInputAssemblyStateCreateInfo assembly {};
	assembly.topology = vk::PrimitiveTopology::eTriangleList;
	vk::PipelineViewportStateCreateInfo viewport {};
	viewport.viewportCount = 1;
	viewport.scissorCount  = 1;
	vk::PipelineRasterizationStateCreateInfo rasterization {};
	rasterization.lineWidth = 1.0f;
	vk::PipelineMultisampleStateCreateInfo multisample {};
	multisample.rasterizationSamples = vk::SampleCountFlagBits::e1;
	vk::PipelineColorBlendAttachmentState attachment {};
	attachment.blendEnable    = VK_FALSE;
	attachment.colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
	                            vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
	vk::PipelineColorBlendStateCreateInfo blend {};
	blend.attachmentCount = 1;
	blend.pAttachments    = &attachment;
	const std::array dynamic_states {vk::DynamicState::eViewport, vk::DynamicState::eScissor};
	vk::PipelineDynamicStateCreateInfo dynamic {};
	dynamic.dynamicStateCount = static_cast<uint32_t>(dynamic_states.size());
	dynamic.pDynamicStates    = dynamic_states.data();
	vk::PipelineRenderingCreateInfo rendering {};
	rendering.colorAttachmentCount    = 1;
	rendering.pColorAttachmentFormats = &format;
	vk::GraphicsPipelineCreateInfo create {};
	create.pNext               = &rendering;
	create.stageCount          = static_cast<uint32_t>(stages.size());
	create.pStages             = stages.data();
	create.pVertexInputState   = &vertex_input;
	create.pInputAssemblyState = &assembly;
	create.pViewportState      = &viewport;
	create.pRasterizationState = &rasterization;
	create.pMultisampleState   = &multisample;
	create.pColorBlendState    = &blend;
	create.pDynamicState       = &dynamic;
	create.layout              = layout;
	vk::Pipeline pipeline      = nullptr;
	RequireVulkanSuccess(device.createGraphicsPipelines(nullptr, 1, &create, nullptr, &pipeline),
	                     "create video-out downscale pipeline");
	device.destroyShaderModule(fragment, nullptr);
	device.destroyShaderModule(vertex, nullptr);
	return pipeline;
}

// KYTY_HDR_TO_SDR (default 1; 0 off): a game that outputs HDR10 (PQ) video-out buffers while the
// window is SDR (HDR off in the launcher or no HDR display) is converted for display: the PQ code
// values were shown unchanged, so the picture looked washed out (Miles Morales outputs PQ whatever
// the reported dynamic range). KYTY_HDR_TO_SDR_WHITE_NITS (default 203, BT.2408 reference white)
// maps to SDR white; highlights roll off up to KYTY_HDR_TO_SDR_PEAK_NITS (default 1000).
static bool PqToSdrEnabled() {
	static const bool enabled = [] {
		const auto* value = std::getenv("KYTY_HDR_TO_SDR");
		return value == nullptr || std::strcmp(value, "0") != 0;
	}();
	return enabled;
}

static float EnvNits(const char* name, float fallback) {
	const auto* value = std::getenv(name);
	const float parsed = value != nullptr ? std::strtof(value, nullptr) : 0.0f;
	return parsed >= 1.0f && parsed <= 10000.0f ? parsed : fallback;
}

float PqToSdrWhiteNits() {
	static const float nits = EnvNits("KYTY_HDR_TO_SDR_WHITE_NITS", 203.0f);
	return nits;
}

float PqToSdrPeakNits() {
	static const float nits = EnvNits("KYTY_HDR_TO_SDR_PEAK_NITS", 1000.0f);
	return nits;
}

bool PresentNeedsFilter(vk::Extent2D source, vk::Extent2D target) noexcept {
	const bool shrinks = source.width > target.width || source.height > target.height;
	const bool halves =
	    source.width == 2ull * target.width && source.height == 2ull * target.height;
	return shrinks && !halves && source.width <= 2ull * target.width &&
	       source.height <= 2ull * target.height;
}

void PresentFilter::Record(vk::Device device, vk::CommandBuffer command, vk::ImageView source_view,
                           uint32_t level, vk::Extent2D source, vk::ImageView target_view,
                           vk::Format target_format, vk::Extent2D target, bool pq_to_sdr) {
	struct Constants {
		int32_t  size[2];
		float    scale[2];
		uint32_t level;
		uint32_t flags;
		float    white_nits;
		float    peak_nits;
	};
	static_assert(sizeof(Constants) == 32);
	if (m_layout == nullptr) {
		vk::SamplerCreateInfo sampler {};
		sampler.magFilter    = vk::Filter::eLinear;
		sampler.minFilter    = vk::Filter::eLinear;
		sampler.mipmapMode   = vk::SamplerMipmapMode::eNearest;
		sampler.addressModeU = vk::SamplerAddressMode::eClampToEdge;
		sampler.addressModeV = vk::SamplerAddressMode::eClampToEdge;
		sampler.addressModeW = vk::SamplerAddressMode::eClampToEdge;
		sampler.maxLod       = VK_LOD_CLAMP_NONE;
		RequireVulkanSuccess(device.createSampler(&sampler, nullptr, &m_sampler),
		                     "create video-out downscale sampler");
		const vk::DescriptorSetLayoutBinding binding {0, vk::DescriptorType::eCombinedImageSampler,
		                                              1, vk::ShaderStageFlagBits::eFragment};
		vk::DescriptorSetLayoutCreateInfo    descriptors {};
		descriptors.flags        = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR;
		descriptors.bindingCount = 1;
		descriptors.pBindings    = &binding;
		RequireVulkanSuccess(
		    device.createDescriptorSetLayout(&descriptors, nullptr, &m_descriptors),
		    "create video-out downscale descriptor layout");
		const vk::PushConstantRange  constants {vk::ShaderStageFlagBits::eFragment, 0,
		                                        sizeof(Constants)};
		vk::PipelineLayoutCreateInfo layout {};
		layout.setLayoutCount         = 1;
		layout.pSetLayouts            = &m_descriptors;
		layout.pushConstantRangeCount = 1;
		layout.pPushConstantRanges    = &constants;
		RequireVulkanSuccess(device.createPipelineLayout(&layout, nullptr, &m_layout),
		                     "create video-out downscale pipeline layout");
	}
	if (m_pipeline != nullptr && m_format != target_format) {
		device.destroyPipeline(m_pipeline, nullptr);
		m_pipeline = nullptr;
	}
	if (m_pipeline == nullptr) {
		m_pipeline =
		    CreateFullscreenPipeline(device, m_layout, GPU_VIDEO_OUT_DOWNSCALE_SPV, target_format);
		m_format = target_format;
	}

	const vk::DescriptorImageInfo image {m_sampler, source_view,
	                                     vk::ImageLayout::eShaderReadOnlyOptimal};
	vk::WriteDescriptorSet        write {};
	write.dstBinding      = 0;
	write.descriptorCount = 1;
	write.descriptorType  = vk::DescriptorType::eCombinedImageSampler;
	write.pImageInfo      = &image;
	command.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics, m_layout, 0, 1, &write);
	const Constants constants {
	    {static_cast<int32_t>(source.width), static_cast<int32_t>(source.height)},
	    {static_cast<float>(source.width) / static_cast<float>(target.width),
	     static_cast<float>(source.height) / static_cast<float>(target.height)},
	    level,
	    // The two-texel box covers at most a 2:1 downscale; beyond that (only an HDR10 frame on an
	    // SDR window takes this pass then) one linear tap per pixel, as the blit it replaces.
	    (pq_to_sdr ? 1u : 0u) |
	        (source.width > 2ull * target.width || source.height > 2ull * target.height ? 2u : 0u),
	    PqToSdrWhiteNits(),
	    PqToSdrPeakNits()};
	command.pushConstants(m_layout, vk::ShaderStageFlagBits::eFragment, 0, sizeof(constants),
	                      &constants);
	command.bindPipeline(vk::PipelineBindPoint::eGraphics, m_pipeline);
	vk::RenderingAttachmentInfo attachment {};
	attachment.imageView   = target_view;
	attachment.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
	attachment.loadOp      = vk::AttachmentLoadOp::eDontCare;
	attachment.storeOp     = vk::AttachmentStoreOp::eStore;
	vk::RenderingInfo rendering {};
	rendering.renderArea.extent    = target;
	rendering.layerCount           = 1;
	rendering.colorAttachmentCount = 1;
	rendering.pColorAttachments    = &attachment;
	command.beginRendering(&rendering);
	const vk::Viewport viewport {
	    0.0f, 0.0f, static_cast<float>(target.width), static_cast<float>(target.height),
	    0.0f, 1.0f};
	const vk::Rect2D scissor {{0, 0}, target};
	command.setViewport(0, 1, &viewport);
	command.setScissor(0, 1, &scissor);
	command.draw(3, 1, 0, 0);
	command.endRendering();
}

void PresentFilter::Release(vk::Device device) {
	device.destroyPipeline(m_pipeline, nullptr);
	device.destroyPipelineLayout(m_layout, nullptr);
	device.destroyDescriptorSetLayout(m_descriptors, nullptr);
	device.destroySampler(m_sampler, nullptr);
	m_pipeline    = nullptr;
	m_layout      = nullptr;
	m_descriptors = nullptr;
	m_sampler     = nullptr;
	m_format      = vk::Format::eUndefined;
}

// The game frame's rectangle in the swapchain image: the window's whole extent with --stretch,
// else the largest one with the frame's aspect ratio, centred (upstream 69fdb03dc; the bars are
// cleared to black).
static vk::Rect2D FitAspect(const vk::Extent3D& source, const vk::Extent2D& target) {
	const uint64_t src_w = std::max(source.width, 1u);
	const uint64_t src_h = std::max(source.height, 1u);
	uint32_t       w     = target.width;
	uint32_t       h     = target.height;
	if (src_w * h > src_h * w) {
		h = std::max<uint32_t>(1, static_cast<uint32_t>(src_h * w / src_w));
	} else {
		w = std::max<uint32_t>(1, static_cast<uint32_t>(src_w * h / src_h));
	}
	return {{static_cast<int32_t>((target.width - w) / 2),
	         static_cast<int32_t>((target.height - h) / 2)},
	        {w, h}};
}

void Swapchain::DrawLayer(vk::CommandBuffer command, const Presenter::Layer& layer,
                          const vk::Rect2D& rect) {
	auto device = m_window.graphic_ctx.device;
	if (m_overlay_pipeline == nullptr) {
		vk::SamplerCreateInfo sampler {};
		sampler.magFilter    = vk::Filter::eLinear;
		sampler.minFilter    = vk::Filter::eLinear;
		sampler.addressModeU = vk::SamplerAddressMode::eClampToEdge;
		sampler.addressModeV = vk::SamplerAddressMode::eClampToEdge;
		sampler.addressModeW = vk::SamplerAddressMode::eClampToEdge;
		RequireVulkanSuccess(device.createSampler(&sampler, nullptr, &m_overlay_sampler),
		                     "create video-out overlay sampler");
		const vk::DescriptorSetLayoutBinding binding {0, vk::DescriptorType::eCombinedImageSampler,
		                                              1, vk::ShaderStageFlagBits::eFragment};
		vk::DescriptorSetLayoutCreateInfo    descriptors {};
		descriptors.flags        = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR;
		descriptors.bindingCount = 1;
		descriptors.pBindings    = &binding;
		RequireVulkanSuccess(
		    device.createDescriptorSetLayout(&descriptors, nullptr, &m_overlay_descriptors),
		    "create video-out overlay descriptor layout");
		const vk::PushConstantRange alpha {vk::ShaderStageFlagBits::eFragment, 0, sizeof(uint32_t)};
		vk::PipelineLayoutCreateInfo layout {};
		layout.setLayoutCount         = 1;
		layout.pSetLayouts            = &m_overlay_descriptors;
		layout.pushConstantRangeCount = 1;
		layout.pPushConstantRanges    = &alpha;
		RequireVulkanSuccess(device.createPipelineLayout(&layout, nullptr, &m_overlay_layout),
		                     "create video-out overlay pipeline layout");
		const auto vertex   = CompileSPV(GPU_BLIT_FS_TRIANGLE_SPV, device);
		const auto fragment = CompileSPV(GPU_VIDEO_OUT_OVERLAY_SPV, device);
		std::array<vk::PipelineShaderStageCreateInfo, 2> stages {};
		stages[0].stage  = vk::ShaderStageFlagBits::eVertex;
		stages[0].module = vertex;
		stages[0].pName  = "main";
		stages[1].stage  = vk::ShaderStageFlagBits::eFragment;
		stages[1].module = fragment;
		stages[1].pName  = "main";
		vk::PipelineVertexInputStateCreateInfo   vertex_input {};
		vk::PipelineInputAssemblyStateCreateInfo assembly {};
		assembly.topology = vk::PrimitiveTopology::eTriangleList;
		vk::PipelineViewportStateCreateInfo viewport {};
		viewport.viewportCount = 1;
		viewport.scissorCount  = 1;
		vk::PipelineRasterizationStateCreateInfo rasterization {};
		rasterization.lineWidth = 1.0f;
		vk::PipelineMultisampleStateCreateInfo multisample {};
		multisample.rasterizationSamples = vk::SampleCountFlagBits::e1;
		vk::PipelineColorBlendAttachmentState attachment {};
		attachment.blendEnable         = VK_TRUE;
		attachment.srcColorBlendFactor = vk::BlendFactor::eOne;
		attachment.dstColorBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha;
		attachment.srcAlphaBlendFactor = vk::BlendFactor::eOne;
		attachment.dstAlphaBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha;
		attachment.colorWriteMask = vk::ColorComponentFlagBits::eR |
		                            vk::ColorComponentFlagBits::eG |
		                            vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
		vk::PipelineColorBlendStateCreateInfo blend {};
		blend.attachmentCount = 1;
		blend.pAttachments    = &attachment;
		const std::array dynamic_states {vk::DynamicState::eViewport, vk::DynamicState::eScissor};
		vk::PipelineDynamicStateCreateInfo dynamic {};
		dynamic.dynamicStateCount = static_cast<uint32_t>(dynamic_states.size());
		dynamic.pDynamicStates    = dynamic_states.data();
		vk::PipelineRenderingCreateInfo rendering {};
		rendering.colorAttachmentCount    = 1;
		rendering.pColorAttachmentFormats = &m_format;
		vk::GraphicsPipelineCreateInfo create {};
		create.pNext               = &rendering;
		create.stageCount          = static_cast<uint32_t>(stages.size());
		create.pStages             = stages.data();
		create.pVertexInputState   = &vertex_input;
		create.pInputAssemblyState = &assembly;
		create.pViewportState      = &viewport;
		create.pRasterizationState = &rasterization;
		create.pMultisampleState   = &multisample;
		create.pColorBlendState    = &blend;
		create.pDynamicState       = &dynamic;
		create.layout              = m_overlay_layout;
		RequireVulkanSuccess(
		    device.createGraphicsPipelines(nullptr, 1, &create, nullptr, &m_overlay_pipeline),
		    "create video-out overlay pipeline");
		device.destroyShaderModule(fragment, nullptr);
		device.destroyShaderModule(vertex, nullptr);
	}
	auto& frame = *layer.frame;
	if (frame.view == nullptr) {
		vk::ImageViewCreateInfo view {};
		view.image            = frame.image.image;
		view.viewType         = vk::ImageViewType::e2D;
		view.format           = frame.image.format;
		view.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
		RequireVulkanSuccess(device.createImageView(&view, nullptr, &frame.view),
		                     "create video-out overlay image view");
	}
	const vk::DescriptorImageInfo image {m_overlay_sampler, frame.view,
	                                     vk::ImageLayout::eShaderReadOnlyOptimal};
	vk::WriteDescriptorSet        write {};
	write.dstBinding      = 0;
	write.descriptorCount = 1;
	write.descriptorType  = vk::DescriptorType::eCombinedImageSampler;
	write.pImageInfo      = &image;
	command.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics, m_overlay_layout, 0, 1, &write);
	// The main bus ignores alpha.
	const uint32_t flags = (layer.premultiplied_alpha ? 1u : 0u) | (m_hdr_active ? 2u : 0u) |
	                       (frame.hdr ? 4u : 0u) |
	                       (frame.image.format == vk::Format::eR16G16B16A16Sfloat ? 8u : 0u) |
	                       (layer.bus == 0 ? 16u : 0u);
	command.pushConstants(m_overlay_layout, vk::ShaderStageFlagBits::eFragment, 0,
	                      sizeof(flags), &flags);
	command.bindPipeline(vk::PipelineBindPoint::eGraphics, m_overlay_pipeline);
	vk::RenderingAttachmentInfo attachment {};
	attachment.imageView   = m_image_views[m_image_index];
	attachment.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
	attachment.loadOp      = vk::AttachmentLoadOp::eLoad;
	attachment.storeOp     = vk::AttachmentStoreOp::eStore;
	vk::RenderingInfo rendering {};
	rendering.renderArea.extent    = m_extent;
	rendering.layerCount           = 1;
	rendering.colorAttachmentCount = 1;
	rendering.pColorAttachments    = &attachment;
	command.beginRendering(&rendering);
	const vk::Viewport viewport {static_cast<float>(rect.offset.x),
	                             static_cast<float>(rect.offset.y),
	                             static_cast<float>(rect.extent.width),
	                             static_cast<float>(rect.extent.height),
	                             0.0f,
	                             1.0f};
	command.setViewport(0, 1, &viewport);
	command.setScissor(0, 1, &rect);
	command.draw(3, 1, 0, 0);
	command.endRendering();
}

void Swapchain::RecordPresentCommands(CommandBuffer& command, Presenter::Frame* source,
                                      const Presenter::Layer& overlay, bool draw_system_overlay) {
	auto       vk_command      = command.Handle();
	const bool draw_overlay    = overlay.frame != nullptr;
	const bool draw_attachment = draw_overlay || draw_system_overlay;
	// KYTY_PRESENT_BOX_DOWNSCALE: shrinking by less than 2x draws with PresentFilter instead of
	// the blit (windowInternal.h).
	const vk::Extent2D source_extent =
	    source != nullptr ? vk::Extent2D {source->image.extent.width, source->image.extent.height}
	                      : vk::Extent2D {};
	// An HDR10 frame on an SDR window is converted by the filter pass (KYTY_HDR_TO_SDR).
	const bool pq_to_sdr = source != nullptr && source->hdr && !m_hdr_active && PqToSdrEnabled();
	const bool filtered  = source != nullptr && (pq_to_sdr || (PresentBoxDownscaleEnabled() &&
	                                                           PresentNeedsFilter(source_extent, m_extent)));
	if (filtered) {
		if (source->view == nullptr) {
			vk::ImageViewCreateInfo view {};
			view.image            = source->image.image;
			view.viewType         = vk::ImageViewType::e2D;
			view.format           = source->image.format;
			view.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
			RequireVulkanSuccess(
			    m_window.graphic_ctx.device.createImageView(&view, nullptr, &source->view),
			    "create presentation source view");
		}
		source->Transit(vk_command, vk::ImageLayout::eShaderReadOnlyOptimal,
		                vk::AccessFlagBits2::eShaderRead);
	} else if (source != nullptr) {
		source->Transit(vk_command, vk::ImageLayout::eTransferSrcOptimal,
		                vk::AccessFlagBits2::eTransferRead);
	}
	if (draw_overlay) {
		overlay.frame->Transit(vk_command, vk::ImageLayout::eShaderReadOnlyOptimal,
		                       vk::AccessFlagBits2::eShaderRead);
	}

	vk::ImageMemoryBarrier to_transfer {};
	to_transfer.sType = vk::StructureType::eImageMemoryBarrier;
	to_transfer.dstAccessMask =
	    filtered ? vk::AccessFlagBits::eColorAttachmentWrite : vk::AccessFlagBits::eTransferWrite;
	to_transfer.oldLayout = vk::ImageLayout::eUndefined;
	to_transfer.newLayout =
	    filtered ? vk::ImageLayout::eColorAttachmentOptimal : vk::ImageLayout::eTransferDstOptimal;
	to_transfer.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
	to_transfer.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
	to_transfer.image                           = m_images[m_image_index];
	to_transfer.subresourceRange.aspectMask     = vk::ImageAspectFlagBits::eColor;
	to_transfer.subresourceRange.baseMipLevel   = 0;
	to_transfer.subresourceRange.levelCount     = 1;
	to_transfer.subresourceRange.baseArrayLayer = 0;
	to_transfer.subresourceRange.layerCount     = 1;
	// Match the acquire wait stage so the layout transition cannot precede acquisition.
	const auto write_stage = filtered ? vk::PipelineStageFlagBits::eColorAttachmentOutput
	                                  : vk::PipelineStageFlagBits::eTransfer;
	vk_command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, write_stage,
	                           vk::DependencyFlags {}, 0, nullptr, 0, nullptr, 1, &to_transfer);

	// The filter pass (KYTY_HDR_TO_SDR, KYTY_PRESENT_BOX_DOWNSCALE) always fills the whole image.
	const auto* aspect_source = source != nullptr ? source : overlay.frame;
	const auto  game_rect     = !filtered && !Config::StretchEnabled() && aspect_source != nullptr
	                                ? FitAspect(aspect_source->image.extent, m_extent)
	                                : vk::Rect2D {{0, 0}, m_extent};
	if (!filtered && (source == nullptr || game_rect.extent != m_extent)) {
		const vk::ClearColorValue black {};
		vk_command.clearColorImage(m_images[m_image_index], vk::ImageLayout::eTransferDstOptimal,
		                           &black, 1, &to_transfer.subresourceRange);
		if (source != nullptr) {
			vk::ImageMemoryBarrier after_clear = to_transfer;
			after_clear.srcAccessMask          = vk::AccessFlagBits::eTransferWrite;
			after_clear.oldLayout              = vk::ImageLayout::eTransferDstOptimal;
			vk_command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
			                           vk::PipelineStageFlagBits::eTransfer,
			                           vk::DependencyFlags {}, 0, nullptr, 0, nullptr, 1,
			                           &after_clear);
		}
	}
	if (filtered) {
		m_filter.Record(m_window.graphic_ctx.device, vk_command, source->view, 0, source_extent,
		                m_image_views[m_image_index], m_format, m_extent, pq_to_sdr);
	} else if (source != nullptr) {
		vk::ImageBlit region {};
		region.srcSubresource.aspectMask     = vk::ImageAspectFlagBits::eColor;
		region.srcSubresource.mipLevel       = 0;
		region.srcSubresource.baseArrayLayer = 0;
		region.srcSubresource.layerCount     = 1;
		region.srcOffsets[1].x               = static_cast<int>(source->image.extent.width);
		region.srcOffsets[1].y               = static_cast<int>(source->image.extent.height);
		region.srcOffsets[1].z               = 1;
		region.dstSubresource.aspectMask     = vk::ImageAspectFlagBits::eColor;
		region.dstSubresource.mipLevel       = 0;
		region.dstSubresource.baseArrayLayer = 0;
		region.dstSubresource.layerCount     = 1;
		region.dstOffsets[0].x = game_rect.offset.x;
		region.dstOffsets[0].y = game_rect.offset.y;
		region.dstOffsets[1].x = game_rect.offset.x + static_cast<int>(game_rect.extent.width);
		region.dstOffsets[1].y = game_rect.offset.y + static_cast<int>(game_rect.extent.height);
		region.dstOffsets[1].z               = 1;
		vk_command.blitImage(source->image.image, vk::ImageLayout::eTransferSrcOptimal,
		                     m_images[m_image_index], vk::ImageLayout::eTransferDstOptimal, 1,
		                     &region, vk::Filter::eLinear);
	}

	vk::ImageMemoryBarrier to_present {};
	to_present.sType = vk::StructureType::eImageMemoryBarrier;
	to_present.srcAccessMask =
	    filtered ? vk::AccessFlagBits::eColorAttachmentWrite : vk::AccessFlagBits::eTransferWrite;
	to_present.dstAccessMask       = draw_attachment ? vk::AccessFlagBits::eColorAttachmentRead |
	                                                       vk::AccessFlagBits::eColorAttachmentWrite
	                                                 : vk::AccessFlagBits::eMemoryRead;
	to_present.oldLayout           = to_transfer.newLayout;
	to_present.newLayout           = draw_attachment ? vk::ImageLayout::eColorAttachmentOptimal
	                                                 : vk::ImageLayout::ePresentSrcKHR;
	to_present.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	to_present.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	to_present.image               = m_images[m_image_index];
	to_present.subresourceRange.aspectMask     = vk::ImageAspectFlagBits::eColor;
	to_present.subresourceRange.baseMipLevel   = 0;
	to_present.subresourceRange.levelCount     = 1;
	to_present.subresourceRange.baseArrayLayer = 0;
	to_present.subresourceRange.layerCount     = 1;
	vk_command.pipelineBarrier(write_stage,
	                           draw_attachment ? vk::PipelineStageFlagBits::eColorAttachmentOutput
	                                           : vk::PipelineStageFlagBits::eAllCommands,
	                           vk::DependencyFlagBits::eByRegion, 0, nullptr, 0, nullptr, 1,
	                           &to_present);
	if (draw_attachment) {
		// Each pass blends over the color writes of the previous one.
		to_present.srcAccessMask      = vk::AccessFlagBits::eColorAttachmentWrite;
		to_present.oldLayout          = vk::ImageLayout::eColorAttachmentOptimal;
		const auto attachment_barrier = [&] {
			vk_command.pipelineBarrier(vk::PipelineStageFlagBits::eColorAttachmentOutput,
			                           vk::PipelineStageFlagBits::eColorAttachmentOutput,
			                           vk::DependencyFlagBits::eByRegion, 0, nullptr, 0, nullptr, 1,
			                           &to_present);
		};
		// Upstream 026189166 also draws an HDR10 main frame through DrawLayer here; ours converts it
		// in the filter pass above (KYTY_HDR_TO_SDR).
		if (draw_overlay) {
			DrawLayer(vk_command, overlay, game_rect);
			if (draw_system_overlay) {
				attachment_barrier();
			}
		}
		if (draw_system_overlay) {
			m_system_overlay->Record(vk_command, m_image_views[m_image_index]);
		}
		to_present.dstAccessMask = vk::AccessFlagBits::eMemoryRead;
		to_present.newLayout     = vk::ImageLayout::ePresentSrcKHR;
		vk_command.pipelineBarrier(vk::PipelineStageFlagBits::eColorAttachmentOutput,
		                           vk::PipelineStageFlagBits::eAllCommands,
		                           vk::DependencyFlagBits::eByRegion, 0, nullptr, 0, nullptr, 1,
		                           &to_present);
	}
}

uint64_t Swapchain::Submit(CommandScheduler& scheduler) {
	SubmitInfo submit;
	submit.AddWait(m_image_acquired[m_frame_index], 1, vk::PipelineStageFlagBits::eTransfer);
	submit.AddSignal(m_render_complete[m_image_index]);
	const auto tick = scheduler.Submit(submit);
	if (m_frame_index < m_frame_ticks.size()) {
		m_frame_ticks[m_frame_index] = tick;
	}
	return tick;
}

Swapchain::Status Swapchain::Present() {
	if (m_minimized || m_handle == nullptr) {
		return Status::Minimized;
	}
	const auto         ready = m_render_complete[m_image_index];
	vk::PresentInfoKHR present {};
	present.sType              = vk::StructureType::ePresentInfoKHR;
	present.swapchainCount     = 1;
	present.pSwapchains        = &m_handle;
	present.pImageIndices      = &m_image_index;
	present.pWaitSemaphores    = &ready;
	present.waitSemaphoreCount = 1;

	vk::Result result;
	{
		// Low-latency mode (lowLatency.h): the presented flip's frame number as present ID.
		const LowLatency::PresentScope latency(static_cast<VkSwapchainKHR>(m_handle));
		const uint64_t                 latency_id = latency.PresentId();
		vk::PresentIdKHR               present_id {};
		if (latency_id != 0) {
			present_id.swapchainCount = 1;
			present_id.pPresentIds    = &latency_id;
			present.pNext             = &present_id;
		}
		// The blit this present waits for goes ahead of game batches still waiting for their
		// texture copies: the present never waits for them, nor for a signal not yet submitted.
		// Hang reports (issue #62, Linux): a present blocked in the driver holds the graphics queue
		// lock, so these scopes tell a stuck vkQueuePresentKHR from a stuck GPU.
		const auto        queue = reinterpret_cast<uint64_t>(static_cast<VkQueue>(m_window.graphic_ctx.queue));
		HangWatchdog::Scope lock_scope("present-queue-lock", queue, m_image_index);
		Common::LockGuard   lock(m_window.graphic_ctx.queue_mutex);
		m_window.graphic_ctx.submission_queue.DrainReadyForPresentLocked();
		HangWatchdog::Scope present_scope("vkQueuePresentKHR", queue, m_image_index);
		result = m_window.graphic_ctx.queue.presentKHR(&present);
	}
	switch (result) {
		case vk::Result::eSuccess: break;
		case vk::Result::eSuboptimalKHR:
			LOGF("vkQueuePresentKHR returned vk::Result::eSuboptimalKHR\n");
			return Status::Recreate;
		case vk::Result::eErrorOutOfDateKHR:
			LOGF("vkQueuePresentKHR returned vk::Result::eErrorOutOfDateKHR\n");
			return Status::Recreate;
		case vk::Result::eErrorSurfaceLostKHR:
			LOGF("vkQueuePresentKHR returned vk::Result::eErrorSurfaceLostKHR\n");
			return Status::SurfaceLost;
		default: EXIT("vkQueuePresentKHR failed: %s\n", vk::to_string(result).c_str());
	}
	m_frame_index = (m_frame_index + 1u) % static_cast<uint32_t>(m_images.size());
	if (m_suboptimal) {
		m_suboptimal = false;
		return Status::Recreate;
	}
	return Status::Success;
}

Presenter::Presenter(WindowContext& window): m_impl(std::make_unique<Impl>(window)) {}

// KYTY_PRESENT_UNLOCKED (default on; =0 keeps the renderer's lock; chenxiao07/KytyPS5 a30144267):
// the present thread records and submits the swapchain blit without the renderer's lock. What it
// uses is the presenter's own: present_scheduler (its pool, timeline and command buffers; only the
// present thread records on it), the presenter's frames (the flip queue hands a frame over only
// once the GPU completed its copy, FlipQueue::Complete) and the swapchain images, and the graphics
// queue, which has its own lock (queue_mutex, also taken by the RenderDoc capture boundaries).
// Under the renderer's lock the GPU thread waited for that whole recording and submission once a
// frame. (This tree has no frame generation, which shares the renderer's state and keeps the lock
// in theirs.)
static bool PresentUnlocked() {
	static const bool enabled = [] {
		const auto* value = std::getenv("KYTY_PRESENT_UNLOCKED");
		return value == nullptr || std::strcmp(value, "0") != 0;
	}();
	return enabled;
}

Presenter::~Presenter() = default;

// KYTY_FRAME_DUMP=<frames>[:<x>,<y>,<w>,<h>] (diagnostic, default off): each time the file
// KYTY_FRAME_DUMP_TRIGGER (default _framedump.go in the working directory) appears, it is removed
// and the next <frames> prepared video-out frames (every prepared frame, also ones that are not
// shown; optional crop) are read back and appended to _framedump.bin ({u32 'KFDM', u32 dump
// ordinal, i32 guest frame, u32 width, u32 height, u32 vk format, u32 bytes per pixel, u64
// steady-clock ns at recording, pixels} per frame), to tell one-frame jumps from motion.
static void FrameDump(RenderContext& renderer, CommandBuffer& command_buffer, Presenter::Frame& frame) {
	struct Request {
		uint32_t    frames = 0;
		uint32_t    x = 0, y = 0, w = 0, h = 0;
		std::string trigger;
	};
	static const Request request = [] {
		Request parsed;
		if (const auto* value = std::getenv("KYTY_FRAME_DUMP"); value != nullptr) {
			char* end     = nullptr;
			parsed.frames = static_cast<uint32_t>(std::strtoul(value, &end, 10));
			if (end != nullptr && *end == ':') {
				std::array<uint32_t, 4> rect {};
				for (auto& v: rect) {
					v = static_cast<uint32_t>(std::strtoul(end + 1, &end, 10));
					if (end == nullptr || *end != ',') break;
				}
				parsed.x = rect[0];
				parsed.y = rect[1];
				parsed.w = rect[2];
				parsed.h = rect[3];
			}
		}
		const auto* trigger = std::getenv("KYTY_FRAME_DUMP_TRIGGER");
		parsed.trigger      = trigger != nullptr && trigger[0] != '\0' ? trigger : "_framedump.go";
		return parsed;
	}();
	if (request.frames == 0) [[likely]] {
		return;
	}
	static uint32_t remaining = 0;
	static uint32_t ordinal   = 0;
	if (remaining == 0) {
		std::error_code error;
		if (!std::filesystem::exists(request.trigger, error)) {
			return;
		}
		std::filesystem::remove(request.trigger, error);
		remaining = request.frames;
	}
	remaining--;
	const auto         format = frame.image.format;
	const uint32_t     bpp    = format == vk::Format::eR16G16B16A16Sfloat ? 8u : 4u;
	const auto         ex     = frame.image.extent;
	const uint32_t     x      = std::min(request.x, ex.width - 1);
	const uint32_t     y      = std::min(request.y, ex.height - 1);
	const uint32_t     w      = request.w == 0 ? ex.width - x : std::min(request.w, ex.width - x);
	const uint32_t     h      = request.h == 0 ? ex.height - y : std::min(request.h, ex.height - y);
	const uint64_t     bytes  = uint64_t {w} * h * bpp;
	constexpr uint32_t Slots  = 16;
	static std::unique_ptr<Buffer> download;
	static uint64_t                slot_bytes = 0;
	if (download == nullptr || bytes > slot_bytes) {
		auto& scheduler = renderer.GetCommandScheduler();
		if (download != nullptr) {
			scheduler.Wait(scheduler.CurrentTick() - 1);
			scheduler.PopPendingOperations();
		}
		slot_bytes = bytes;
		download   = std::make_unique<Buffer>(renderer.GetGraphics(), scheduler, MemoryUsage::Download, 0,
		                                      AllFlags, slot_bytes * Slots);
	}
	const auto slot    = ordinal % Slots;
	const auto number  = ordinal++;
	auto       command = command_buffer.Handle();
	frame.Transit(command, vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead);
	vk::BufferImageCopy copy {};
	copy.bufferOffset     = slot * slot_bytes;
	copy.imageSubresource = vk::ImageSubresourceLayers {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
	copy.imageOffset      = vk::Offset3D {static_cast<int32_t>(x), static_cast<int32_t>(y), 0};
	copy.imageExtent      = vk::Extent3D {w, h, 1};
	command.copyImageToBuffer(frame.image.image, vk::ImageLayout::eTransferSrcOptimal, download->Handle(),
	                          copy);
	vk::BufferMemoryBarrier barrier {};
	barrier.srcAccessMask       = vk::AccessFlagBits::eTransferWrite;
	barrier.dstAccessMask       = vk::AccessFlagBits::eHostRead;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.buffer              = download->Handle();
	barrier.offset              = copy.bufferOffset;
	barrier.size                = bytes;
	command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eHost, {}, {},
	                        barrier, {});
	const auto guest_frame = renderer.GetGpu().GetFrameNum();
	const auto t_ns        = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                std::chrono::steady_clock::now().time_since_epoch())
                                                .count());
	const auto vk_format   = static_cast<uint32_t>(format);
	renderer.GetCommandScheduler().DeferOperation(
	    [slot, number, guest_frame, w, h, vk_format, bpp, t_ns, bytes] {
		    static std::FILE* file = std::fopen("_framedump.bin", "wb");
		    if (file == nullptr) return;
		    download->Invalidate(slot * slot_bytes, bytes);
		    const uint32_t header[7] = {0x4d44464bu, number, static_cast<uint32_t>(guest_frame), w, h, vk_format,
		                                bpp};
		    std::fwrite(header, sizeof(header), 1, file);
		    std::fwrite(&t_ns, sizeof(t_ns), 1, file);
		    std::fwrite(download->Mapped().data() + slot * slot_bytes, 1, bytes, file);
		    std::fflush(file);
	    });
}

Presenter::Frame& Presenter::PrepareFrame(CommandBuffer& buffer, const ImageInfo& info, bool hdr,
                                          bool headset) {
	KYTY_PROFILER_FUNCTION();
	EXIT_IF(buffer.IsInvalid());
	auto frame_format = info.pixel_format;
	switch (frame_format) {
		case vk::Format::eR8G8B8A8Srgb: frame_format = vk::Format::eR8G8B8A8Unorm; break;
		case vk::Format::eB8G8R8A8Srgb: frame_format = vk::Format::eB8G8R8A8Unorm; break;
		default: break;
	}
	auto* frame  = m_impl->frames.Acquire({info.extent.width, info.extent.height}, frame_format);
	frame->hdr   = hdr;
	frame->blank = false;
	RenderLockGuard render_lock(m_impl->renderer.GetMutex());
	auto&           image = m_impl->ResolveSurface(info);
	if (image.backing.format == vk::Format::eUndefined) {
		EXIT("unsupported presentation source, image=%p\n", static_cast<const void*>(&image));
	}
	frame->Configure(m_impl->window.graphic_ctx,
	                 {image.backing.extent.width, image.backing.extent.height}, frame_format);
	frame->CopyFrom(buffer, image);
	FrameDump(m_impl->renderer, buffer, *frame);
	if (headset) {
		// The frame holds the raw display-encoded pixels the headset needs.
		frame->Transit(buffer.Handle(), vk::ImageLayout::eTransferSrcOptimal,
		               vk::AccessFlagBits2::eTransferRead);
		Vr::OpenXrQueueEyeSource(buffer, frame->image);
	}
	return *frame;
}

Presenter::Frame& Presenter::PrepareBlankFrame(uint32_t width, uint32_t height, bool opaque,
                                               CommandBuffer* producer) {
	KYTY_PROFILER_FUNCTION();
	auto  format = m_impl->frames.GetFormat();
	auto* frame  = m_impl->frames.Acquire({width, height}, format);
	frame->hdr   = false;
	frame->blank = true;
	// Into the GPU thread's command buffer (its flip) under the renderer's lock; the present
	// thread's blank frame (no producer) uses only the presenter's scheduler and frame.
	std::optional<RenderLockGuard> render_lock;
	if (producer != nullptr || !PresentUnlocked()) {
		render_lock.emplace(m_impl->renderer.GetMutex());
	}
	frame->Configure(m_impl->window.graphic_ctx, {width, height}, format);
	vk::ClearColorValue clear {};
	clear.float32[3] = opaque ? 1.0f : 0.0f;
	if (producer != nullptr) {
		EXIT_IF(producer->IsInvalid());
		frame->Clear(*producer, clear);
	} else {
		auto& command = m_impl->present_scheduler.BeginCommand();
		frame->Clear(command, clear);
		frame->present_tick = m_impl->present_scheduler.Submit();
	}
	return *frame;
}

bool Presenter::PresentLastFrame() {
	Common::LockGuard lock(m_impl->present_mutex);
	if (m_impl->layers[0].frame == nullptr && m_impl->layers[1].frame == nullptr) {
		return false;
	}
	m_impl->Present();
	return true;
}

bool Presenter::IsHdrSupported() const noexcept {
	// The native export protocol and GPU pool carry SDR only. Do not advertise HDR
	// while a streaming exporter is installed, including local-window capture.
	return m_impl->stream_export == nullptr && m_impl->swapchain.IsHdrSupported();
}

bool Presenter::NeedsSystemOverlayRefresh() const noexcept {
	const auto visual = GetSystemOverlayVisualState();
	return visual.active ||
	       visual.revision != m_impl->presented_overlay_revision.load(std::memory_order_acquire);
}

RenderContext& Presenter::Renderer() const noexcept {
	return m_impl->renderer;
}

void Presenter::Present(Frame& frame) {
	const Layer layer {&frame, 0, false};
	Present(std::span(&layer, 1));
}

void Presenter::Present(std::span<const Layer> layers) {
	Common::LockGuard lock(m_impl->present_mutex);
	for (const auto& layer: layers) {
		EXIT_IF(layer.bus < 0 || layer.bus >= static_cast<int>(m_impl->layers.size()));
		auto& previous = m_impl->layers[layer.bus];
		if (layer.hold && previous.frame != nullptr && previous.frame != layer.frame) {
			// KYTY_HOLD_SKIPPED_DRAWS: the frame shown before stays; this one goes back to the pool.
			m_impl->frames.Release(layer.frame);
			continue;
		}
		if (previous.frame != nullptr) {
			m_impl->frames.Release(previous.frame);
		}
		previous = layer;
	}
	m_impl->Present();
}

void Presenter::ClearLayer(int bus) {
	if (static_cast<size_t>(bus) >= m_impl->layers.size()) {
		return;
	}
	Common::LockGuard lock(m_impl->present_mutex);
	auto&             layer = m_impl->layers[bus];
	if (layer.frame != nullptr) {
		m_impl->frames.Release(layer.frame);
		layer = {};
	}
}

void Presenter::Impl::Present() {
	KYTY_PROFILER_FUNCTION();
	using Common::DebugCounters::Counter;
	using Common::DebugCounters::ScopedTimer;
	ScopedTimer present_time(Counter::PresentNs);

	const auto overlay_visual = GetSystemOverlayVisualState();
	const bool exported       = stream_export != nullptr && stream_export->Capture(layers);
	if (window.stream_only) {
		// A throttled/dropped frame must retry the final redraw that removes a closed dialog.
		if (exported)
			presented_overlay_revision.store(overlay_visual.revision, std::memory_order_release);
		return;
	}
	// Blank flips retain the current color space; real main-bus frames select it.
	const auto* main_frame = layers[0].frame;
	const bool  hdr_changed =
	    main_frame != nullptr && !main_frame->blank && swapchain.SetHdr(main_frame->hdr);
	// Some window systems keep presenting an old swapchain after a resize.
	if (hdr_changed || swapchain.NeedsResize()) {
		RecoverSwapchain(Swapchain::Status::Recreate);
	}
	if (swapchain.IsMinimized()) {
		return;
	}
	for (uint32_t attempt = 0; attempt < 2; attempt++) {
		auto status = [&] {
			ScopedTimer acquire_time(Counter::PresentAcquireNs);
			return swapchain.AcquireNextImage(present_scheduler);
		}();
		if (status == Swapchain::Status::Minimized) {
			return;
		}
		if (status != Swapchain::Status::Success) {
			RecoverSwapchain(status);
			if (swapchain.IsMinimized()) {
				return;
			}
			continue;
		}
		{
			std::optional<RenderLockGuard> render_lock;
			if (!PresentUnlocked()) {
				render_lock.emplace(renderer.GetMutex());
			}
			auto&      command             = present_scheduler.BeginCommand();
			const bool draw_system_overlay = [&] {
				ScopedTimer overlay_time(Counter::OverlayNs);
				return (overlay_visual.active || PerformanceOverlayEnabled()) &&
				       swapchain.PrepareSystemOverlay();
			}();
			swapchain.RecordPresentCommands(command, layers[0].frame, layers[1],
			                                draw_system_overlay);
			const auto tick = swapchain.Submit(present_scheduler);
			for (const auto& layer: layers) {
				if (layer.frame != nullptr) {
					layer.frame->present_tick = tick;
				}
			}
		}
		status = swapchain.Present();
		if (status == Swapchain::Status::Minimized) {
			return;
		}
		if (status != Swapchain::Status::Success) {
			RecoverSwapchain(status);
			if (swapchain.IsMinimized()) {
				return;
			}
			continue;
		}

		presented_overlay_revision.store(overlay_visual.revision, std::memory_order_release);
		window.loop.presented_frames.fetch_add(1, std::memory_order_relaxed);
		return;
	}
	LOGF("Vulkan presentation retry exhausted; dropping frame\n");
	Common::DebugCounters::Add(Common::DebugCounters::Counter::DroppedPresents);
}

void Presenter::Discard(Frame& frame) {
	m_impl->frames.Release(&frame);
}

WindowContext::WindowContext() = default;

} // namespace Libs::Graphics
