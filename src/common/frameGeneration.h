#ifndef EMULATOR_SRC_COMMON_FRAMEGENERATION_H_
#define EMULATOR_SRC_COMMON_FRAMEGENERATION_H_

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

// Frame generation through lsfg-vk (Linux), a Vulkan layer that inserts generated frames between
// the game's own at present time. The layer is implicit: once installed it loads into every
// Vulkan application and takes its settings from a profile in its conf.toml or, with LSFGVK_ENV,
// from the environment. The emulator does not load or call it; it only sets those variables for
// its own process before the Vulkan instance is created (window.cpp).
//
// Off sets DISABLE_LSFGVK, so a conf.toml profile matching kyty_emulator is not applied either;
// a multiplier sets LSFGVK_ENV and the profile values. When the environment already chooses
// (DISABLE_LSFGVK, LSFGVK_ENV, LSFGVK_PROFILE or LSFGVK_CONFIG is set), it is left as it is.
//
// lsfg-vk 2.0 needs Lossless Scaling's shaders (the Steam application); it finds them itself.

namespace Common::FrameGeneration {

constexpr uint32_t MIN_FLOW_SCALE_PERCENT = 25;
constexpr uint32_t MAX_FLOW_SCALE_PERCENT = 100;

// The layer's name in vkEnumerateInstanceLayerProperties.
constexpr char LAYER_NAME[] = "VK_LAYER_LSFGVK_frame_generation";

struct Settings {
	uint32_t multiplier         = 1;   // presented frames per game frame; 1 is off, else 2-4
	uint32_t flow_scale_percent = 100; // optical flow resolution, 25-100
	bool     performance_mode   = false;
};

using Variable = std::pair<std::string, std::string>;
using GetEnv   = std::function<const char*(const char*)>;

// True when the user's environment already decides whether and how lsfg-vk runs.
bool EnvironmentChooses(const GetEnv& get_env);

// The variables to set for `settings`, or none when EnvironmentChooses. A flow scale outside
// 25-100 is clamped.
std::vector<Variable> LayerEnvironment(const Settings& settings, const GetEnv& get_env);

} // namespace Common::FrameGeneration

#endif /* EMULATOR_SRC_COMMON_FRAMEGENERATION_H_ */
