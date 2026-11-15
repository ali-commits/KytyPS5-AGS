#include "common/upscalerProfile.h"

#include <algorithm>
#include <charconv>
#include <fmt/format.h>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <nlohmann/json.hpp>
#include <utility>

namespace Common::UpscalerProfile {

namespace {

using Json = nlohmann::json;

// A dword index: an unsigned integer below 4096.
bool Index(const Json& object, const char* key, uint32_t& out, std::string& error) {
	const auto it = object.find(key);
	if (it == object.end() || !it->is_number_unsigned() || it->get<uint64_t>() >= 4096) {
		error = fmt::format("\"{}\" must be an integer from 0 to 4095", key);
		return false;
	}
	out = static_cast<uint32_t>(it->get<uint64_t>());
	return true;
}

bool OptionalIndex(const Json& object, const char* key, std::optional<uint32_t>& out,
                   std::string& error) {
	if (!object.contains(key)) {
		out.reset();
		return true;
	}
	uint32_t value = 0;
	if (!Index(object, key, value, error)) {
		return false;
	}
	out = value;
	return true;
}

bool Object(const Json& parent, const char* key, const Json*& out, std::string& error) {
	const auto it = parent.find(key);
	if (it == parent.end() || !it->is_object()) {
		error = fmt::format("\"{}\" must be an object", key);
		return false;
	}
	out = &*it;
	return true;
}

bool Choice(const Json& object, const char* key, std::initializer_list<const char*> choices,
            std::string& out, std::string& error) {
	const auto it = object.find(key);
	if (it != object.end() && it->is_string()) {
		const auto value = it->get<std::string>();
		if (std::any_of(choices.begin(), choices.end(),
		                [&](const char* choice) { return value == choice; })) {
			out = value;
			return true;
		}
	}
	std::string list;
	for (const auto* choice: choices) {
		list += list.empty() ? "" : ", ";
		list += fmt::format("\"{}\"", choice);
	}
	error = fmt::format("\"{}\" must be one of {}", key, list);
	return false;
}

bool Bool(const Json& object, const char* key, bool& out, std::string& error) {
	const auto it = object.find(key);
	if (it == object.end() || !it->is_boolean()) {
		error = fmt::format("\"{}\" must be true or false", key);
		return false;
	}
	out = it->get<bool>();
	return true;
}

bool Hash(const Json& object, const char* key, uint64_t& out, std::string& error) {
	const auto it = object.find(key);
	if (it != object.end() && it->is_string()) {
		const auto  text = it->get<std::string>();
		const auto* end  = text.data() + text.size();
		if (text.size() == 16 && std::from_chars(text.data(), end, out, 16).ptr == end) {
			return true;
		}
	}
	error = fmt::format("\"{}\" must be 16 hexadecimal digits", key);
	return false;
}

bool ParseDocument(const Json& root, Profile& p, std::string& error) {
	if (!root.is_object()) {
		error = "not a JSON object";
		return false;
	}
	if (const auto it = root.find("format"); it == root.end() || *it != 1) {
		error = "\"format\" must be 1";
		return false;
	}
	if (const auto it = root.find("id"); it != root.end() && it->is_string()) {
		p.title_id = it->get<std::string>();
	} else {
		error = "\"id\" must be the title id";
		return false;
	}
	if (const auto it = root.find("version"); it != root.end()) {
		if (!it->is_string()) {
			error = "\"version\" must be a string";
			return false;
		}
		p.version = it->get<std::string>();
	}

	const Json* pass      = nullptr;
	const Json* constants = nullptr;
	const Json* jitter    = nullptr;
	const Json* motion    = nullptr;
	const Json* depth     = nullptr;
	std::string jitter_units;
	std::string motion_units;
	std::string direction;
	std::string depth_source;
	int         sign = 0;
	if (!Object(root, "pass", pass, error) || !Hash(*pass, "pixel_shader", p.pixel_shader, error) ||
	    !Index(*pass, "color_image", p.color_image, error) ||
	    !Index(*pass, "motion_vector_image", p.motion_vector_image, error) ||
	    !Index(*pass, "constant_buffer", p.constant_buffer, error) ||
	    !Object(root, "constants", constants, error) ||
	    !Index(*constants, "jitter_x", p.jitter_x, error) ||
	    !Index(*constants, "jitter_y", p.jitter_y, error) ||
	    !Index(*constants, "output_width", p.output_width, error) ||
	    !Index(*constants, "output_height", p.output_height, error) ||
	    !Index(*constants, "input_width", p.input_width, error) ||
	    !Index(*constants, "input_height", p.input_height, error) ||
	    !OptionalIndex(*constants, "frame_index", p.frame_index, error) ||
	    !OptionalIndex(*constants, "motion_vectors_enabled", p.motion_vectors_enabled, error) ||
	    !Object(root, "jitter", jitter, error) ||
	    !Choice(*jitter, "units", {"uv", "pixels"}, jitter_units, error) ||
	    !Object(root, "motion_vectors", motion, error) ||
	    !Choice(*motion, "units", {"uv", "pixels"}, motion_units, error) ||
	    !Choice(*motion, "direction", {"previous_to_current", "current_to_previous"}, direction,
	            error) ||
	    !Object(root, "depth", depth, error) ||
	    !Choice(*depth, "source", {"motion_vector_target"}, depth_source, error) ||
	    !Bool(*depth, "inverted", p.depth_inverted, error) || !Bool(root, "hdr", p.hdr, error)) {
		return false;
	}
	if (p.color_image == p.motion_vector_image) {
		error = "\"color_image\" and \"motion_vector_image\" must differ";
		return false;
	}
	if (const auto it = jitter->find("sign");
	    it != jitter->end() && it->is_number_integer() && (*it == 1 || *it == -1)) {
		sign = it->get<int>();
	} else {
		error = "\"sign\" must be 1 or -1";
		return false;
	}
	p.jitter_units      = jitter_units == "uv" ? Units::Uv : Units::Pixels;
	p.jitter_sign       = static_cast<float>(sign);
	p.motion_units      = motion_units == "uv" ? Units::Uv : Units::Pixels;
	p.motion_to_current = direction == "previous_to_current";
	return true;
}

} // namespace

uint32_t Profile::ConstantBytes() const {
	uint32_t last =
	    std::max({jitter_x, jitter_y, output_width, output_height, input_width, input_height});
	if (frame_index.has_value()) {
		last = std::max(last, *frame_index);
	}
	if (motion_vectors_enabled.has_value()) {
		last = std::max(last, *motion_vectors_enabled);
	}
	return (last + 1) * 4;
}

bool Parse(std::string_view text, Profile& out, std::string& error) {
	const auto root = Json::parse(text.begin(), text.end(), nullptr, false);
	if (root.is_discarded()) {
		error = "not valid JSON";
		return false;
	}
	Profile profile;
	if (!ParseDocument(root, profile, error)) {
		return false;
	}
	out = std::move(profile);
	return true;
}

bool Load(const std::filesystem::path& path, Profile& out, std::string& error) {
	std::ifstream file(path, std::ios::binary);
	if (!file) {
		error = "the file cannot be opened";
		return false;
	}
	const std::string text((std::istreambuf_iterator<char>(file)),
	                       std::istreambuf_iterator<char>());
	if (file.bad()) {
		error = "reading the file failed";
		return false;
	}
	return Parse(text, out, error);
}

} // namespace Common::UpscalerProfile
