// common/frameGeneration.h: the lsfg-vk variables for each setting, and that a choice made in
// the environment is left alone.
#include "common/frameGeneration.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

namespace {

using namespace Common::FrameGeneration;

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "FrameGenerationTests: failed: %s\n", message);
		std::abort();
	}
}

GetEnv Env(std::map<std::string, std::string> values) {
	return [values = std::move(values)](const char* name) -> const char* {
		const auto it = values.find(name);
		return it != values.end() ? it->second.c_str() : nullptr;
	};
}

std::map<std::string, std::string> Vars(const Settings& settings, const GetEnv& env) {
	std::map<std::string, std::string> out;
	for (const auto& [name, value]: LayerEnvironment(settings, env)) {
		Check(out.emplace(name, value).second, "each variable once");
	}
	return out;
}

void OffDisablesTheLayer() {
	const auto vars = Vars(Settings {}, Env({}));
	Check(vars.size() == 1 && vars.at("DISABLE_LSFGVK") == "1", "Off sets only DISABLE_LSFGVK");
}

void AMultiplierConfiguresTheLayer() {
	const auto vars = Vars({.multiplier = 2, .flow_scale_percent = 80, .performance_mode = false},
	                       Env({}));
	Check(vars.size() == 4, "four variables");
	Check(vars.at("LSFGVK_ENV") == "1", "the environment profile is used");
	Check(vars.at("LSFGVK_MULTIPLIER") == "2", "multiplier");
	Check(vars.at("LSFGVK_FLOW_SCALE") == "0.80", "flow scale as a fraction");
	Check(vars.at("LSFGVK_PERFORMANCE_MODE") == "0", "performance mode off");

	const auto quadruple =
	    Vars({.multiplier = 4, .flow_scale_percent = 100, .performance_mode = true}, Env({}));
	Check(quadruple.at("LSFGVK_MULTIPLIER") == "4", "multiplier 4");
	Check(quadruple.at("LSFGVK_FLOW_SCALE") == "1.00", "full flow scale");
	Check(quadruple.at("LSFGVK_PERFORMANCE_MODE") == "1", "performance mode on");
}

void FlowScaleIsClamped() {
	Check(Vars({.multiplier = 3, .flow_scale_percent = 5}, Env({})).at("LSFGVK_FLOW_SCALE") ==
	          "0.25",
	      "lsfg-vk's minimum");
	Check(Vars({.multiplier = 3, .flow_scale_percent = 400}, Env({})).at("LSFGVK_FLOW_SCALE") ==
	          "1.00",
	      "lsfg-vk's maximum");
}

void TheEnvironmentWins() {
	for (const char* name: {"DISABLE_LSFGVK", "LSFGVK_ENV", "LSFGVK_PROFILE", "LSFGVK_CONFIG"}) {
		const auto env = Env({{name, "1"}});
		Check(EnvironmentChooses(env), name);
		Check(LayerEnvironment({.multiplier = 2}, env).empty(), "a multiplier changes nothing");
		Check(LayerEnvironment(Settings {}, env).empty(), "Off changes nothing");
	}
	// Variables that do not choose by themselves (lsfg-vk reads them only with LSFGVK_ENV), and
	// empty ones, do not count.
	const auto env = Env({{"LSFGVK_MULTIPLIER", "3"}, {"LSFGVK_PROFILE", ""}});
	Check(!EnvironmentChooses(env), "LSFGVK_MULTIPLIER alone or an empty variable");
	Check(Vars({.multiplier = 2}, env).at("LSFGVK_MULTIPLIER") == "2", "the setting applies");
}

} // namespace

int main() {
	OffDisablesTheLayer();
	AMultiplierConfiguresTheLayer();
	FlowScaleIsClamped();
	TheEnvironmentWins();
	std::puts("FrameGenerationTests: passed");
	return 0;
}
