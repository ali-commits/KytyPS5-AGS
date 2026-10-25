#include "graphics/host_gpu/renderer/pipeline/shaderJournalLegacyInt20.h"
#include "graphics/host_gpu/renderer/pipeline/shaderJournalMigration.h"
#include "graphics/shader/shaderCompiler.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <xxhash.h>

// shaderStaticKey.cpp's invariant failures must fail this standalone
// executable.
namespace Common {
int DbgExitIfHandler(const char *expression, const char *, int) {
  std::fprintf(stderr, "Unexpected static-key invariant: %s\n", expression);
  std::abort();
}
void DbgExit(int) { std::abort(); }
} // namespace Common

namespace {
using namespace Libs::Graphics;
namespace Migration = ShaderJournalMigration;
namespace Legacy = ShaderJournalMigration::Int20;
constexpr uint32_t Vendor = 0x10de, Device = 0x2204;
int failures = 0;

void Expect(bool value, const char *what) {
  if (!value) {
    std::fprintf(stderr, "ShaderJournalMigrationTests: %s\n", what);
    failures++;
  }
}

template <typename Info>
std::vector<uint8_t> Bytes(const Info &info, size_t size = sizeof(Info)) {
  const auto *bytes = reinterpret_cast<const uint8_t *>(&info);
  return {bytes, bytes + size};
}

template <typename Info> Info Load(const ShaderJournal::Source &source) {
  Info info{};
  std::memcpy(&info, source.input_info.data(),
              std::min(sizeof(info), source.input_info.size()));
  return info;
}

ShaderJournal::Source Source(ShaderJournal::Kind kind, ShaderType stage,
                             std::vector<uint8_t> input,
                             std::vector<uint32_t> key,
                             std::vector<uint32_t> code = {0xbf810000u}) {
  ShaderJournal::Source source;
  source.kind = kind;
  source.stage = static_cast<uint32_t>(stage);
  source.wave_size = 64;
  source.user_data_count = 4;
  source.code = std::move(code);
  source.code_size = static_cast<uint32_t>(source.code.size());
  source.hash =
      XXH3_64bits(source.code.data(), source.code.size() * sizeof(uint32_t));
  source.input_info = std::move(input);
  source.static_state = std::move(key);
  return source;
}

ShaderJournal::Source Pixel(bool ds = false) {
  Legacy::PixelInputInfo old;
  ShaderPixelInputInfo expected;
  old.input_num = expected.input_num = 2;
  old.interpolator_settings[0] = expected.interpolator_settings[0] = 3;
  old.interpolator_settings[1] = expected.interpolator_settings[1] = 0x100;
  old.scratch_size_dwords = expected.scratch_size_dwords = 24;
  old.ps_system_input_base = expected.ps_system_input_base = 8;
  old.ps_pos_x = expected.ps_pos_x = true;
  old.ps_front_face = expected.ps_front_face = true;
  old.ps_pixel_kill_enable = expected.ps_pixel_kill_enable = true;
  old.ps_sample_shading = expected.ps_sample_shading = true;
  old.ps_execute_on_noop = expected.ps_execute_on_noop = true;
  old.alpha_blend_source = expected.alpha_blend_source =
      ShaderAlphaBlendSource::SourceAlphaOne;
  old.target_output_mode[0] = expected.target_output_mode[0] = 5;
  old.stage.program =
      reinterpret_cast<const ShaderRecompiler::IR::CompiledShaderInfo *>(
          0x1234);
  std::vector<uint32_t> key;
  BuildStageStaticKey(expected, key);
  return Source(ShaderJournal::Kind::Pixel, ShaderType::Pixel, Bytes(old),
                std::move(key),
                ds ? std::vector<uint32_t>{0xd8000000u, 0, 0xbf810000u}
                   : std::vector<uint32_t>{0xbf810000u});
}

ShaderJournal::Source Vertex(bool mesh = false) {
  auto old = std::make_unique<Legacy::VertexInputInfo>();
  auto expected = std::make_unique<ShaderVertexInputInfo>();
  old->logical_stage = expected->logical_stage =
      mesh ? ShaderType::Mesh : ShaderType::Vertex;
  old->resources_num = expected->resources_num = 1;
  old->resources_dst[0].attr_id = expected->resources_dst[0].attr_id = 7;
  old->resources_dst[0].registers_num =
      expected->resources_dst[0].registers_num = 4;
  old->resources[0].fields[3] = expected->resources[0].fields[3] = 0x2345;
  old->start_instance_sgpr = expected->start_instance_sgpr = 11;
  old->fetch_embedded = expected->fetch_embedded = true;
  old->pa_cl_vs_out_cntl = expected->pa_cl_vs_out_cntl = 0x123;
  old->clip_space.enabled = expected->clip_space.enabled = true;
  old->clip_space.scale[0] = expected->clip_space.scale[0] = 0.5f;
  old->buffers[0].addr = expected->buffers[0].addr = 0x12345000;
  old->buffers_num = expected->buffers_num = 1;
  old->stage.resources =
      reinterpret_cast<const ShaderRecompiler::IR::ResourceSnapshot *>(0x5678);
  expected->param_alias_mask = 1u << 4;
  expected->param_alias_source[4] = 2;
  if (mesh) {
    old->mesh.threads_num[0] = expected->mesh.threads_num[0] = 64;
    old->mesh.threads_num[1] = expected->mesh.threads_num[1] = 1;
    old->mesh.threads_num[2] = expected->mesh.threads_num[2] = 1;
    old->mesh.lds_size_dwords = expected->mesh.lds_size_dwords = 1024;
  }
  std::vector<uint32_t> key;
  BuildStageStaticKey(*expected, key);
  return Source(ShaderJournal::Kind::Vertex, old->logical_stage,
                Bytes(*old, Legacy::VertexStoredBytes), std::move(key),
                mesh ? std::vector<uint32_t>{0xd8000000u, 0, 0xbf810000u}
                     : std::vector<uint32_t>{0xbf810000u});
}

ShaderJournal::Source Compute() {
  ShaderComputeInputInfo info;
  info.threads_num[0] = 64;
  info.threads_num[1] = info.threads_num[2] = 1;
  info.lds_size_dwords = 2048;
  info.stage.program =
      reinterpret_cast<const ShaderRecompiler::IR::CompiledShaderInfo *>(
          0x1234);
  std::vector<uint32_t> key;
  BuildStageStaticKey(info, key);
  return Source(ShaderJournal::Kind::Compute, ShaderType::Compute, Bytes(info),
                std::move(key));
}

void TestDsMigration() {
  const auto with_code = [](ShaderJournal::Source source,
                            std::vector<uint32_t> code) {
    source.code = std::move(code);
    source.code_size = static_cast<uint32_t>(source.code.size());
    source.hash =
        XXH3_64bits(source.code.data(), source.code.size() * sizeof(uint32_t));
    return source;
  };
  const auto ds_word = [](uint32_t opcode) {
    return 0xd8000000u | (opcode << 18u);
  };
  for (const auto opcode : {0x35u, 0xb2u, 0xb3u}) {
    for (auto source : {Pixel(), Vertex()}) {
      const auto key = source.static_state;
      auto shuffle = with_code(source, {ds_word(opcode), 0, 0xbf810000u});
      std::string reason;
      Expect(Migration::ConvertInt20Source(shuffle, reason) &&
                 shuffle.static_state == key,
             "VS/PS lane shuffles migrate without inventing LDS metadata");
      auto gds =
          with_code(source, {ds_word(opcode) | (1u << 17u), 0, 0xbf810000u});
      Expect(!Migration::ConvertInt20Source(gds, reason),
             "GDS shuffle encoding remains conservative");
      auto mixed = with_code(
          source, {ds_word(opcode), 0, ds_word(0x36u), 0, 0xbf810000u});
      Expect(!Migration::ConvertInt20Source(mixed, reason),
             "shuffle followed by LDS memory still requires metadata");
    }
  }
  // ADD, READ, ADDTID read/write, and an unknown opcode must never get the
  // lane-only exemption.
  for (const auto opcode : {0x00u, 0x36u, 0xb0u, 0xb1u, 0xffu}) {
    for (auto source : {Pixel(), Vertex()}) {
      auto memory = with_code(source, {ds_word(opcode), 0, 0xbf810000u});
      std::string reason;
      Expect(!Migration::ConvertInt20Source(memory, reason),
             "memory and unknown DS opcodes require metadata");
    }
  }
}

void TestConversion() {
  // Use one vector: iterators from different temporary vectors would not name a
  // range.
  const auto legacy_identity = Migration::Int20Identity(Vendor, Device);
  Expect(std::string(legacy_identity.begin(), legacy_identity.end()) ==
             "kyty shader journal; device 000010de:00002204; input infos 10152 "
             "224 96",
         "exact int20 ABI identity");
  std::string reason;
  auto pixel = Pixel();
  const auto pixel_key = pixel.static_state;
  Expect(Migration::ConvertInt20Source(pixel, reason),
         "pixel conversion succeeds");
  const auto pi = Load<ShaderPixelInputInfo>(pixel);
  Expect(pixel.static_state == pixel_key && pi.lds_size_dwords == 0 &&
             pi.scratch_size_dwords == 24,
         "pixel static identity and scratch preserved with new LDS zero");
  Expect(pi.ps_pos_x && pi.ps_front_face && pi.ps_pixel_kill_enable &&
             pi.ps_sample_shading && pi.ps_execute_on_noop && !pi.ps_pos_y &&
             pi.alpha_blend_source == ShaderAlphaBlendSource::SourceAlphaOne,
         "pixel booleans are translated by field across the ABI shift");
  Expect(pi.stage.program == nullptr && pi.stage.resources == nullptr,
         "pixel pointers cleared");
  auto vertex = Vertex();
  const auto vertex_key = vertex.static_state;
  Expect(Migration::ConvertInt20Source(vertex, reason),
         "vertex aliases convert");
  const auto vi = Load<ShaderVertexInputInfo>(vertex);
  Expect(vertex.static_state == vertex_key && vertex_key[8] == 0x7A11A5u &&
             vi.start_instance_sgpr == 11 && vi.pa_cl_vs_out_cntl == 0x123 &&
             vi.lds_size_dwords == 0 && vi.user_clip_planes == 0,
         "vertex key aliases and shifted fields preserved");
  Expect(vi.stage.program == nullptr && vi.stage.resources == nullptr &&
             vi.buffers[0].addr == 0x12345000,
         "vertex runtime pointers cleared without clearing guest buffer "
         "addresses");
  auto mesh = Vertex(true);
  Expect(Migration::ConvertInt20Source(mesh, reason) &&
             Load<ShaderVertexInputInfo>(mesh).mesh.lds_size_dwords == 1024,
         "mesh DS keeps its known workgroup LDS metadata");
  auto compute = Compute();
  Expect(Migration::ConvertInt20Source(compute, reason) &&
             Load<ShaderComputeInputInfo>(compute).lds_size_dwords == 2048 &&
             Load<ShaderComputeInputInfo>(compute).stage.program == nullptr,
         "compute keeps known LDS and clears pointers");
  auto ds = Pixel(true);
  const auto ds_original = ds.input_info;
  Expect(!Migration::ConvertInt20Source(ds, reason) &&
             ds.input_info == ds_original &&
             reason.find("LDS") != std::string::npos,
         "missing pixel LDS metadata is rejected without changing source");
  auto tess = Vertex();
  auto ti = Load<Legacy::VertexInputInfo>(tess);
  ti.tess.input_control_points = 3;
  tess.input_info = Bytes(ti, Legacy::VertexStoredBytes);
  Expect(!Migration::ConvertInt20Source(tess, reason) &&
             reason.find("tessellation") != std::string::npos,
         "missing tessellation patch metadata is rejected");
  auto mismatch = Pixel();
  mismatch.static_state[0]++;
  Expect(!Migration::ConvertInt20Source(mismatch, reason),
         "mismatched old static key rejected");
  auto bad_hash = Compute();
  bad_hash.hash++;
  Expect(!Migration::ConvertInt20Source(bad_hash, reason),
         "bad code identity rejected");
}

ShaderJournal::Settings Settings(const std::filesystem::path &path,
                                 std::vector<uint8_t> identity) {
  ShaderJournal::Settings settings;
  settings.path = path;
  settings.identity = std::move(identity);
  settings.background_writer = false;
  return settings;
}

void WriteOld(const std::filesystem::path &path) {
  ShaderJournal journal(
      Settings(path, Migration::Int20Identity(Vendor, Device)));
  const std::vector<uint8_t> specialization{1, 2, 3};
  for (auto source : {Pixel(), Vertex(), Compute(), Pixel(true)})
    journal.Record(std::move(source), 7, specialization);
  Expect(journal.Flush(), "old fixture persisted");
}

std::vector<uint8_t> Read(const std::filesystem::path &path) {
  std::ifstream in(path, std::ios::binary);
  return {(std::istreambuf_iterator<char>(in)),
          std::istreambuf_iterator<char>()};
}

void Write(const std::filesystem::path &path,
           const std::vector<uint8_t> &bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char *>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
}

void TestMigration(const std::filesystem::path &directory) {
  const auto path = directory / "valid";
  WriteOld(path);
  const auto original = Read(path);
  const auto result = Migration::MigrateInt20(path, Vendor, Device);
  Expect(result.usable && result.migrated && result.converted_sources == 3 &&
             result.unavailable_sources == 1 && result.entries == 4 &&
             result.unavailable_entries == 1,
         "migration reports converted and unavailable recipes");
  Expect(Read(result.backup) == original, "backup exactly preserves original");
  const auto migrated = Read(path);
  {
    ShaderJournal journal(
        Settings(path, Migration::CurrentIdentity(Vendor, Device)));
    Expect(!journal.GetStats().header_rejected &&
               journal.Entries().size() == 4 && journal.Sources().size() == 4,
           "current journal retains every permutation");
    Expect(journal.Sources()[3].input_info.size() == 8 &&
               journal.Sources()[3].user_data_count == UINT32_MAX,
           "unsafe input persists only as non-replayable placeholder");
    Expect(journal.Entries()[0].push_data_cursor == 7 &&
               journal.Entries()[0].specialization ==
                   std::vector<uint8_t>({1, 2, 3}),
           "cursor and specialization preserved for permutation identity");
  }
  const auto again = Migration::MigrateInt20(path, Vendor, Device);
  Expect(again.usable && !again.migrated && Read(path) == migrated &&
             Read(result.backup) == original,
         "warm repeat makes no further writes");
  // A missing-metadata placeholder must not occupy a real source's recording
  // identity.
  auto live = Pixel(true);
  auto current_pixel = Pixel();
  std::string reason;
  Expect(Migration::ConvertInt20Source(current_pixel, reason),
         "current live fixture input");
  live.input_info = current_pixel.input_info;
  {
    ShaderJournal journal(
        Settings(path, Migration::CurrentIdentity(Vendor, Device)));
    Expect(!journal.HasSource(live),
           "unavailable placeholder does not suppress real source capture");
    journal.Record(live, 8, std::vector<uint8_t>({1, 2, 3}));
    Expect(journal.Flush(), "live variant is appended");
  }
  {
    ShaderJournal journal(
        Settings(path, Migration::CurrentIdentity(Vendor, Device)));
    Expect(Migration::ExcludeRecoveredUnavailable(journal) == 0 &&
               journal.Entries().size() == 5,
           "a different cursor does not claim recovery of an unavailable "
           "permutation");
    Expect(journal.HasSource(live), "real source was recorded independently");
    journal.Record(live, 7, std::vector<uint8_t>({1, 2, 3}));
    Expect(journal.Flush(), "matching live permutation is appended");
  }
  {
    ShaderJournal journal(
        Settings(path, Migration::CurrentIdentity(Vendor, Device)));
    Expect(journal.Entries().size() == 6 &&
               Migration::ExcludeRecoveredUnavailable(journal) == 1 &&
               journal.Entries().size() == 5,
           "warm replay excludes only the unavailable permutation replaced by "
           "a real capture");
    Expect(Migration::ExcludeRecoveredUnavailable(journal) == 0,
           "recovered-placeholder exclusion is idempotent");
  }
  Expect(Read(result.backup) == original,
         "live recapture never changes int20 backup");
  for (const auto name : {"device", "checksum", "tail", "backup", "lock"}) {
    const auto bad = directory / name;
    Write(bad, original);
    if (std::strcmp(name, "checksum") == 0) {
      auto bytes = Read(bad);
      bytes[24] ^= 1;
      Write(bad, bytes);
    } else if (std::strcmp(name, "tail") == 0) {
      auto bytes = Read(bad);
      bytes.push_back(1);
      Write(bad, bytes);
    } else if (std::strcmp(name, "backup") == 0) {
      Write(std::filesystem::path(bad.string() + ".int20.bak"), {1, 2, 3});
    } else if (std::strcmp(name, "lock") == 0) {
      std::filesystem::create_directory(
          std::filesystem::path(bad.string() + ".int21-migrate"));
    }
    const auto before = Read(bad);
    const auto denied = Migration::MigrateInt20(
        bad, Vendor, std::strcmp(name, "device") == 0 ? Device + 1 : Device);
    Expect(!denied.usable && !denied.migrated && Read(bad) == before,
           "refused migration never alters original");
  }
}
} // namespace

int main() {
  const auto directory =
      std::filesystem::current_path() /
      ("shader-journal-migration-test-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directory(directory);
  TestConversion();
  TestDsMigration();
  TestMigration(directory);
  std::error_code ignored;
  std::filesystem::remove_all(directory, ignored);
  if (failures == 0)
    std::printf("ShaderJournalMigrationTests: passed\n");
  return failures == 0 ? 0 : 1;
}
