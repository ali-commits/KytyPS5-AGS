#ifndef EMULATOR_SRC_COMMON_UPSCALERPROFILE_H_
#define EMULATOR_SRC_COMMON_UPSCALERPROFILE_H_

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

// Upscaler profile (--upscaler-profile, _Upscalers/<TITLE>[-<version>].json): describes a game's
// own temporal upscale pass so that an upscaler (Config::GetUpscaler) can replace it. The
// emulator knows no game: everything game-specific is in this file.
//
// The pass is a full-screen draw whose pixel shader is `pass.pixel_shader`. It reads the
// colour to upscale and the motion vectors as pixel-stage images, and one pixel-stage buffer of
// 32-bit constants holds the values the upscaler needs, by dword index. The output is the
// draw's first colour target. Depth is not bound to the pass: it is the depth attachment the
// game rendered with into the motion-vector target ("depth": {"source": "motion_vector_target"}).
//
// Format 1:
//   {"format": 1, "id": "PPSA00000", "version": "01.000.000" (optional, see below),
//    "pass": {"pixel_shader": "<16 hex digits>", "color_image": 1, "motion_vector_image": 0,
//             "constant_buffer": 0},
//    "constants": {"jitter_x": 0, "jitter_y": 1, "output_width": 4, "output_height": 5,
//                  "input_width": 6, "input_height": 7,
//                  "frame_index": 11 (optional), "motion_vectors_enabled": 13 (optional)},
//    "jitter": {"units": "uv" | "pixels", "sign": 1 | -1},
//    "motion_vectors": {"units": "uv" | "pixels",
//                       "direction": "previous_to_current" | "current_to_previous"},
//    "depth": {"source": "motion_vector_target", "inverted": true | false},
//    "hdr": true | false}
// Jitter is the current frame's sub-pixel offset as floats; sizes, frame index and the motion
// vector switch are unsigned integers. A frame whose frame index does not follow the previous
// one resets the upscaler's history; a frame whose motion-vector switch is 0 keeps the game's pass.
// Unknown keys are ignored. "id" and "version" follow the cheat file rule (README, "Cheat
// files"): a file without "version" applies to every version of the game, and a version whose
// shader differs simply never matches `pixel_shader`, so the game's pass runs.
namespace Common::UpscalerProfile {

enum class Units : uint8_t { Uv, Pixels };

struct Profile {
	std::string title_id;
	std::string version; // empty: every version

	uint64_t pixel_shader        = 0;
	uint32_t color_image         = 0; // pixel-stage image binding indices
	uint32_t motion_vector_image = 0;
	uint32_t constant_buffer     = 0; // pixel-stage buffer binding index

	// Dword indices in the constant buffer.
	uint32_t                jitter_x      = 0;
	uint32_t                jitter_y      = 0;
	uint32_t                output_width  = 0;
	uint32_t                output_height = 0;
	uint32_t                input_width   = 0;
	uint32_t                input_height  = 0;
	std::optional<uint32_t> frame_index;
	std::optional<uint32_t> motion_vectors_enabled;

	Units jitter_units      = Units::Uv;
	float jitter_sign       = 1.0f;
	Units motion_units      = Units::Uv;
	bool  motion_to_current = true; // previous_to_current
	bool  depth_inverted    = false;
	bool  hdr               = false;

	// The smallest constant buffer, in bytes, that holds every dword above.
	[[nodiscard]] uint32_t ConstantBytes() const;
};

// False with `error` set when `text` is not a valid format 1 profile.
[[nodiscard]] bool Parse(std::string_view text, Profile& out, std::string& error);
[[nodiscard]] bool Load(const std::filesystem::path& path, Profile& out, std::string& error);

} // namespace Common::UpscalerProfile

#endif // EMULATOR_SRC_COMMON_UPSCALERPROFILE_H_
