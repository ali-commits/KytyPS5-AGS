// fsr2GetPermutationBlobByIndexVK for the FSR2 Vulkan backend. FidelityFX
// builds these blobs into tables with its Windows-only shader compiler; here
// fsr2.cmake compiles each permutation with glslangValidator to
// <dir>/<pass>_<options>.spv, and the blob, with the bindings the backend
// matches by name, is read from that file on first use.
#include "compat.h"
#include "kytyFsr2.h"

#include "ffx_fsr2_shaders_vk.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace KytyFsr2 {

namespace {

// The permutation options compiled per pass (Fs2ShaderPermutationOptionsVK bits
// 0-5). Wave64 only selects a table on AMD and needs no other code; FP16 is
// never selected (Blob below), so the device needs no 16-bit features.
constexpr uint32_t OPTION_MASK = 0x3f;

const char *PassFile(FfxFsr2Pass pass) {
  switch (pass) {
  case FFX_FSR2_PASS_DEPTH_CLIP:
    return "ffx_fsr2_depth_clip_pass";
  case FFX_FSR2_PASS_RECONSTRUCT_PREVIOUS_DEPTH:
    return "ffx_fsr2_reconstruct_previous_depth_pass";
  case FFX_FSR2_PASS_LOCK:
    return "ffx_fsr2_lock_pass";
  case FFX_FSR2_PASS_ACCUMULATE:
  case FFX_FSR2_PASS_ACCUMULATE_SHARPEN:
    return "ffx_fsr2_accumulate_pass";
  case FFX_FSR2_PASS_RCAS:
    return "ffx_fsr2_rcas_pass";
  case FFX_FSR2_PASS_COMPUTE_LUMINANCE_PYRAMID:
    return "ffx_fsr2_compute_luminance_pyramid_pass";
  case FFX_FSR2_PASS_GENERATE_REACTIVE:
    return "ffx_fsr2_autogen_reactive_pass";
  case FFX_FSR2_PASS_TCR_AUTOGENERATE:
    return "ffx_fsr2_tcr_autogen_pass";
  default:
    return nullptr;
  }
}

constexpr std::array<FfxFsr2Pass, 8> PASSES{
    FFX_FSR2_PASS_DEPTH_CLIP,
    FFX_FSR2_PASS_RECONSTRUCT_PREVIOUS_DEPTH,
    FFX_FSR2_PASS_LOCK,
    FFX_FSR2_PASS_ACCUMULATE,
    FFX_FSR2_PASS_RCAS,
    FFX_FSR2_PASS_COMPUTE_LUMINANCE_PYRAMID,
    FFX_FSR2_PASS_GENERATE_REACTIVE,
    FFX_FSR2_PASS_TCR_AUTOGENERATE};

std::filesystem::path FilePath(const std::filesystem::path &dir,
                               FfxFsr2Pass pass, uint32_t options) {
  return dir / (std::string(PassFile(pass)) + "_" +
                std::to_string(options & OPTION_MASK) + ".spv");
}

// A loaded permutation: its code and its bindings, kept for the life of the
// process (the backend keeps the pointers of a Fsr2ShaderBlobVK only while it
// creates the pipeline, but they are cheap).
struct Entry {
  std::vector<uint32_t> code;
  Reflection reflection;
  std::vector<const char *> storage_names;
  std::vector<const char *> sampled_names;
  std::vector<const char *> uniform_names;
};

struct Store {
  std::mutex mutex;
  std::filesystem::path dir;
  std::map<std::pair<int, uint32_t>, std::unique_ptr<Entry>> entries;
};

Store &GetStore() {
  static Store store;
  return store;
}

bool ReadSpirv(const std::filesystem::path &path, std::vector<uint32_t> &out) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file) {
    return false;
  }
  const auto size = static_cast<size_t>(file.tellg());
  if (size == 0 || size % 4 != 0) {
    return false;
  }
  out.resize(size / 4);
  file.seekg(0);
  file.read(reinterpret_cast<char *>(out.data()),
            static_cast<std::streamsize>(size));
  return static_cast<bool>(file);
}

std::vector<const char *> Pointers(const std::vector<std::string> &names) {
  std::vector<const char *> out;
  out.reserve(names.size());
  for (const auto &name : names) {
    out.push_back(name.c_str());
  }
  return out;
}

} // namespace

