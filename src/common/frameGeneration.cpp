#include "common/frameGeneration.h"

#include <algorithm>
#include <fmt/format.h>

namespace Common::FrameGeneration {

namespace {

bool IsSet(const GetEnv& get_env, const char* name) {
	const char* value = get_env(name);
	return value != nullptr && value[0] != '\0';
}

} // namespace

bool EnvironmentChooses(const GetEnv& get_env) {
	return IsSet(get_env, "DISABLE_LSFGVK") || IsSet(get_env, "LSFGVK_ENV") ||
	       IsSet(get_env, "LSFGVK_PROFILE") || IsSet(get_env, "LSFGVK_CONFIG");
}

std::vector<Variable> LayerEnvironment(const Settings& settings, const GetEnv& get_env) {
	if (EnvironmentChooses(get_env)) {
		return {};
	}
	if (settings.multiplier < 2) {
		return {{"DISABLE_LSFGVK", "1"}};
	}
	const auto flow =
	    std::clamp(settings.flow_scale_percent, MIN_FLOW_SCALE_PERCENT, MAX_FLOW_SCALE_PERCENT);
	return {
	    {"LSFGVK_ENV", "1"},
	    {"LSFGVK_MULTIPLIER", fmt::format("{}", settings.multiplier)},
	    {"LSFGVK_FLOW_SCALE", fmt::format("{:.2f}", flow / 100.0)},
	    {"LSFGVK_PERFORMANCE_MODE", settings.performance_mode ? "1" : "0"},
	};
}

} // namespace Common::FrameGeneration
