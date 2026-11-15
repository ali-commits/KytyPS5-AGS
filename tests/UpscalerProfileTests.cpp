// common/upscalerProfile.h: the format 1 parser, and every profile shipped in
// assets/upscalers.
#include "common/upscalerProfile.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

namespace {

using namespace Common::UpscalerProfile;

void Check(bool value, const char *message) {
  if (!value) {
    std::fprintf(stderr, "UpscalerProfileTests: failed: %s\n", message);
    std::abort();
  }
}

const char *const VALID = R"({
  "format": 1, "id": "PPSA99997", "version": "01.000.000", "extra": "ignored",
  "pass": {"pixel_shader": "00000000deadbeef", "color_image": 1, "motion_vector_image": 0,
           "constant_buffer": 2},
  "constants": {"jitter_x": 0, "jitter_y": 1, "output_width": 4, "output_height": 5,
                "input_width": 6, "input_height": 7, "frame_index": 11,
                "motion_vectors_enabled": 13},
  "jitter": {"units": "uv", "sign": -1},
  "motion_vectors": {"units": "pixels", "direction": "current_to_previous"},
  "depth": {"source": "motion_vector_target", "inverted": true},
  "hdr": false})";

std::string Replace(std::string text, const std::string &from,
                    const std::string &to) {
  const auto at = text.find(from);
  Check(at != std::string::npos, "test text contains the replaced part");
  return text.replace(at, from.size(), to);
}

bool Fails(const std::string &text) {
  Profile profile;
  std::string error;
  return !Parse(text, profile, error) && !error.empty();
}

void ParsesAValidProfile() {
  Profile p;
  std::string error;
  Check(Parse(VALID, p, error), "the valid profile parses");
  Check(p.title_id == "PPSA99997" && p.version == "01.000.000",
        "id and version");
  Check(p.pixel_shader == 0xdeadbeefULL, "pixel shader hash");
  Check(p.color_image == 1 && p.motion_vector_image == 0 &&
            p.constant_buffer == 2,
        "bindings");
  Check(p.output_width == 4 && p.input_height == 7, "size dwords");
  Check(p.frame_index == 11U && p.motion_vectors_enabled == 13U,
        "optional dwords");
  Check(p.jitter_units == Units::Uv && p.jitter_sign == -1.0f, "jitter");
  Check(p.motion_units == Units::Pixels && !p.motion_to_current,
        "motion vectors");
  Check(p.depth_inverted && !p.hdr, "depth and hdr");
  Check(p.ConstantBytes() == 56, "constant bytes cover dword 13");
}

void OptionalFieldsMayBeLeftOut() {
  auto text = Replace(VALID, R"("version": "01.000.000", )", "");
  text = Replace(text, R"(, "frame_index": 11,
                "motion_vectors_enabled": 13)",
                 "");
  Profile p;
  std::string error;
  Check(Parse(text, p, error),
        "a profile without version and optional dwords parses");
  Check(p.version.empty() && !p.frame_index && !p.motion_vectors_enabled,
        "absent fields");
  Check(p.ConstantBytes() == 32, "constant bytes cover dword 7");
}

void RejectsInvalidProfiles() {
  Check(Fails("[]"), "not an object");
  Check(Fails("{"), "not JSON");
  Check(Fails(Replace(VALID, R"("format": 1)", R"("format": 2)")),
        "unknown format");
  Check(Fails(Replace(VALID, "00000000deadbeef", "deadbeef")), "short hash");
  Check(Fails(Replace(VALID, "00000000deadbeef", "00000000deadbeeg")),
        "hash not hex");
  Check(Fails(Replace(VALID, R"("color_image": 1)", R"("color_image": 0)")),
        "colour and motion vectors on the same binding");
  Check(Fails(Replace(VALID, R"("jitter_x": 0)", R"("jitter_x": -1)")),
        "negative index");
  Check(Fails(Replace(VALID, R"("jitter_x": 0)", R"("jitter_x": 4096)")),
        "index too large");
  Check(Fails(Replace(VALID, R"("sign": -1)", R"("sign": 2)")), "jitter sign");
  Check(Fails(Replace(VALID, R"("units": "uv")", R"("units": "ndc")")),
        "jitter units");
  Check(Fails(Replace(VALID, "current_to_previous", "sideways")), "direction");
  Check(Fails(Replace(VALID, "motion_vector_target", "bound")), "depth source");
  Check(Fails(Replace(VALID, R"("hdr": false)", R"("hdr": 0)")),
        "hdr not a bool");
  Check(Fails(Replace(VALID, R"("output_width": 4, )", "")),
        "missing required dword");
}

void ShippedProfilesAreValid() {
  int count = 0;
  for (const auto &entry :
       std::filesystem::directory_iterator(KYTY_UPSCALER_PROFILE_DIR)) {
    if (entry.path().extension() != ".json") {
      continue;
    }
    Profile p;
    std::string error;
    if (!Load(entry.path(), p, error)) {
      std::fprintf(stderr, "UpscalerProfileTests: %s: %s\n",
                   entry.path().string().c_str(), error.c_str());
      std::abort();
    }
    const auto stem = entry.path().stem().string();
    Check(stem == p.title_id || stem == p.title_id + "-" + p.version,
          "a shipped profile is named <id>.json or <id>-<version>.json");
    count++;
  }
  Check(count > 0, "assets/upscalers holds profiles");
}

} // namespace

int main() {
  ParsesAValidProfile();
  OptionalFieldsMayBeLeftOut();
  RejectsInvalidProfiles();
  ShippedProfilesAreValid();
  std::puts("UpscalerProfileTests: passed");
  return 0;
}
