// The emulator's side of the FidelityFX FSR2 library (fsr2.cmake): what the
// FSR2 Vulkan backend needs before ffxFsr2GetInterfaceVK and
// ffxFsr2ContextCreate.
#ifndef KYTY_FSR2_H_
#define KYTY_FSR2_H_

#include <vulkan/vulkan_core.h>

#include <filesystem>
#include <string>
#include <vector>

namespace KytyFsr2 {

// The Vulkan functions the backend calls directly, from `instance` (once,
// before any context).
void LoadVulkanFunctions(PFN_vkGetInstanceProcAddr get_instance_proc_addr,
                         VkInstance instance,
                         PFN_vkGetDeviceProcAddr get_device_proc_addr);

// The folder holding the compiled shader permutations (<pass>_<options>.spv,
// installed as fsr2/). False, with the first missing file in `missing`, when
// any permutation is absent: the backend cannot create a pass without its
// shader, so FSR2 must not be used then.
[[nodiscard]] bool SetShaderDirectory(const std::filesystem::path &dir,
                                      std::string &missing);

// Bindings of one compiled permutation, as the backend matches them by name
// (descriptor set 1).
struct Reflection {
  std::vector<std::string> storage_image_names;
  std::vector<uint32_t> storage_image_bindings;
  std::vector<std::string> sampled_image_names;
  std::vector<uint32_t> sampled_image_bindings;
  std::vector<std::string> uniform_buffer_names;
  std::vector<uint32_t> uniform_buffer_bindings;
};
// False for malformed SPIR-V.
[[nodiscard]] bool Reflect(const std::vector<uint32_t> &spirv, Reflection &out);

} // namespace KytyFsr2

#endif // KYTY_FSR2_H_
