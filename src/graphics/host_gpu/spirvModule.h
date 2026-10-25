#pragma once

#include "graphics/host_gpu/vulkanCommon.h"

namespace Libs::Graphics {

// Hash the exact driver input, including local-array shrinking and diagnostic salt.
// Pipeline journals must not identify transformed modules by their original words.
vk::ShaderModule CompileSPVWithHash(std::span<const uint32_t> code, vk::Device device,
                                    uint64_t& driver_hash);

} // namespace Libs::Graphics
