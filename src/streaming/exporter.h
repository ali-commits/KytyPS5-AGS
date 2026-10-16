#pragma once
#include "graphics/host_gpu/vulkanCommon.h"
#include "streaming/protocol.h"

#include <functional>
#include <memory>
namespace Streaming {
struct CaptureLayer {
	vk::Image     image;
	vk::ImageView view;
	vk::Extent2D  extent;
	bool          premultiplied = false;
};
// Independently testable Vulkan producer. Caller owns command submission and source transitions.
class GpuPool {
public:
	using OverlayRecorder = std::function<void(vk::CommandBuffer, vk::ImageView)>;
	static constexpr vk::Format     CompositeFormat = vk::Format::eB8G8R8A8Unorm;
	static std::unique_ptr<GpuPool> Create(vk::PhysicalDevice physical, vk::Device device,
	                                       uint32_t family, uint32_t width, uint32_t height,
	                                       uint64_t epoch, std::string& error);
	~GpuPool();
	GpuPool(const GpuPool&)            = delete;
	GpuPool& operator=(const GpuPool&) = delete;
	void     Record(vk::CommandBuffer command, const Lease& lease, const CaptureLayer& main,
	                const CaptureLayer& overlay, const OverlayRecorder& system_overlay = {});
	const PoolDescriptor&     Descriptor() const { return m_descriptor; }
	const std::array<int, 9>& Fds() const { return m_fds; }
	vk::Semaphore             Ready(uint32_t slot) const { return m_slots[slot].ready; }
	vk::Semaphore             Done(uint32_t slot) const { return m_slots[slot].done; }

private:
	GpuPool(vk::Device device, uint32_t family): m_device(device), m_family(family) {
		m_fds.fill(-1);
	}
	bool Initialize(vk::PhysicalDevice physical, uint32_t width, uint32_t height, uint64_t epoch,
	                std::string& error);
	bool CreateOverlay(std::string& error);
	struct Slot {
		vk::Buffer       buffer;
		vk::DeviceMemory memory;
		vk::Semaphore    ready, done;
	};
	vk::Device              m_device;
	uint32_t                m_family;
	PoolDescriptor          m_descriptor;
	std::array<Slot, 3>     m_slots {};
	std::array<int, 9>      m_fds;
	vk::Image               m_image;
	vk::DeviceMemory        m_image_memory;
	vk::ImageView           m_view;
	vk::Sampler             m_sampler;
	vk::DescriptorSetLayout m_descriptors;
	vk::PipelineLayout      m_layout;
	vk::Pipeline            m_pipeline;
};
} // namespace Streaming
