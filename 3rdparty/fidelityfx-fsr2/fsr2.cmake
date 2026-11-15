# AMD FidelityFX Super Resolution 2 for the FSR upscaler (src/graphics/host_gpu/renderer/upscaler.h).
# Included from the top-level CMakeLists.txt with KYTY_FSR2_SOURCE_DIR (3rdparty/CMakeLists.txt) and
# KYTY_GLSLANG_VALIDATOR set. Defines the static library `ffx_fsr2` (the FSR2 API, its Vulkan
# backend and this folder's glue) and the target `ffx_fsr2_shaders`, which compiles every shader
# permutation to ${CMAKE_BINARY_DIR}/fsr2/<pass>_<options>.spv; both are installed as fsr2/.
# The FidelityFX sources are used unchanged; licence: LICENSES/FidelityFX-FSR2-MIT.txt.

set(kyty_fsr2_api "${KYTY_FSR2_SOURCE_DIR}/src/ffx-fsr2-api")
set(kyty_fsr2_glue "${CMAKE_CURRENT_LIST_DIR}")

add_library(ffx_fsr2 STATIC
	"${kyty_fsr2_api}/ffx_fsr2.cpp"
	"${kyty_fsr2_api}/ffx_assert.cpp"
	"${kyty_fsr2_api}/vk/ffx_fsr2_vk.cpp"
	"${kyty_fsr2_glue}/shaderBlobs.cpp"
	"${kyty_fsr2_glue}/vulkanFunctions.cpp"
)
target_include_directories(ffx_fsr2 SYSTEM PUBLIC "${kyty_fsr2_api}" "${kyty_fsr2_api}/vk")
target_include_directories(ffx_fsr2 PUBLIC "${kyty_fsr2_glue}")
target_include_directories(ffx_fsr2 PRIVATE "${kyty_fsr2_api}/vk/shaders")
target_link_libraries(ffx_fsr2 PUBLIC Vulkan::Headers)
target_compile_definitions(ffx_fsr2 PUBLIC FFX_GCC VK_NO_PROTOTYPES)
target_compile_features(ffx_fsr2 PRIVATE cxx_std_20)
if(MSVC)
	target_compile_options(ffx_fsr2 PRIVATE "/FI${kyty_fsr2_glue}/compat.h" /W0)
else()
	# std::wstring_convert is deprecated but is what the backend uses.
	target_compile_options(ffx_fsr2 PRIVATE -include "${kyty_fsr2_glue}/compat.h" -w)
endif()

# Shader permutations: Fs2ShaderPermutationOptionsVK bits 0-5 (Lanczos reprojection table, HDR
# colour, low-resolution motion vectors, jittered motion vectors, inverted depth, sharpening) with
# the base defines of FidelityFX's own build. 16-bit and wave64 variants are not built.
set(kyty_fsr2_shader_dir "${kyty_fsr2_api}/shaders")
set(kyty_fsr2_output_dir "${CMAKE_BINARY_DIR}/fsr2")
set(kyty_fsr2_passes
	ffx_fsr2_depth_clip_pass
	ffx_fsr2_reconstruct_previous_depth_pass
	ffx_fsr2_lock_pass
	ffx_fsr2_accumulate_pass
	ffx_fsr2_rcas_pass
	ffx_fsr2_compute_luminance_pyramid_pass
	ffx_fsr2_autogen_reactive_pass
	ffx_fsr2_tcr_autogen_pass
)
set(kyty_fsr2_options
	FFX_FSR2_OPTION_REPROJECT_USE_LANCZOS_TYPE
	FFX_FSR2_OPTION_HDR_COLOR_INPUT
	FFX_FSR2_OPTION_LOW_RESOLUTION_MOTION_VECTORS
	FFX_FSR2_OPTION_JITTERED_MOTION_VECTORS
	FFX_FSR2_OPTION_INVERTED_DEPTH
	FFX_FSR2_OPTION_APPLY_SHARPENING
)
file(GLOB kyty_fsr2_shader_includes "${kyty_fsr2_shader_dir}/*.h")
set(kyty_fsr2_spirv)
foreach(pass IN LISTS kyty_fsr2_passes)
	foreach(options RANGE 63)
		set(defines)
		set(bit 0)
		foreach(option IN LISTS kyty_fsr2_options)
			math(EXPR value "(${options} >> ${bit}) & 1")
			list(APPEND defines "-D${option}=${value}")
			math(EXPR bit "${bit} + 1")
		endforeach()
		set(output "${kyty_fsr2_output_dir}/${pass}_${options}.spv")
		add_custom_command(
			OUTPUT "${output}"
			COMMAND ${CMAKE_COMMAND} -E make_directory "${kyty_fsr2_output_dir}"
			COMMAND "${KYTY_GLSLANG_VALIDATOR}" -V --target-env vulkan1.1 -S comp -e main -Os
			        -DFFX_GLSL=1 -DFFX_GPU=1 -DFFX_HALF=0
			        -DFFX_FSR2_OPTION_UPSAMPLE_SAMPLERS_USE_DATA_HALF=0
			        -DFFX_FSR2_OPTION_ACCUMULATE_SAMPLERS_USE_DATA_HALF=0
			        -DFFX_FSR2_OPTION_REPROJECT_SAMPLERS_USE_DATA_HALF=1
			        -DFFX_FSR2_OPTION_POSTPROCESSLOCKSTATUS_SAMPLERS_USE_DATA_HALF=0
			        -DFFX_FSR2_OPTION_UPSAMPLE_USE_LANCZOS_TYPE=2
			        ${defines} "-I${kyty_fsr2_shader_dir}" -o "${output}"
			        "${kyty_fsr2_shader_dir}/${pass}.glsl"
			DEPENDS "${kyty_fsr2_shader_dir}/${pass}.glsl" ${kyty_fsr2_shader_includes}
			VERBATIM
		)
		list(APPEND kyty_fsr2_spirv "${output}")
	endforeach()
endforeach()
add_custom_target(ffx_fsr2_shaders DEPENDS ${kyty_fsr2_spirv})
install(DIRECTORY "${kyty_fsr2_output_dir}/" DESTINATION fsr2 FILES_MATCHING PATTERN "*.spv")
