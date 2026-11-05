#include "common/fontSearch.h"

#include <cstdio>
#include <cstdlib>

namespace {

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "FontSearchTests: failed: %s\n", message);
		std::abort();
	}
}

} // namespace

int main() {
	namespace fs   = std::filesystem;
	const auto rel = fs::path("3rdparty/tracy/profiler/src/font/Roboto-Regular.ttf");

	// The executable directory comes first, so a cwd outside the install still finds the font.
	const auto paths = Common::FallbackFontCandidates("Roboto-Regular.ttf", "/opt/kyty/",
	                                                  "/home/user", "/src/kyty");
	Check(paths.size() == 5, "exe dir, source root, cwd and two parents");
	Check(paths[0] == fs::path("/opt/kyty/") / rel, "executable directory first");
	Check(paths[1] == fs::path("/src/kyty") / rel, "source root second");
	Check(paths[2] == fs::path("/home/user") / rel, "cwd after that");

	// Missing pieces are skipped instead of producing relative paths.
	const auto no_exe = Common::FallbackFontCandidates("Roboto-Regular.ttf", "", "/cwd", "");
	Check(no_exe.size() == 3 && no_exe[0] == fs::path("/cwd") / rel, "no exe dir, no source root");
	const auto none = Common::FallbackFontCandidates("Roboto-Regular.ttf", "", {}, "");
	Check(none.empty(), "nothing known, nothing to search");
	std::printf("FontSearchTests: all passed\n");
	return 0;
}
