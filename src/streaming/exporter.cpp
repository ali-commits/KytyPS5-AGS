#include "streaming/exporter.h"

#include "gpu_blit_shaders/gpu_blit_fs_triangle_spv.h"
#include "gpu_blit_shaders/gpu_video_out_overlay_spv.h"

#include <algorithm>
#include <cstdio>
#if defined(__linux__)
#include <unistd.h>
#endif
namespace Streaming {
namespace {
bool Check(vk::Result result, const char* operation, std::string& error) {
	if (result == vk::Result::eSuccess) return true;
	error = std::string(operation) + ": " + std::to_string(static_cast<int>(result));
	return false;
}
uint32_t MemoryType(vk::PhysicalDevice physical, uint32_t bits) {
	vk::PhysicalDeviceMemoryProperties memory;
	physical.getMemoryProperties(&memory);
	for (uint32_t i = 0; i < memory.memoryTypeCount; i++)
		if ((bits & (1U << i)) &&
		    bool(memory.memoryTypes[i].propertyFlags & vk::MemoryPropertyFlagBits::eDeviceLocal))
			return i;
	return UINT32_MAX;
}
void ImageBarrier(vk::CommandBuffer command, vk::Image image, vk::ImageLayout old_layout,
                  vk::ImageLayout new_layout, vk::AccessFlags2 src, vk::AccessFlags2 dst) {
	vk::ImageMemoryBarrier2 barrier {};
	barrier.srcStageMask        = vk::PipelineStageFlagBits2::eAllCommands;
	barrier.srcAccessMask       = src;
	barrier.dstStageMask        = vk::PipelineStageFlagBits2::eAllCommands;
	barrier.dstAccessMask       = dst;
	barrier.oldLayout           = old_layout;
	barrier.newLayout           = new_layout;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image               = image;
	barrier.subresourceRange    = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
	vk::DependencyInfo dep {};
	dep.imageMemoryBarrierCount = 1;
	dep.pImageMemoryBarriers    = &barrier;
	command.pipelineBarrier2(dep);
}
} // namespace
std::unique_ptr<GpuPool> GpuPool::Create(vk::PhysicalDevice physical, vk::Device device,
                                         uint32_t family, uint32_t width, uint32_t height,
                                         uint64_t epoch, std::string& error) {
	auto pool = std::unique_ptr<GpuPool>(new GpuPool(device, family));
	if (!pool->Initialize(physical, width, height, epoch, error)) return {};
	return pool;
}
bool GpuPool::Initialize(vk::PhysicalDevice physical, uint32_t width, uint32_t height,
                         uint64_t epoch, std::string& error) {
#if !defined(__linux__)
	(void)physical;
	(void)width;
	(void)height;
	(void)epoch;
	error = "Vulkan FD transport requires Linux";
	return false;
#else
	if (width == 0 || height == 0 || width > 4096 || height > 4096 || epoch == 0) {
		error = "invalid stream dimensions/epoch";
		return false;
	}
	if (!VULKAN_HPP_DEFAULT_DISPATCHER.vkGetMemoryFdKHR ||
	    !VULKAN_HPP_DEFAULT_DISPATCHER.vkGetSemaphoreFdKHR) {
		error = "Vulkan external memory/semaphore FD extensions are unavailable";
		return false;
	}
	vk::PhysicalDeviceExternalBufferInfo external {};
	external.usage      = vk::BufferUsageFlagBits::eTransferDst;
	external.handleType = vk::ExternalMemoryHandleTypeFlagBits::eOpaqueFd;
	vk::ExternalBufferProperties properties {};
	physical.getExternalBufferProperties(&external, &properties);
	if (!bool(properties.externalMemoryProperties.externalMemoryFeatures &
	          vk::ExternalMemoryFeatureFlagBits::eExportable)) {
		error = "DEVICE_LOCAL buffer opaque FD export unsupported";
		return false;
	}
	vk::PhysicalDeviceExternalSemaphoreInfo semaphore_info {};
	semaphore_info.handleType = vk::ExternalSemaphoreHandleTypeFlagBits::eOpaqueFd;
	vk::ExternalSemaphoreProperties sem_props {};
	physical.getExternalSemaphoreProperties(&semaphore_info, &sem_props);
	if (!bool(sem_props.externalSemaphoreFeatures &
	          vk::ExternalSemaphoreFeatureFlagBits::eExportable)) {
		error = "binary semaphore opaque FD export unsupported";
		return false;
	}
	const auto features = physical.getFormatProperties(CompositeFormat).optimalTilingFeatures;
	const auto required = vk::FormatFeatureFlagBits::eBlitDst |
	                      vk::FormatFeatureFlagBits::eColorAttachment |
	                      vk::FormatFeatureFlagBits::eTransferSrc;
	if ((features & required) != required) {
		error = "BGRA stream compositor unsupported";
		return false;
	}
	m_descriptor.epoch  = epoch;
	m_descriptor.width  = width;
	m_descriptor.height = height;
	m_descriptor.stride = (width * 4 + 4095) & ~4095U;
	vk::PhysicalDeviceIDProperties id {};
	vk::PhysicalDeviceProperties2  props {};
	props.pNext = &id;
	physical.getProperties2(&props);
	char uuid[33] {};
	for (size_t i = 0; i < 16; i++)
		std::snprintf(uuid + i * 2, 3, "%02x", id.deviceUUID[i]);
	m_descriptor.gpu_uuid = uuid;
	const auto payload    = uint64_t(m_descriptor.stride) * height;
	for (size_t i = 0; i < SlotCount; i++) {
		auto&                              slot = m_slots[i];
		vk::ExternalMemoryBufferCreateInfo ext_buffer {};
		ext_buffer.handleTypes = vk::ExternalMemoryHandleTypeFlagBits::eOpaqueFd;
		vk::BufferCreateInfo buffer {};
		buffer.pNext       = &ext_buffer;
		buffer.size        = payload;
		buffer.usage       = vk::BufferUsageFlagBits::eTransferDst;
		buffer.sharingMode = vk::SharingMode::eExclusive;
		if (!Check(m_device.createBuffer(&buffer, nullptr, &slot.buffer),
		           "create external stream buffer", error))
			return false;
		const auto requirements = m_device.getBufferMemoryRequirements(slot.buffer);
		const auto type         = MemoryType(physical, requirements.memoryTypeBits);
		if (type == UINT32_MAX || requirements.size > 268435456) {
			error = "no bounded DEVICE_LOCAL external allocation";
			return false;
		}
		vk::MemoryDedicatedAllocateInfo dedicated {};
		dedicated.buffer = slot.buffer;
		vk::ExportMemoryAllocateInfo export_info {};
		export_info.pNext       = &dedicated;
		export_info.handleTypes = vk::ExternalMemoryHandleTypeFlagBits::eOpaqueFd;
		vk::MemoryAllocateInfo allocate {};
		allocate.pNext           = &export_info;
		allocate.allocationSize  = requirements.size;
		allocate.memoryTypeIndex = type;
		if (!Check(m_device.allocateMemory(&allocate, nullptr, &slot.memory),
		           "allocate external stream buffer", error) ||
		    !Check(m_device.bindBufferMemory(slot.buffer, slot.memory, 0),
		           "bind external stream buffer", error))
			return false;
		m_descriptor.allocation_sizes[i] = requirements.size;
		vk::MemoryGetFdInfoKHR fd_info {};
		fd_info.memory     = slot.memory;
		fd_info.handleType = vk::ExternalMemoryHandleTypeFlagBits::eOpaqueFd;
		if (!Check(m_device.getMemoryFdKHR(&fd_info, &m_fds[i * 3]), "export stream memory FD",
		           error))
			return false;
		vk::ExportSemaphoreCreateInfo export_sem {};
		export_sem.handleTypes = vk::ExternalSemaphoreHandleTypeFlagBits::eOpaqueFd;
		vk::SemaphoreCreateInfo sem {};
		sem.pNext = &export_sem;
		if (!Check(m_device.createSemaphore(&sem, nullptr, &slot.ready), "create READY", error) ||
		    !Check(m_device.createSemaphore(&sem, nullptr, &slot.done), "create DONE", error))
			return false;
		vk::SemaphoreGetFdInfoKHR sem_fd {};
		sem_fd.handleType = vk::ExternalSemaphoreHandleTypeFlagBits::eOpaqueFd;
		sem_fd.semaphore  = slot.ready;
		if (!Check(m_device.getSemaphoreFdKHR(&sem_fd, &m_fds[i * 3 + 1]), "export READY", error))
			return false;
		sem_fd.semaphore = slot.done;
		if (!Check(m_device.getSemaphoreFdKHR(&sem_fd, &m_fds[i * 3 + 2]), "export DONE", error))
			return false;
	}
	vk::ImageCreateInfo image {};
	image.imageType   = vk::ImageType::e2D;
	image.extent      = {width, height, 1};
	image.mipLevels   = 1;
	image.arrayLayers = 1;
	image.format      = CompositeFormat;
	image.tiling      = vk::ImageTiling::eOptimal;
	image.usage   = vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eTransferSrc |
	                vk::ImageUsageFlagBits::eColorAttachment;
	image.samples = vk::SampleCountFlagBits::e1;
	image.sharingMode = vk::SharingMode::eExclusive;
	if (!Check(m_device.createImage(&image, nullptr, &m_image), "create stream composite", error))
		return false;
	const auto requirements = m_device.getImageMemoryRequirements(m_image);
	const auto type         = MemoryType(physical, requirements.memoryTypeBits);
	if (type == UINT32_MAX) {
		error = "no DEVICE_LOCAL composite memory";
		return false;
	}
	vk::MemoryAllocateInfo allocate {};
	allocate.allocationSize  = requirements.size;
	allocate.memoryTypeIndex = type;
	if (!Check(m_device.allocateMemory(&allocate, nullptr, &m_image_memory),
	           "allocate stream composite", error) ||
	    !Check(m_device.bindImageMemory(m_image, m_image_memory, 0), "bind stream composite",
	           error))
		return false;
	vk::ImageViewCreateInfo view {};
	view.image            = m_image;
	view.viewType         = vk::ImageViewType::e2D;
	view.format           = image.format;
	view.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
	return Check(m_device.createImageView(&view, nullptr, &m_view), "create stream composite view",
	             error) &&
	       CreateOverlay(error);
#endif
}
bool GpuPool::CreateOverlay(std::string& error) {
	vk::SamplerCreateInfo sampler {};
	sampler.magFilter    = vk::Filter::eLinear;
	sampler.minFilter    = vk::Filter::eLinear;
	sampler.addressModeU = vk::SamplerAddressMode::eClampToEdge;
	sampler.addressModeV = vk::SamplerAddressMode::eClampToEdge;
	sampler.addressModeW = vk::SamplerAddressMode::eClampToEdge;
	if (!Check(m_device.createSampler(&sampler, nullptr, &m_sampler), "create stream sampler",
	           error))
		return false;
	const vk::DescriptorSetLayoutBinding binding {0, vk::DescriptorType::eCombinedImageSampler, 1,
	                                              vk::ShaderStageFlagBits::eFragment};
	vk::DescriptorSetLayoutCreateInfo    descriptors {};
	descriptors.flags        = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR;
	descriptors.bindingCount = 1;
	descriptors.pBindings    = &binding;
	if (!Check(m_device.createDescriptorSetLayout(&descriptors, nullptr, &m_descriptors),
	           "create stream descriptors", error))
		return false;
	const vk::PushConstantRange constants {vk::ShaderStageFlagBits::eFragment, 0, sizeof(uint32_t)};
	vk::PipelineLayoutCreateInfo layout {};
	layout.setLayoutCount         = 1;
	layout.pSetLayouts            = &m_descriptors;
	layout.pushConstantRangeCount = 1;
	layout.pPushConstantRanges    = &constants;
	if (!Check(m_device.createPipelineLayout(&layout, nullptr, &m_layout), "create stream layout",
	           error))
		return false;
	vk::ShaderModule           vertex, fragment;
	vk::ShaderModuleCreateInfo shader {};
	shader.codeSize = sizeof(GPU_BLIT_FS_TRIANGLE_SPV);
	shader.pCode    = GPU_BLIT_FS_TRIANGLE_SPV;
	if (!Check(m_device.createShaderModule(&shader, nullptr, &vertex),
	           "create stream vertex shader", error))
		return false;
	shader.codeSize = sizeof(GPU_VIDEO_OUT_OVERLAY_SPV);
	shader.pCode    = GPU_VIDEO_OUT_OVERLAY_SPV;
	if (!Check(m_device.createShaderModule(&shader, nullptr, &fragment),
	           "create stream overlay shader", error)) {
		m_device.destroyShaderModule(vertex);
		return false;
	}
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
	vk::PipelineRasterizationStateCreateInfo raster {};
	raster.lineWidth = 1;
	vk::PipelineMultisampleStateCreateInfo multisample {};
	multisample.rasterizationSamples = vk::SampleCountFlagBits::e1;
	vk::PipelineColorBlendAttachmentState attachment {};
	attachment.blendEnable         = VK_TRUE;
	attachment.srcColorBlendFactor = vk::BlendFactor::eOne;
	attachment.dstColorBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha;
	attachment.srcAlphaBlendFactor = vk::BlendFactor::eOne;
	attachment.dstAlphaBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha;
	attachment.colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
	                            vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
	vk::PipelineColorBlendStateCreateInfo blend {};
	blend.attachmentCount = 1;
	blend.pAttachments    = &attachment;
	const std::array states {vk::DynamicState::eViewport, vk::DynamicState::eScissor};
	vk::PipelineDynamicStateCreateInfo dynamic {};
	dynamic.dynamicStateCount              = states.size();
	dynamic.pDynamicStates                 = states.data();
	const vk::Format                format = CompositeFormat;
	vk::PipelineRenderingCreateInfo rendering {};
	rendering.colorAttachmentCount    = 1;
	rendering.pColorAttachmentFormats = &format;
	vk::GraphicsPipelineCreateInfo create {};
	create.pNext               = &rendering;
	create.stageCount          = stages.size();
	create.pStages             = stages.data();
	create.pVertexInputState   = &vertex_input;
	create.pInputAssemblyState = &assembly;
	create.pViewportState      = &viewport;
	create.pRasterizationState = &raster;
	create.pMultisampleState   = &multisample;
	create.pColorBlendState    = &blend;
	create.pDynamicState       = &dynamic;
	create.layout              = m_layout;
	const auto result = m_device.createGraphicsPipelines(nullptr, 1, &create, nullptr, &m_pipeline);
	m_device.destroyShaderModule(fragment);
	m_device.destroyShaderModule(vertex);
	return Check(result, "create stream overlay pipeline", error);
}
void GpuPool::Record(vk::CommandBuffer command, const Lease& lease, const CaptureLayer& main,
                     const CaptureLayer& overlay, const OverlayRecorder& system_overlay) {
	auto&                    slot = m_slots[lease.slot];
	vk::BufferMemoryBarrier2 acquire {};
	acquire.srcStageMask  = vk::PipelineStageFlagBits2::eNone;
	acquire.dstStageMask  = vk::PipelineStageFlagBits2::eTransfer;
	acquire.dstAccessMask = vk::AccessFlagBits2::eTransferWrite;
	acquire.srcQueueFamilyIndex =
	    lease.recycled ? VK_QUEUE_FAMILY_EXTERNAL : VK_QUEUE_FAMILY_IGNORED;
	acquire.dstQueueFamilyIndex = lease.recycled ? m_family : VK_QUEUE_FAMILY_IGNORED;
	acquire.buffer              = slot.buffer;
	acquire.size                = VK_WHOLE_SIZE;
	vk::DependencyInfo dep {};
	dep.bufferMemoryBarrierCount = 1;
	dep.pBufferMemoryBarriers    = &acquire;
	command.pipelineBarrier2(dep);
	ImageBarrier(command, m_image, vk::ImageLayout::eUndefined,
	             vk::ImageLayout::eTransferDstOptimal, vk::AccessFlagBits2::eTransferRead,
	             vk::AccessFlagBits2::eTransferWrite);
	if (main.image) {
		vk::ImageBlit blit {};
		blit.srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
		blit.srcOffsets[1]  = {int32_t(main.extent.width), int32_t(main.extent.height), 1};
		blit.dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
		blit.dstOffsets[1]  = {int32_t(m_descriptor.width), int32_t(m_descriptor.height), 1};
		command.blitImage(main.image, vk::ImageLayout::eTransferSrcOptimal, m_image,
		                  vk::ImageLayout::eTransferDstOptimal, 1, &blit, vk::Filter::eLinear);
	} else {
		const vk::ClearColorValue       black {};
		const vk::ImageSubresourceRange range {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
		command.clearColorImage(m_image, vk::ImageLayout::eTransferDstOptimal, &black, 1, &range);
	}
	const bool draw_hud = overlay.image && overlay.view;
	if (draw_hud || system_overlay) {
		ImageBarrier(command, m_image, vk::ImageLayout::eTransferDstOptimal,
		             vk::ImageLayout::eColorAttachmentOptimal, vk::AccessFlagBits2::eTransferWrite,
		             vk::AccessFlagBits2::eColorAttachmentRead |
		                 vk::AccessFlagBits2::eColorAttachmentWrite);
	}
	if (draw_hud) {
		const vk::DescriptorImageInfo image {m_sampler, overlay.view,
		                                     vk::ImageLayout::eShaderReadOnlyOptimal};
		vk::WriteDescriptorSet        write {};
		write.dstBinding      = 0;
		write.descriptorCount = 1;
		write.descriptorType  = vk::DescriptorType::eCombinedImageSampler;
		write.pImageInfo      = &image;
		command.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics, m_layout, 0, 1, &write);
		const uint32_t alpha = overlay.premultiplied;
		command.pushConstants(m_layout, vk::ShaderStageFlagBits::eFragment, 0, sizeof(alpha),
		                      &alpha);
		command.bindPipeline(vk::PipelineBindPoint::eGraphics, m_pipeline);
		vk::RenderingAttachmentInfo color {};
		color.imageView   = m_view;
		color.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
		color.loadOp      = vk::AttachmentLoadOp::eLoad;
		color.storeOp     = vk::AttachmentStoreOp::eStore;
		const vk::Extent2D extent {m_descriptor.width, m_descriptor.height};
		vk::RenderingInfo  rendering {};
		rendering.renderArea.extent    = extent;
		rendering.layerCount           = 1;
		rendering.colorAttachmentCount = 1;
		rendering.pColorAttachments    = &color;
		command.beginRendering(&rendering);
		const vk::Viewport viewport {0, 0, float(extent.width), float(extent.height), 0, 1};
		const vk::Rect2D   scissor {{0, 0}, extent};
		command.setViewport(0, 1, &viewport);
		command.setScissor(0, 1, &scissor);
		command.draw(3, 1, 0, 0);
		command.endRendering();
	}
	if (system_overlay) {
		if (draw_hud) {
			ImageBarrier(command, m_image, vk::ImageLayout::eColorAttachmentOptimal,
			             vk::ImageLayout::eColorAttachmentOptimal,
			             vk::AccessFlagBits2::eColorAttachmentWrite,
			             vk::AccessFlagBits2::eColorAttachmentRead |
			                 vk::AccessFlagBits2::eColorAttachmentWrite);
		}
		system_overlay(command, m_view);
	}
	if (draw_hud || system_overlay) {
		ImageBarrier(command, m_image, vk::ImageLayout::eColorAttachmentOptimal,
		             vk::ImageLayout::eTransferSrcOptimal,
		             vk::AccessFlagBits2::eColorAttachmentWrite,
		             vk::AccessFlagBits2::eTransferRead);
	} else
		ImageBarrier(command, m_image, vk::ImageLayout::eTransferDstOptimal,
		             vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferWrite,
		             vk::AccessFlagBits2::eTransferRead);
	vk::BufferImageCopy copy {};
	copy.bufferRowLength   = m_descriptor.stride / 4;
	copy.bufferImageHeight = m_descriptor.height;
	copy.imageSubresource  = {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
	copy.imageExtent       = {m_descriptor.width, m_descriptor.height, 1};
	command.copyImageToBuffer(m_image, vk::ImageLayout::eTransferSrcOptimal, slot.buffer, 1, &copy);
	vk::BufferMemoryBarrier2 release {};
	release.srcStageMask        = vk::PipelineStageFlagBits2::eTransfer;
	release.srcAccessMask       = vk::AccessFlagBits2::eTransferWrite;
	release.dstStageMask        = vk::PipelineStageFlagBits2::eNone;
	release.srcQueueFamilyIndex = m_family;
	release.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
	release.buffer              = slot.buffer;
	release.size                = VK_WHOLE_SIZE;
	dep.pBufferMemoryBarriers   = &release;
	command.pipelineBarrier2(dep);
}
GpuPool::~GpuPool() {
#if defined(__linux__)
	for (int fd: m_fds)
		if (fd >= 0) close(fd);
#endif
	m_device.destroyPipeline(m_pipeline);
	m_device.destroyPipelineLayout(m_layout);
	m_device.destroyDescriptorSetLayout(m_descriptors);
	m_device.destroySampler(m_sampler);
	m_device.destroyImageView(m_view);
	m_device.destroyImage(m_image);
	m_device.freeMemory(m_image_memory);
	for (auto& slot: m_slots) {
		m_device.destroySemaphore(slot.ready);
		m_device.destroySemaphore(slot.done);
		m_device.destroyBuffer(slot.buffer);
		m_device.freeMemory(slot.memory);
	}
}
} // namespace Streaming
