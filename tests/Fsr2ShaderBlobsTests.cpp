// 3rdparty/fidelityfx-fsr2/shaderBlobs.cpp: the compiled FSR2 shader permutations are all there,
// and the bindings the FSR2 Vulkan backend matches by name are read from their SPIR-V.
#include <cstddef> // ffx_types.h uses size_t without including it

#include "ffx_fsr2_shaders_vk.h"
#include "kytyFsr2.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace {

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "Fsr2ShaderBlobsTests: failed: %s\n", message);
		std::abort();
	}
}

bool Has(const char* const* names, uint32_t count, const char* name) {
	return std::any_of(names, names + count,
	                   [&](const char* n) { return std::strcmp(n, name) == 0; });
}

void RejectsAnIncompleteFolder() {
	std::string missing;
	Check(!KytyFsr2::SetShaderDirectory(std::filesystem::temp_directory_path() /
	                                        "kyty-fsr2-no-such-folder",
	                                    missing),
	      "a folder without the shaders is refused");
	Check(missing.find("ffx_fsr2_") != std::string::npos, "the missing file is named");
}

void RejectsMalformedSpirv() {
	KytyFsr2::Reflection reflection;
	Check(!KytyFsr2::Reflect({}, reflection), "empty SPIR-V");
	Check(!KytyFsr2::Reflect({0x12345678, 0, 0, 0, 0}, reflection), "wrong magic");
	Check(!KytyFsr2::Reflect({0x07230203, 0, 0, 0, 0, 0xffff0005}, reflection),
	      "an instruction longer than the module");
}

void EveryPassLoads() {
	std::string missing;
	if (!KytyFsr2::SetShaderDirectory(KYTY_FSR2_SHADER_DIR, missing)) {
		std::fprintf(stderr, "Fsr2ShaderBlobsTests: %s is missing\n", missing.c_str());
		std::abort();
	}
	for (uint32_t pass = 0; pass < FFX_FSR2_PASS_COUNT; pass++) {
		for (uint32_t options = 0; options < 0x100; options += 0x11) {
			const auto blob = fsr2GetPermutationBlobByIndexVK(static_cast<FfxFsr2Pass>(pass), options);
			Check(blob.data != nullptr && blob.size > 0 && blob.size % 4 == 0, "a blob per pass");
			Check(blob.storageImageCount > 0, "every pass writes an image");
			Check(blob.uniformBufferCount > 0, "every pass has constants");
		}
	}
	const auto accumulate = fsr2GetPermutationBlobByIndexVK(FFX_FSR2_PASS_ACCUMULATE, 0);
	Check(Has(accumulate.boundUniformBufferNames, accumulate.uniformBufferCount, "cbFSR2"),
	      "accumulate binds cbFSR2");
	Check(Has(accumulate.boundStorageImageNames, accumulate.storageImageCount,
	          "rw_upscaled_output"),
	      "accumulate writes rw_upscaled_output");
	const auto depth_clip = fsr2GetPermutationBlobByIndexVK(FFX_FSR2_PASS_DEPTH_CLIP, 0);
	Check(Has(depth_clip.boundSampledImageNames, depth_clip.sampledImageCount,
	          "r_dilated_motion_vectors"),
	      "depth clip samples r_dilated_motion_vectors");
	const auto rcas = fsr2GetPermutationBlobByIndexVK(FFX_FSR2_PASS_RCAS, 0);
	Check(Has(rcas.boundUniformBufferNames, rcas.uniformBufferCount, "cbRCAS"), "RCAS binds cbRCAS");
	const auto pyramid =
	    fsr2GetPermutationBlobByIndexVK(FFX_FSR2_PASS_COMPUTE_LUMINANCE_PYRAMID, 0);
	Check(Has(pyramid.boundUniformBufferNames, pyramid.uniformBufferCount, "cbSPD"),
	      "the luminance pyramid binds cbSPD");
}

} // namespace

int main() {
	RejectsAnIncompleteFolder();
	RejectsMalformedSpirv();
	EveryPassLoads();
	std::puts("Fsr2ShaderBlobsTests: passed");
	return 0;
}
