// The physical-device functions the FSR2 Vulkan backend calls directly
// (compat.h).
#include "compat.h"
#include "kytyFsr2.h"

#include <type_traits>

PFN_vkGetDeviceProcAddr vkGetDeviceProcAddr = nullptr;
PFN_vkGetPhysicalDeviceProperties vkGetPhysicalDeviceProperties = nullptr;
PFN_vkGetPhysicalDeviceProperties2 vkGetPhysicalDeviceProperties2 = nullptr;
PFN_vkGetPhysicalDeviceFeatures2 vkGetPhysicalDeviceFeatures2 = nullptr;
PFN_vkGetPhysicalDeviceMemoryProperties vkGetPhysicalDeviceMemoryProperties =
    nullptr;
PFN_vkEnumerateDeviceExtensionProperties vkEnumerateDeviceExtensionProperties =
    nullptr;

namespace KytyFsr2 {

void LoadVulkanFunctions(PFN_vkGetInstanceProcAddr get_instance_proc_addr,
                         VkInstance instance,
                         PFN_vkGetDeviceProcAddr get_device_proc_addr) {
  const auto load = [&](auto &function, const char *name) {
    function = reinterpret_cast<std::remove_reference_t<decltype(function)>>(
        get_instance_proc_addr(instance, name));
  };
  vkGetDeviceProcAddr = get_device_proc_addr;
  load(vkGetPhysicalDeviceProperties, "vkGetPhysicalDeviceProperties");
  load(vkGetPhysicalDeviceProperties2, "vkGetPhysicalDeviceProperties2");
  load(vkGetPhysicalDeviceFeatures2, "vkGetPhysicalDeviceFeatures2");
  load(vkGetPhysicalDeviceMemoryProperties,
       "vkGetPhysicalDeviceMemoryProperties");
  load(vkEnumerateDeviceExtensionProperties,
       "vkEnumerateDeviceExtensionProperties");
}

} // namespace KytyFsr2
