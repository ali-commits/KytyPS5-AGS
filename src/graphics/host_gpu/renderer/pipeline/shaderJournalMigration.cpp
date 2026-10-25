#include "graphics/host_gpu/renderer/pipeline/shaderJournalMigration.h"

#include "graphics/host_gpu/renderer/pipeline/shaderJournalLegacyInt20.h"
#include "graphics/shader/shaderCompiler.h"

#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <system_error>
#include <unordered_map>
#include <xxhash.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace Libs::Graphics::ShaderJournalMigration {
namespace {

constexpr uint64_t MaxJournalBytes = 256ull * 1024 * 1024;

std::vector<uint8_t> Identity(uint32_t vendor, uint32_t device, size_t vertex, size_t pixel,
                              size_t compute) {
	char text[192];
	std::snprintf(text, sizeof(text),
	              "kyty shader journal; device %08x:%08x; input infos %zu %zu %zu", vendor, device,
	              vertex, pixel, compute);
	return {text, text + std::strlen(text)};
}

bool ReadBytes(const std::filesystem::path& path, std::vector<uint8_t>& bytes) {
	std::error_code error;
	const auto      size = std::filesystem::file_size(path, error);
	if (error || size > MaxJournalBytes) return false;
	std::ifstream in(path, std::ios::binary);
	if (!in) return false;
	bytes.resize(static_cast<size_t>(size));
	return bytes.empty() || static_cast<bool>(in.read(reinterpret_cast<char*>(bytes.data()),
	                                                  static_cast<std::streamsize>(bytes.size())));
}

bool HeaderIdentity(std::span<const uint8_t> bytes, std::vector<uint8_t>& identity) {
	if (bytes.size() < 24 || std::memcmp(bytes.data(), ShaderJournal::FileMagic, 8) != 0)
		return false;
	uint32_t version = 0, size = 0;
	std::memcpy(&version, bytes.data() + 8, 4);
	std::memcpy(&size, bytes.data() + 12, 4);
	if (version != ShaderJournal::FormatVersion || size > bytes.size() - 24) return false;
	uint64_t checksum = 0;
	std::memcpy(&checksum, bytes.data() + 16 + size, 8);
	if (checksum != XXH3_64bits(bytes.data(), 16 + size)) return false;
	identity.assign(bytes.begin() + 16, bytes.begin() + 16 + size);
	return true;
}

// Conservative word scan: a literal may resemble DS, in which case the recipe is unavailable
// rather than assigned an invented LDS allocation. Mesh/compute already stored workgroup LDS.
// DecodeDs (MemoryOps.cpp) extracts OP from bits 18..25 and GDS from bit 17. Only the three
// proven lane shuffles are exempt: the translator emits Swizzle/Permute/BpermuteU32, with no
// shared-memory access or allocation. Every memory, unknown, or GDS opcode remains conservative.
bool MayRequireLdsMetadata(std::span<const uint32_t> code) {
	return std::ranges::any_of(code, [](uint32_t word) {
		if (word >> 26u != 0x36u) return false;
		if ((word & (1u << 17u)) != 0) return true;
		const auto opcode = (word >> 18u) & 0xffu;
		return opcode != 0x35u && opcode != 0xb2u && opcode != 0xb3u;
	});
}

bool RestoreAliases(ShaderVertexInputInfo& info, std::span<const uint32_t> key) {
	constexpr size_t tag = 8;
	if (key.size() <= tag || key[tag] != 0x7A11A5u) return true;
	if (key.size() <= tag + 1) return false;
	const auto mask = key[tag + 1];
	if (mask == 0 || key.size() <= tag + 2 + std::popcount(mask)) return false;
	size_t word = tag + 2;
	for (uint32_t index = 0; index < 32; index++) {
		if ((mask & (1u << index)) == 0) continue;
		if (key[word] >= 32) return false;
		info.param_alias_source[index] = key[word++];
	}
	info.param_alias_mask = mask;
	return true;
}

template <typename Info>
bool Store(ShaderJournal::Source& source, Info& info, std::string& reason) {
	info.stage = {};
	std::vector<uint32_t> key;
	BuildStageStaticKey(info, key);
	if (key != source.static_state) {
		reason = "stored input fields do not reconstruct the int20 static key";
		return false;
	}
	const auto* bytes = reinterpret_cast<const uint8_t*>(&info);
	source.input_info.assign(bytes, bytes + ShaderStoredInputInfoSize<Info>());
	// Equal by construction: old pipeline-journal permutation identities remain valid.
	source.static_state = std::move(key);
	return true;
}

constexpr uint32_t UnavailableMarker = 0x49323055u; // int20 unavailable

void MarkUnavailable(ShaderJournal::Source& source) {
	const uint32_t words[] = {UnavailableMarker, source.user_data_count};
	const auto*    bytes   = reinterpret_cast<const uint8_t*>(words);
	source.input_info.assign(bytes, bytes + sizeof(words));
	// Runtime sources have at most 40 user SGPRs. This separate identity cannot poison
	// HasSource or permutation dedup when the real shader is captured in a later gameplay run.
	source.user_data_count = UINT32_MAX;
}

bool OriginalUnavailableIdentity(const ShaderJournal::Source& source,
                                 ShaderJournal::Source&       identity) {
	if (source.user_data_count != UINT32_MAX || source.input_info.size() != 8) return false;
	uint32_t words[2];
	std::memcpy(words, source.input_info.data(), sizeof(words));
	if (words[0] != UnavailableMarker || words[1] > 40) return false;
	identity                 = source;
	identity.user_data_count = words[1];
	return true;
}

bool CurrentInputLayout(const ShaderJournal::Source& source) {
	switch (source.kind) {
		case ShaderJournal::Kind::Vertex:
			return source.input_info.size() == ShaderStoredInputInfoSize<ShaderVertexInputInfo>();
		case ShaderJournal::Kind::Pixel:
			return source.input_info.size() == ShaderStoredInputInfoSize<ShaderPixelInputInfo>();
		case ShaderJournal::Kind::Compute:
			return source.input_info.size() == ShaderStoredInputInfoSize<ShaderComputeInputInfo>();
	}
	return false;
}

bool SameSourceIdentity(const ShaderJournal::Source& a, const ShaderJournal::Source& b) {
	return a.stage == b.stage && a.kind == b.kind && a.hash == b.hash &&
	       a.user_data_count == b.user_data_count && a.code_size == b.code_size &&
	       a.wave_size == b.wave_size && a.user_data_base == b.user_data_base &&
	       a.plain_mip_stats_variant == b.plain_mip_stats_variant &&
	       a.static_state == b.static_state;
}

ShaderJournal::Settings Settings(const std::filesystem::path& path, std::vector<uint8_t> identity) {
	ShaderJournal::Settings settings;
	settings.path              = path;
	settings.identity          = std::move(identity);
	settings.background_writer = false;
	return settings;
}

bool Replace(const std::filesystem::path& from, const std::filesystem::path& to,
             std::error_code& error) {
#if defined(_WIN32)
	if (MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
		return true;
	error = std::error_code(static_cast<int>(GetLastError()), std::system_category());
	return false;
#else
	std::filesystem::rename(from, to, error);
	return !error;
#endif
}

} // namespace

std::vector<uint8_t> CurrentIdentity(uint32_t vendor, uint32_t device) {
	return Identity(vendor, device, ShaderStoredInputInfoSize<ShaderVertexInputInfo>(),
	                ShaderStoredInputInfoSize<ShaderPixelInputInfo>(),
	                ShaderStoredInputInfoSize<ShaderComputeInputInfo>());
}

std::vector<uint8_t> Int20Identity(uint32_t vendor, uint32_t device) {
	return Identity(vendor, device, Int20::VertexStoredBytes, sizeof(Int20::PixelInputInfo),
	                sizeof(ShaderComputeInputInfo));
}

bool ConvertInt20Source(ShaderJournal::Source& source, std::string& reason) {
	reason.clear();
	const auto fail = [&](const char* why) {
		reason = why;
		return false;
	};
	if (source.code.empty() || source.code.size() != source.code_size ||
	    source.user_data_count > 40)
		return fail("invalid code or user-data count");
	const uint64_t front = XXH3_64bits(source.code.data(), source.code.size() * sizeof(uint32_t));
	const uint64_t halves[] = {
	    front, XXH3_64bits(source.back_code.data(), source.back_code.size() * sizeof(uint32_t))};
	const auto hash = source.back_code.empty() ? front : XXH3_64bits(halves, sizeof(halves));
	if (hash != source.hash) return fail("code hash mismatch");
	switch (source.kind) {
		case ShaderJournal::Kind::Vertex: {
			if (source.input_info.size() != Int20::VertexStoredBytes)
				return fail("unknown int20 vertex layout");
			auto old = std::make_unique<Int20::VertexInputInfo>();
			std::memcpy(old.get(), source.input_info.data(), Int20::VertexStoredBytes);
			if (source.stage != static_cast<uint32_t>(old->logical_stage) ||
			    old->resources_num < 0 || old->resources_num > 32 || old->buffers_num < 0 ||
			    old->buffers_num > 32)
				return fail("invalid vertex stage or resource counts");
			if (old->tess.input_control_points != 0 || (old->logical_stage != ShaderType::Vertex &&
			                                            old->logical_stage != ShaderType::Mesh))
				return fail("int20 lacks tessellation patch metadata");
			if (old->logical_stage != ShaderType::Mesh &&
			    (MayRequireLdsMetadata(source.code) || MayRequireLdsMetadata(source.back_code)))
				return fail("int20 lacks vertex LDS allocation metadata");
			auto info = std::make_unique<ShaderVertexInputInfo>();
			std::copy(std::begin(old->resources), std::end(old->resources),
			          std::begin(info->resources));
			std::copy(std::begin(old->resources_dst), std::end(old->resources_dst),
			          std::begin(info->resources_dst));
			std::copy(std::begin(old->buffers), std::end(old->buffers), std::begin(info->buffers));
#define COPY(field) info->field = old->field
			COPY(logical_stage);
			COPY(resources_num);
			COPY(fetch_attrib_reg);
			COPY(fetch_buffer_reg);
			COPY(buffers_num);
			COPY(wave_size);
			COPY(scratch_size_dwords);
			COPY(pa_cl_vs_out_cntl);
			COPY(clip_space);
			COPY(mesh);
			COPY(fetch_external);
			COPY(fetch_embedded);
			COPY(start_instance_sgpr);
#undef COPY
			if (!RestoreAliases(*info, source.static_state))
				return fail("invalid vertex parameter aliases");
			return Store(source, *info, reason);
		}
		case ShaderJournal::Kind::Pixel: {
			if (source.input_info.size() != sizeof(Int20::PixelInputInfo) ||
			    source.stage != static_cast<uint32_t>(ShaderType::Pixel))
				return fail("unknown int20 pixel layout or stage");
			Int20::PixelInputInfo old;
			std::memcpy(&old, source.input_info.data(), sizeof(old));
			if (old.input_num > 32) return fail("invalid interpolator count");
			if (MayRequireLdsMetadata(source.code) || MayRequireLdsMetadata(source.back_code))
				return fail("int20 lacks pixel LDS allocation metadata");
			ShaderPixelInputInfo info {};
			std::copy(std::begin(old.interpolator_settings), std::end(old.interpolator_settings),
			          std::begin(info.interpolator_settings));
			std::copy(std::begin(old.target_output_mode), std::end(old.target_output_mode),
			          std::begin(info.target_output_mode));
#define COPY(field) info.field = old.field
			COPY(input_num);
			COPY(wave_size);
			COPY(ps_system_input_base);
			COPY(custom_interpolation_mask);
			COPY(ps_perspective_center_vgpr);
			COPY(ps_perspective_centroid_vgpr);
			COPY(ps_perspective_sample_vgpr);
			COPY(ps_linear_sample_vgpr);
			COPY(ps_linear_center_vgpr);
			COPY(ps_linear_centroid_vgpr);
			COPY(target_shader_mask);
			COPY(target_export_mapping);
			COPY(scratch_size_dwords);
			COPY(ps_pos_x);
			COPY(ps_pos_y);
			COPY(ps_pos_z);
			COPY(ps_pos_w);
			COPY(ps_front_face);
			COPY(ps_ancillary);
			COPY(ps_no_perspective);
			COPY(ps_pixel_kill_enable);
			COPY(ps_depth_export_enable);
			COPY(ps_sample_mask_export_enable);
			COPY(ps_sample_shading);
			COPY(dual_source_blending);
			COPY(alpha_blend_source);
			COPY(ps_early_z);
			COPY(ps_execute_on_noop);
#undef COPY
			return Store(source, info, reason);
		}
		case ShaderJournal::Kind::Compute: {
			if (source.input_info.size() != sizeof(ShaderComputeInputInfo) ||
			    source.stage != static_cast<uint32_t>(ShaderType::Compute))
				return fail("unknown int20 compute layout or stage");
			ShaderComputeInputInfo info {};
			std::memcpy(&info, source.input_info.data(), sizeof(info));
			return Store(source, info, reason);
		}
	}
	return fail("unknown shader kind");
}

uint64_t ExcludeRecoveredUnavailable(ShaderJournal& journal) {
	const auto& sources = journal.Sources();
	const auto& entries = journal.Entries();
	// Index only usable candidates; matching each unavailable entry must not rescan the
	// whole journal (which can contain tens of thousands of permutations).
	const auto digest = [](const ShaderJournal::Source& source, const ShaderJournal::Entry& entry) {
		const uint64_t words[] = {source.stage,          static_cast<uint64_t>(source.kind),
		                          source.hash,           source.user_data_count,
		                          source.code_size,      source.wave_size,
		                          source.user_data_base, source.plain_mip_stats_variant,
		                          entry.push_data_cursor};
		const auto source_hash = XXH3_64bits_withSeed(source.static_state.data(),
		                                              source.static_state.size() * sizeof(uint32_t),
		                                              XXH3_64bits(words, sizeof(words)));
		return XXH3_64bits_withSeed(entry.specialization.data(), entry.specialization.size(),
		                            source_hash);
	};
	std::unordered_multimap<uint64_t, const ShaderJournal::Entry*> candidates;
	for (const auto& entry: entries) {
		const auto& source = sources[entry.source];
		if (CurrentInputLayout(source)) candidates.emplace(digest(source, entry), &entry);
	}
	if (candidates.empty()) return 0;
	std::vector<uint32_t> excluded;
	for (size_t i = 0; i < entries.size(); i++) {
		const auto&           unavailable = entries[i];
		ShaderJournal::Source original;
		if (!OriginalUnavailableIdentity(sources[unavailable.source], original)) continue;
		const auto range = candidates.equal_range(digest(original, unavailable));
		for (auto found = range.first; found != range.second; ++found) {
			const auto& candidate = *found->second;
			const auto& source    = sources[candidate.source];
			if (candidate.push_data_cursor == unavailable.push_data_cursor &&
			    candidate.specialization == unavailable.specialization &&
			    SameSourceIdentity(source, original)) {
				excluded.push_back(static_cast<uint32_t>(i));
				break;
			}
		}
	}
	journal.ExcludeReplayEntries(excluded);
	return excluded.size();
}

Result MigrateInt20(const std::filesystem::path& path, uint32_t vendor, uint32_t device,
                    const std::function<void(const std::string&)>& log) {
	Result     result;
	const auto fail = [&](std::string reason) {
		result.usable = false;
		result.error  = std::move(reason);
		if (log) log("Shader journal migration: " + result.error + "; original preserved");
		return result;
	};
	std::error_code error;
	if (!std::filesystem::exists(path, error) && !error) return result;
	std::vector<uint8_t> original, identity;
	if (!ReadBytes(path, original) || !HeaderIdentity(original, identity))
		return fail("invalid or unreadable header");
	const auto current = CurrentIdentity(vendor, device);
	if (identity == current) return result;
	if (identity != Int20Identity(vendor, device)) return fail("unsupported ABI or another device");

	// A directory is an exclusive migration claim, and keeps the temporary file on this volume.
	const auto work = std::filesystem::path(path.string() + ".int21-migrate");
	if (!std::filesystem::create_directory(work, error))
		return fail("cannot acquire migration directory: " + error.message());
	struct Cleanup {
		std::filesystem::path path;
		~Cleanup() {
			std::error_code ignored;
			std::filesystem::remove_all(path, ignored);
		}
	} cleanup {work};
	const auto    temporary = work / "journal";
	ShaderJournal previous(Settings(path, identity));
	const auto    old_stats = previous.GetStats();
	if (old_stats.header_rejected || old_stats.damaged_bytes != 0)
		return fail("int20 journal has a damaged record tail");
	if (previous.Entries().empty()) return fail("int20 journal has no permutations");
	std::vector<ShaderJournal::Source> sources = previous.Sources();
	std::vector<bool>                  unavailable(sources.size());
	for (size_t i = 0; i < sources.size(); i++) {
		std::string reason;
		if (ConvertInt20Source(sources[i], reason)) {
			result.converted_sources++;
		} else {
			unavailable[i] = true;
			result.unavailable_sources++;
			// Keep the recipe counted under a reserved identity until a live capture repairs it.
			MarkUnavailable(sources[i]);
			if (log)
				log("Shader journal migration: source " + std::to_string(i) +
				    " unavailable: " + reason);
		}
	}
	uint64_t written_entries = 0, written_sources = 0;
	{
		ShaderJournal replacement(Settings(temporary, current));
		for (const auto& entry: previous.Entries()) {
			if (entry.source >= sources.size()) return fail("invalid source reference");
			replacement.Record(sources[entry.source], entry.push_data_cursor, entry.specialization);
			result.entries++;
			result.unavailable_entries += unavailable[entry.source];
		}
		if (!replacement.Flush()) return fail("temporary journal write failed");
		const auto stats = replacement.GetStats();
		if (stats.over_capacity || stats.write_failures != 0)
			return fail("temporary journal exceeded capacity or failed to write");
		written_entries = stats.recorded_entries;
		written_sources = stats.recorded_sources;
		if (written_entries != result.entries)
			return fail("migration could not preserve every permutation");
	}
	{
		ShaderJournal verified(Settings(temporary, current));
		const auto    stats = verified.GetStats();
		if (stats.header_rejected || stats.damaged_bytes != 0 ||
		    verified.Entries().size() != written_entries ||
		    verified.Sources().size() != written_sources || written_entries == 0)
			return fail("temporary journal verification failed");
	}
	std::vector<uint8_t> check;
	if (!ReadBytes(path, check) || check != original)
		return fail("journal changed during migration");
	result.backup = std::filesystem::path(path.string() + ".int20.bak");
	if (!std::filesystem::exists(result.backup, error)) {
		if (!std::filesystem::copy_file(path, result.backup, std::filesystem::copy_options::none,
		                                error))
			return fail("could not preserve int20 backup: " + error.message());
	}
	if (!ReadBytes(result.backup, check) || check != original)
		return fail("int20 backup differs from original");
	if (!ReadBytes(path, check) || check != original)
		return fail("journal changed before replacement");
	if (!Replace(temporary, path, error))
		return fail("atomic replacement failed: " + error.message());
	result.migrated = true;
	if (log)
		log("Shader journal migration: converted " + std::to_string(result.converted_sources) +
		    " sources; " + std::to_string(result.unavailable_entries) + " of " +
		    std::to_string(result.entries) + " permutations unavailable; backup " +
		    result.backup.string());
	return result;
}

} // namespace Libs::Graphics::ShaderJournalMigration
