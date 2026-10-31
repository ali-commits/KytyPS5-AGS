#ifndef EMULATOR_SHADER_JOURNAL_MIGRATION_H_
#define EMULATOR_SHADER_JOURNAL_MIGRATION_H_

#include "graphics/host_gpu/renderer/pipeline/shaderPrecompile.h"

namespace Libs::Graphics::ShaderJournalMigration {

std::vector<uint8_t> CurrentIdentity(uint32_t vendor, uint32_t device);
std::vector<uint8_t> Int20Identity(uint32_t vendor, uint32_t device);

// Pure conversion. Accepted inputs rebuild to the original static key, so pipeline recipes keep
// their permutation identities. On rejection the source is unchanged and reason explains why.
bool ConvertInt20Source(ShaderJournal::Source& source, std::string& reason);

struct Result {
	bool     usable            = true; // false: do not open the original with a different identity
	bool     migrated          = false;
	uint64_t converted_sources = 0;
	uint64_t unavailable_sources = 0;
	uint64_t entries             = 0;
	uint64_t unavailable_entries = 0;
	std::filesystem::path backup;
	std::string           error;
};

// At startup, before any writer opens the path. Only the exact same-device int20 ABI is migrated.
// A verified replacement is renamed atomically after an exact original backup is secured. Failed
// or unknown migrations preserve the source. Unavailable inputs persist as non-replayable records
// with an impossible user-data count, so they cannot suppress a later live recapture.
// Subsequent preparation runs still report missing recipes until the exact permutation is captured.
// Removes unavailable placeholders from replay only when a real current source with the same
// original identity, cursor and specialization was subsequently captured. Call before replay.
uint64_t ExcludeRecoveredUnavailable(ShaderJournal& journal);

Result MigrateInt20(const std::filesystem::path& path, uint32_t vendor, uint32_t device,
                    const std::function<void(const std::string&)>& log = {});

} // namespace Libs::Graphics::ShaderJournalMigration
#endif
