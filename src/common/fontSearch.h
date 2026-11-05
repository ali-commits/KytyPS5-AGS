#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace Common {

// Where the emulator looks for a bundled fallback font, in order: next to the executable (so a
// working directory outside the install still works), then the source tree it was built from,
// then the working directory and its two parents.
inline std::vector<std::filesystem::path>
FallbackFontCandidates(const char* file_name, const std::string& executable_dir,
                       const std::filesystem::path& working_dir,
                       const std::filesystem::path& source_root) {
	const auto rel =
	    std::filesystem::path("3rdparty") / "tracy" / "profiler" / "src" / "font" / file_name;
	std::vector<std::filesystem::path> paths;

	if (!executable_dir.empty()) {
		const std::filesystem::path exe(executable_dir);
		paths.push_back(exe / rel);
	}
	if (!source_root.empty()) {
		paths.push_back(source_root / rel);
	}
	if (!working_dir.empty()) {
		paths.push_back(working_dir / rel);
		paths.push_back(working_dir / ".." / rel);
		paths.push_back(working_dir / ".." / ".." / rel);
	}
	return paths;
}

} // namespace Common
