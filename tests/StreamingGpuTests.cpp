#include "streaming/exporter.h"
#include "streaming/transport.h"
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <thread>
#include <unistd.h>
#include <vector>
#if __has_include(<cuda.h>)
#include <cuda.h>
#define KYTY_STREAM_TEST_CUDA 1
#endif
VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE
using namespace Streaming;
namespace {
void Check(bool value, const char *text) {
  if (!value) {
    std::fprintf(stderr, "StreamingGpuTests FAIL: %s\n", text);
    std::abort();
  }
}
void Vk(vk::Result r, const char *operation) {
  if (r != vk::Result::eSuccess) {
    std::fprintf(stderr, "%s: %d\n", operation, int(r));
    std::abort();
  }
}
struct Image {
  vk::Image image;
  vk::ImageView view;
  vk::DeviceMemory memory;
};
class Fixture {
public:
  vk::Instance instance;
  vk::PhysicalDevice physical;
  vk::Device device;
  vk::Queue queue;
  uint32_t family = 0;
  vk::CommandPool commands;
  vk::CommandBuffer command;
  vk::Fence fence;
  Image main, overlay, pre_overlay;
  Fixture() {
    static vk::detail::DynamicLoader loader;
    auto get = loader.getProcAddress<PFN_vkGetInstanceProcAddr>(
        "vkGetInstanceProcAddr");
    Check(get != nullptr, "Vulkan loader");
    VULKAN_HPP_DEFAULT_DISPATCHER.init(get);
    vk::ApplicationInfo app{};
    app.pApplicationName = "KytyStreamingGpuTests";
    app.apiVersion = VK_API_VERSION_1_3;
    vk::InstanceCreateInfo create{};
    create.pApplicationInfo = &app;
    Vk(vk::createInstance(&create, nullptr, &instance), "instance");
    VULKAN_HPP_DEFAULT_DISPATCHER.init(instance);
    uint32_t count = 0;
    Vk(instance.enumeratePhysicalDevices(&count, nullptr), "count devices");
    std::vector<vk::PhysicalDevice> devices(count);
    Vk(instance.enumeratePhysicalDevices(&count, devices.data()), "devices");
    for (auto candidate : devices)
      if (candidate.getProperties().vendorID == 0x10de) {
        physical = candidate;
        break;
      }
    Check(bool(physical), "NVIDIA GPU available");
    auto properties = physical.getProperties();
    std::printf("Producer GPU: %s\n", properties.deviceName.data());
    uint32_t queues = 0;
    physical.getQueueFamilyProperties(&queues, nullptr);
    std::vector<vk::QueueFamilyProperties> families(queues);
    physical.getQueueFamilyProperties(&queues, families.data());
    for (uint32_t i = 0; i < queues; i++)
      if (bool(families[i].queueFlags & vk::QueueFlagBits::eGraphics)) {
        family = i;
        break;
      }
    const float priority = 1;
    vk::DeviceQueueCreateInfo q{};
    q.queueFamilyIndex = family;
    q.queueCount = 1;
    q.pQueuePriorities = &priority;
    const std::array extensions{VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
                                VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
                                VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME};
    vk::PhysicalDeviceVulkan13Features features{};
    features.dynamicRendering = VK_TRUE;
    features.synchronization2 = VK_TRUE;
    vk::DeviceCreateInfo info{};
    info.pNext = &features;
    info.queueCreateInfoCount = 1;
    info.pQueueCreateInfos = &q;
    info.enabledExtensionCount = extensions.size();
    info.ppEnabledExtensionNames = extensions.data();
    Vk(physical.createDevice(&info, nullptr, &device), "device");
    VULKAN_HPP_DEFAULT_DISPATCHER.init(device);
    device.getQueue(family, 0, &queue);
    vk::CommandPoolCreateInfo pool{};
    pool.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
    pool.queueFamilyIndex = family;
    Vk(device.createCommandPool(&pool, nullptr, &commands), "command pool");
    vk::CommandBufferAllocateInfo allocate{};
    allocate.commandPool = commands;
    allocate.level = vk::CommandBufferLevel::ePrimary;
    allocate.commandBufferCount = 1;
    Vk(device.allocateCommandBuffers(&allocate, &command), "command buffer");
    vk::FenceCreateInfo f{};
    Vk(device.createFence(&f, nullptr, &fence), "fence");
    main = Texture(false, false);
    overlay = Texture(true, false);
    pre_overlay = Texture(true, true);
  }
  uint32_t Memory(uint32_t bits, vk::MemoryPropertyFlags flags) {
    auto memory = physical.getMemoryProperties();
    for (uint32_t i = 0; i < memory.memoryTypeCount; i++)
      if ((bits & (1U << i)) &&
          (memory.memoryTypes[i].propertyFlags & flags) == flags)
        return i;
    Check(false, "memory type");
    return 0;
  }
  void Begin() {
    Vk(command.reset({}), "command reset");
    vk::CommandBufferBeginInfo begin{};
    begin.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
    Vk(command.begin(&begin), "begin");
  }
  void Submit(vk::Semaphore ready = {}, vk::Semaphore done = {}) {
    Vk(command.end(), "end");
    Vk(device.resetFences(1, &fence), "reset fence");
    const vk::PipelineStageFlags stage = vk::PipelineStageFlagBits::eTransfer;
    vk::SubmitInfo submit{};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command;
    if (ready) {
      submit.signalSemaphoreCount = 1;
      submit.pSignalSemaphores = &ready;
    }
    if (done) {
      submit.waitSemaphoreCount = 1;
      submit.pWaitSemaphores = &done;
      submit.pWaitDstStageMask = &stage;
    }
    Vk(queue.submit(1, &submit, fence), "submit");
    Vk(device.waitForFences(1, &fence, VK_TRUE, 10000000000ULL),
       "producer completion");
  }
  void Barrier(vk::Image image, vk::ImageLayout old_layout,
               vk::ImageLayout layout, vk::AccessFlags2 access) {
    vk::ImageMemoryBarrier2 b{};
    b.srcStageMask = vk::PipelineStageFlagBits2::eAllCommands;
    b.srcAccessMask = old_layout == vk::ImageLayout::eUndefined
                          ? vk::AccessFlags2{}
                          : vk::AccessFlagBits2::eTransferWrite;
    b.dstStageMask = vk::PipelineStageFlagBits2::eAllCommands;
    b.dstAccessMask = access;
    b.oldLayout = old_layout;
    b.newLayout = layout;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
    vk::DependencyInfo d{};
    d.imageMemoryBarrierCount = 1;
    d.pImageMemoryBarriers = &b;
    command.pipelineBarrier2(d);
  }
  Image Texture(bool is_overlay, bool premultiplied) {
    Image result;
    vk::ImageCreateInfo create{};
    create.imageType = vk::ImageType::e2D;
    create.extent = {4, 4, 1};
    create.mipLevels = 1;
    create.arrayLayers = 1;
    create.format = vk::Format::eR8G8B8A8Unorm;
    create.tiling = vk::ImageTiling::eOptimal;
    create.usage = vk::ImageUsageFlagBits::eSampled |
                   vk::ImageUsageFlagBits::eTransferDst |
                   vk::ImageUsageFlagBits::eTransferSrc;
    create.samples = vk::SampleCountFlagBits::e1;
    Vk(device.createImage(&create, nullptr, &result.image), "source image");
    auto req = device.getImageMemoryRequirements(result.image);
    vk::MemoryAllocateInfo alloc{};
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex =
        Memory(req.memoryTypeBits, vk::MemoryPropertyFlagBits::eDeviceLocal);
    Vk(device.allocateMemory(&alloc, nullptr, &result.memory), "source memory");
    Vk(device.bindImageMemory(result.image, result.memory, 0), "source bind");
    vk::ImageViewCreateInfo view{};
    view.image = result.image;
    view.viewType = vk::ImageViewType::e2D;
    view.format = create.format;
    view.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
    Vk(device.createImageView(&view, nullptr, &result.view), "source view");
    vk::Buffer staging;
    vk::DeviceMemory staging_memory;
    vk::BufferCreateInfo buffer{};
    buffer.size = 64;
    buffer.usage = vk::BufferUsageFlagBits::eTransferSrc;
    Vk(device.createBuffer(&buffer, nullptr, &staging), "staging");
    req = device.getBufferMemoryRequirements(staging);
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = Memory(
        req.memoryTypeBits, vk::MemoryPropertyFlagBits::eHostVisible |
                                vk::MemoryPropertyFlagBits::eHostCoherent);
    Vk(device.allocateMemory(&alloc, nullptr, &staging_memory),
       "staging memory");
    Vk(device.bindBufferMemory(staging, staging_memory, 0), "staging bind");
    void *bytes = nullptr;
    Vk(device.mapMemory(staging_memory, 0, 64, {}, &bytes), "map staging");
    const std::array<std::array<uint8_t, 4>, 4> colors{{{255, 0, 0, 255},
                                                        {0, 255, 0, 255},
                                                        {0, 0, 255, 255},
                                                        {255, 255, 255, 255}}};
    for (uint32_t y = 0; y < 4; y++)
      for (uint32_t x = 0; x < 4; x++) {
        const auto quadrant = (y >= 2 ? 2 : 0) + (x >= 2 ? 1 : 0);
        auto color = colors[quadrant];
        if (is_overlay)
          color =
              quadrant == 0
                  ? std::array<uint8_t, 4>{uint8_t(premultiplied ? 128 : 255),
                                           uint8_t(premultiplied ? 128 : 255),
                                           0, 128}
                  : std::array<uint8_t, 4>{0, 0, 0, 0};
        std::memcpy(static_cast<uint8_t *>(bytes) + (y * 4 + x) * 4,
                    color.data(), 4);
      }
    device.unmapMemory(staging_memory);
    Begin();
    Barrier(result.image, vk::ImageLayout::eUndefined,
            vk::ImageLayout::eTransferDstOptimal,
            vk::AccessFlagBits2::eTransferWrite);
    vk::BufferImageCopy copy{};
    copy.imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
    copy.imageExtent = {4, 4, 1};
    command.copyBufferToImage(staging, result.image,
                              vk::ImageLayout::eTransferDstOptimal, 1, &copy);
    Barrier(result.image, vk::ImageLayout::eTransferDstOptimal,
            is_overlay ? vk::ImageLayout::eShaderReadOnlyOptimal
                       : vk::ImageLayout::eTransferSrcOptimal,
            is_overlay ? vk::AccessFlagBits2::eShaderRead
                       : vk::AccessFlagBits2::eTransferRead);
    Submit();
    device.destroyBuffer(staging);
    device.freeMemory(staging_memory);
    return result;
  }
  void Capture(GpuPool &pool, const Lease &lease, bool premultiplied = false) {
    Begin();
    auto &layer = premultiplied ? pre_overlay : overlay;
    pool.Record(command, lease, {main.image, main.view, {4, 4}, false},
                {layer.image, layer.view, {4, 4}, premultiplied});
    Submit(pool.Ready(lease.slot),
           lease.recycled ? pool.Done(lease.slot) : vk::Semaphore{});
  }
  ~Fixture() {
    if (device) {
      Vk(device.waitIdle(), "idle");
      for (auto *image : {&main, &overlay, &pre_overlay}) {
        device.destroyImageView(image->view);
        device.destroyImage(image->image);
        device.freeMemory(image->memory);
      }
      device.destroyFence(fence);
      device.destroyCommandPool(commands);
      device.destroy();
    }
    if (instance)
      instance.destroy();
  }
};
#ifdef KYTY_STREAM_TEST_CUDA
class Cuda {
public:
  void *library = dlopen("libcuda.so.1", RTLD_NOW | RTLD_LOCAL);
  CUcontext context{};
#define CUDA_FUNCTION(name)                                                    \
  decltype(&name) p_##name =                                                   \
      reinterpret_cast<decltype(&name)>(dlsym(library, #name))
  CUDA_FUNCTION(cuInit);
  CUDA_FUNCTION(cuDeviceGet);
  CUDA_FUNCTION(cuDeviceGetCount);
  CUDA_FUNCTION(cuDeviceGetUuid_v2);
  using CreateContext = CUresult(CUDAAPI *)(CUcontext *, unsigned int,
                                            CUdevice);
  CreateContext p_cuCtxCreate_v2 =
      reinterpret_cast<CreateContext>(dlsym(library, "cuCtxCreate_v2"));
  CUDA_FUNCTION(cuCtxDestroy_v2);
  CUDA_FUNCTION(cuCtxSynchronize);
  CUDA_FUNCTION(cuImportExternalMemory);
  CUDA_FUNCTION(cuExternalMemoryGetMappedBuffer);
  CUDA_FUNCTION(cuDestroyExternalMemory);
  CUDA_FUNCTION(cuImportExternalSemaphore);
  CUDA_FUNCTION(cuDestroyExternalSemaphore);
  CUDA_FUNCTION(cuWaitExternalSemaphoresAsync);
  CUDA_FUNCTION(cuSignalExternalSemaphoresAsync);
  CUDA_FUNCTION(cuMemAlloc_v2);
  CUDA_FUNCTION(cuMemFree_v2);
  CUDA_FUNCTION(cuMemcpyDtoD_v2);
  CUDA_FUNCTION(cuMemcpyDtoH_v2);
#undef CUDA_FUNCTION
  static void Require(CUresult result, const char *op) {
    if (result != CUDA_SUCCESS) {
      std::fprintf(stderr, "CUDA %s: %d\n", op, int(result));
      std::abort();
    }
  }
  void Init(const PoolDescriptor &descriptor) {
    Check(library && p_cuInit && p_cuCtxCreate_v2 && p_cuImportExternalMemory &&
              p_cuDeviceGetUuid_v2,
          "CUDA external interop symbols");
    Require(p_cuInit(0), "init");
    int count = 0;
    Require(p_cuDeviceGetCount(&count), "device count");
    CUdevice matching = -1;
    for (int i = 0; i < count; i++) {
      CUdevice dev;
      Require(p_cuDeviceGet(&dev, i), "device");
      CUuuid uuid;
      Require(p_cuDeviceGetUuid_v2(&uuid, dev), "UUID");
      char hex[33]{};
      for (int n = 0; n < 16; n++)
        std::snprintf(hex + 2 * n, 3, "%02x", uint8_t(uuid.bytes[n]));
      if (descriptor.gpu_uuid == hex)
        matching = dev;
    }
    Check(matching >= 0, "CUDA/Vulkan UUID match");
    Require(p_cuCtxCreate_v2(&context, 0, matching), "create context");
  }
  ~Cuda() {
    if (context)
      p_cuCtxDestroy_v2(context);
    if (library)
      dlclose(library);
  }
};
struct Imported {
  Cuda &cuda;
  std::array<CUexternalMemory, 3> memory{};
  std::array<CUexternalSemaphore, 3> ready{}, done{};
  std::array<CUdeviceptr, 3> buffer{};
  CUdeviceptr owned{};
  uint64_t size;
  Imported(Cuda &c, GpuPool &pool)
      : cuda(c),
        size(uint64_t(pool.Descriptor().stride) * pool.Descriptor().height) {
    const auto &d = pool.Descriptor();
    auto &fds = pool.Fds();
    for (size_t i = 0; i < 3; i++) {
      CUDA_EXTERNAL_MEMORY_HANDLE_DESC desc{};
      desc.type = CU_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD;
      desc.handle.fd = dup(fds[i * 3]);
      desc.size = d.allocation_sizes[i];
      desc.flags = CUDA_EXTERNAL_MEMORY_DEDICATED;
      Cuda::Require(cuda.p_cuImportExternalMemory(&memory[i], &desc),
                    "import dedicated memory FD");
      CUDA_EXTERNAL_MEMORY_BUFFER_DESC map{};
      map.size = size;
      Cuda::Require(
          cuda.p_cuExternalMemoryGetMappedBuffer(&buffer[i], memory[i], &map),
          "map external buffer");
      CUDA_EXTERNAL_SEMAPHORE_HANDLE_DESC sem{};
      sem.type = CU_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD;
      sem.handle.fd = dup(fds[i * 3 + 1]);
      Cuda::Require(cuda.p_cuImportExternalSemaphore(&ready[i], &sem),
                    "import READY");
      sem.handle.fd = dup(fds[i * 3 + 2]);
      Cuda::Require(cuda.p_cuImportExternalSemaphore(&done[i], &sem),
                    "import DONE");
    }
    Cuda::Require(cuda.p_cuMemAlloc_v2(&owned, size),
                  "owned consumer snapshot");
  }
  std::vector<uint8_t> Consume(uint32_t slot, bool copy) {
    CUDA_EXTERNAL_SEMAPHORE_WAIT_PARAMS wait{};
    Cuda::Require(
        cuda.p_cuWaitExternalSemaphoresAsync(&ready[slot], &wait, 1, nullptr),
        "wait READY");
    if (copy)
      Cuda::Require(cuda.p_cuMemcpyDtoD_v2(owned, buffer[slot], size),
                    "external BGRA D2D snapshot");
    CUDA_EXTERNAL_SEMAPHORE_SIGNAL_PARAMS signal{};
    Cuda::Require(cuda.p_cuSignalExternalSemaphoresAsync(&done[slot], &signal,
                                                         1, nullptr),
                  "signal DONE");
    Cuda::Require(cuda.p_cuCtxSynchronize(), "consumer completion before ACK");
    std::vector<uint8_t> bytes;
    if (copy) {
      bytes.resize(size);
      Cuda::Require(cuda.p_cuMemcpyDtoH_v2(bytes.data(), owned, size),
                    "read owned test snapshot");
    }
    return bytes;
  }
  ~Imported() {
    Cuda::Require(cuda.p_cuCtxSynchronize(), "import cleanup");
    cuda.p_cuMemFree_v2(owned);
    for (size_t i = 0; i < 3; i++) {
      cuda.p_cuMemFree_v2(buffer[i]);
      cuda.p_cuDestroyExternalSemaphore(ready[i]);
      cuda.p_cuDestroyExternalSemaphore(done[i]);
      cuda.p_cuDestroyExternalMemory(memory[i]);
    }
  }
};
void Verify(const PoolDescriptor &d, const std::vector<uint8_t> &bytes) {
  const std::array<std::array<int, 4>, 4> expected{{{0, 128, 255, 255},
                                                    {0, 255, 0, 255},
                                                    {255, 0, 0, 255},
                                                    {255, 255, 255, 255}}};
  for (uint32_t q = 0; q < 4; q++) {
    const uint32_t x = d.width * (q % 2 == 0 ? 1 : 7) / 8,
                   y = d.height * (q < 2 ? 1 : 7) / 8;
    const auto offset = uint64_t(y) * d.stride + x * 4;
    for (size_t ch = 0; ch < 4; ch++)
      if (std::abs(int(bytes[offset + ch]) - expected[q][ch]) > 2) {
        std::fprintf(stderr,
                     "pixel q%u ch%zu=%u expected=%d at %ux%u pitch=%u\n", q,
                     ch, bytes[offset + ch], expected[q][ch], x, y, d.stride);
        Check(false, "BGRA/orientation/alpha overlay pattern");
      }
  }
}
#endif
} // namespace
int main(int argc, char **argv) {
  Fixture fixture;
  if (argc >= 3 && std::strcmp(argv[1], "--serve") == 0) {
    const int seconds = argc >= 4 ? std::atoi(argv[3]) : 30;
    std::string error;
    auto pool = GpuPool::Create(fixture.physical, fixture.device,
                                fixture.family, 1920, 1080, 1, error);
    Check(pool != nullptr, error.c_str());
    auto ring = std::make_shared<LeaseRing>(1);
    Transport transport(
        argv[2], [](const Input *) {}, [](uint64_t) { return true; });
    uint64_t generation = 0, sequence = 0;
    const auto end =
        std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (std::chrono::steady_clock::now() < end) {
      if (auto current = transport.Generation();
          current != 0 && current != generation) {
        if (generation != 0)
          break;
        generation = current;
        transport.SetPool(pool->Descriptor(), pool->Fds(), ring, generation);
      }
      if (transport.Attached())
        if (auto lease = ring->TryAcquire()) {
          fixture.Capture(*pool, *lease);
          transport.Publish(
              {*lease, ++sequence,
               uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count())});
        }
      std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    std::printf("GPU producer served %llu frames\n",
                static_cast<unsigned long long>(sequence));
    return sequence > 0 ? 0 : 1;
  }
#ifdef KYTY_STREAM_TEST_CUDA
  std::string error;
  for (const auto extent :
       {vk::Extent2D{1920, 1080}, vk::Extent2D{2560, 1440}}) {
    auto pool =
        GpuPool::Create(fixture.physical, fixture.device, fixture.family,
                        extent.width, extent.height, 1, error);
    Check(pool != nullptr, error.c_str());
    Cuda cuda;
    cuda.Init(pool->Descriptor());
    Imported imported(cuda, *pool);
    LeaseRing ring(1);
    std::array<Lease, 3> leases;
    for (auto &lease : leases) {
      lease = *ring.TryAcquire();
      fixture.Capture(*pool, lease);
    }
    Check(!ring.TryAcquire(), "three GPU slots saturated without blocking");
    for (const auto &lease : leases) {
      auto bytes = imported.Consume(lease.slot, true);
      Verify(pool->Descriptor(), bytes);
      Check(ring.Release(lease.epoch, lease.slot, lease.sequence),
            "ACK after CUDA completion");
    }
    for (int frame = 0; frame < 9; frame++) {
      auto lease = ring.TryAcquire();
      Check(lease && lease->recycled, "recycled lease requires DONE");
      fixture.Capture(*pool, *lease, frame % 2 != 0);
      const bool copy = frame != 4;
      auto bytes = imported.Consume(lease->slot, copy);
      if (copy)
        Verify(pool->Descriptor(), bytes);
      Check(ring.Release(lease->epoch, lease->slot, lease->sequence),
            "dropped or consumed frame released");
    }
    std::printf("PASS %ux%u pitch=%u UUID=%s: CUDA imports, BGRA+alpha, 3-slot "
                "exhaustion, recycling, skipped frame READY/DONE\n",
                extent.width, extent.height, pool->Descriptor().stride,
                pool->Descriptor().gpu_uuid.c_str());
  }
  return 0;
#else
  std::puts("SKIP: CUDA development headers unavailable; --serve still "
            "supports backend interop");
  return 77;
#endif
}