bool Reflect(const std::vector<uint32_t> &spirv, Reflection &out) {
  constexpr uint32_t MAGIC = 0x07230203;
  constexpr uint32_t OP_NAME = 5;
  constexpr uint32_t OP_TYPE_IMAGE = 25;
  constexpr uint32_t OP_TYPE_STRUCT = 30;
  constexpr uint32_t OP_TYPE_POINTER = 32;
  constexpr uint32_t OP_VARIABLE = 59;
  constexpr uint32_t OP_DECORATE = 71;
  constexpr uint32_t DECORATION_BLOCK = 2;
  constexpr uint32_t DECORATION_BINDING = 33;
  constexpr uint32_t DECORATION_SET = 34;
  constexpr uint32_t STORAGE_UNIFORM_CONST = 0;
  constexpr uint32_t STORAGE_UNIFORM = 2;

  if (spirv.size() < 5 || spirv[0] != MAGIC) {
    return false;
  }
  std::unordered_map<uint32_t, std::string> names;
  std::unordered_map<uint32_t, uint32_t> bindings;
  std::unordered_map<uint32_t, uint32_t> sets;
  std::unordered_map<uint32_t, uint32_t>
      image_sampled;                         // image type -> Sampled operand
  std::unordered_map<uint32_t, bool> blocks; // struct types decorated Block
  std::unordered_map<uint32_t, uint32_t>
      pointees; // pointer type -> pointee type
  std::vector<std::pair<uint32_t, uint32_t>> variables; // (pointer type, id)
  for (size_t at = 5; at < spirv.size();) {
    const uint32_t count = spirv[at] >> 16u;
    const uint32_t opcode = spirv[at] & 0xffffu;
    if (count == 0 || at + count > spirv.size()) {
      return false;
    }
    const uint32_t *w = &spirv[at];
    switch (opcode) {
    case OP_NAME:
      if (count >= 3) {
        const auto *text = reinterpret_cast<const char *>(&w[2]);
        names[w[1]] = std::string(text, strnlen(text, (count - 2) * 4));
      }
      break;
    case OP_DECORATE:
      if (count >= 4 && w[2] == DECORATION_BINDING)
        bindings[w[1]] = w[3];
      if (count >= 4 && w[2] == DECORATION_SET)
        sets[w[1]] = w[3];
      if (count >= 3 && w[2] == DECORATION_BLOCK)
        blocks[w[1]] = true;
      break;
    case OP_TYPE_IMAGE:
      if (count >= 8)
        image_sampled[w[1]] = w[7];
      break;
    case OP_TYPE_POINTER:
      if (count >= 4)
        pointees[w[1]] = w[3];
      break;
    case OP_VARIABLE:
      if (count >= 4 &&
          (w[3] == STORAGE_UNIFORM_CONST || w[3] == STORAGE_UNIFORM)) {
        variables.emplace_back(w[1], w[2]);
      }
      break;
    default:
      break;
    }
    at += count;
  }
  out = {};
  for (const auto &[pointer, id] : variables) {
    // The backend's resources are in set 1; set 0 holds its static samplers.
    if (sets.count(id) == 0 || sets[id] != 1 || bindings.count(id) == 0 ||
        pointees.count(pointer) == 0) {
      continue;
    }
    const auto type = pointees[pointer];
    const auto binding = bindings[id];
    const auto name = names.count(id) != 0 ? names[id] : std::string();
    if (const auto image = image_sampled.find(type);
        image != image_sampled.end()) {
      if (image->second == 2) {
        out.storage_image_names.push_back(name);
        out.storage_image_bindings.push_back(binding);
      } else {
        out.sampled_image_names.push_back(name);
        out.sampled_image_bindings.push_back(binding);
      }
    } else if (blocks.count(type) != 0) {
      out.uniform_buffer_names.push_back(name);
      out.uniform_buffer_bindings.push_back(binding);
    }
  }
  return true;
}

bool SetShaderDirectory(const std::filesystem::path &dir,
                        std::string &missing) {
  for (const auto pass : PASSES) {
    for (uint32_t options = 0; options <= OPTION_MASK; options++) {
      std::error_code ec;
      const auto path = FilePath(dir, pass, options);
      if (!std::filesystem::is_regular_file(path, ec)) {
        missing = path.string();
        return false;
      }
    }
  }
  auto &store = GetStore();
  std::lock_guard<std::mutex> lock(store.mutex);
  store.dir = dir;
  return true;
}

} // namespace KytyFsr2

// FSR2 Vulkan backend (ffx_fsr2_shaders_vk.h). An empty blob makes the backend
// fail the pass, and SetShaderDirectory checked beforehand that every file
// exists.
Fsr2ShaderBlobVK fsr2GetPermutationBlobByIndexVK(FfxFsr2Pass pass,
                                                 uint32_t options) {
  using namespace KytyFsr2;
  Fsr2ShaderBlobVK blob{};
  if (PassFile(pass) == nullptr) {
    return blob;
  }
  auto &store = GetStore();
  std::lock_guard<std::mutex> lock(store.mutex);
  const auto key =
      std::make_pair(static_cast<int>(pass), options & OPTION_MASK);
  auto &entry = store.entries[key];
  if (entry == nullptr) {
    auto loaded = std::make_unique<Entry>();
    const auto path = FilePath(store.dir, pass, options);
    if (!ReadSpirv(path, loaded->code) ||
        !Reflect(loaded->code, loaded->reflection)) {
      std::printf("FSR2: cannot load %s\n", path.string().c_str());
      store.entries.erase(key);
      return blob;
    }
    loaded->storage_names = Pointers(loaded->reflection.storage_image_names);
    loaded->sampled_names = Pointers(loaded->reflection.sampled_image_names);
    loaded->uniform_names = Pointers(loaded->reflection.uniform_buffer_names);
    entry = std::move(loaded);
  }
  const auto &r = entry->reflection;
  blob.data = reinterpret_cast<const uint8_t *>(entry->code.data());
  blob.size = static_cast<uint32_t>(entry->code.size() * 4);
  blob.storageImageCount = static_cast<uint32_t>(r.storage_image_names.size());
  blob.sampledImageCount = static_cast<uint32_t>(r.sampled_image_names.size());
  blob.uniformBufferCount =
      static_cast<uint32_t>(r.uniform_buffer_names.size());
  blob.boundStorageImageNames = entry->storage_names.data();
  blob.boundStorageImageBindings = r.storage_image_bindings.data();
  blob.boundSampledImageNames = entry->sampled_names.data();
  blob.boundSampledImageBindings = r.sampled_image_bindings.data();
  blob.boundUniformBufferNames = entry->uniform_names.data();
  blob.boundUniformBufferBindings = r.uniform_buffer_bindings.data();
  return blob;
}
