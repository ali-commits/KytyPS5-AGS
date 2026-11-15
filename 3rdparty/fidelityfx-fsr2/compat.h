// Force-included into the FidelityFX FSR2 sources (fsr2.cmake) so that they
// build unchanged with GCC/Clang on Linux: Windows CRT names they use, and the
// physical-device Vulkan functions their Vulkan backend calls directly
// (vulkanFunctions.cpp; the emulator builds with VK_NO_PROTOTYPES and loads
// Vulkan at run time, so there are no prototypes to link against).
#ifndef KYTY_FSR2_COMPAT_H_
#define KYTY_FSR2_COMPAT_H_

#include <vulkan/vulkan_core.h>

#ifdef __cplusplus
#include <codecvt>
#include <cstddef>
#include <cwchar>
#include <iterator>
#include <locale>

#ifndef _WIN32
#ifndef _countof
#define _countof(array) std::size(array)
#endif

template <size_t N>
inline int wcscpy_s(wchar_t (&destination)[N], const wchar_t *source) {
  std::wcsncpy(destination, source, N - 1);
  destination[N - 1] = L'\0';
  return 0;
}

inline int wcscpy_s(wchar_t *destination, size_t size, const wchar_t *source) {
  std::wcsncpy(destination, source, size - 1);
  destination[size - 1] = L'\0';
  return 0;
}
#endif

extern PFN_vkGetDeviceProcAddr vkGetDeviceProcAddr;
extern PFN_vkGetPhysicalDeviceProperties vkGetPhysicalDeviceProperties;
extern PFN_vkGetPhysicalDeviceProperties2 vkGetPhysicalDeviceProperties2;
extern PFN_vkGetPhysicalDeviceFeatures2 vkGetPhysicalDeviceFeatures2;
extern PFN_vkGetPhysicalDeviceMemoryProperties
    vkGetPhysicalDeviceMemoryProperties;
extern PFN_vkEnumerateDeviceExtensionProperties
    vkEnumerateDeviceExtensionProperties;
#endif

#endif // KYTY_FSR2_COMPAT_H_
