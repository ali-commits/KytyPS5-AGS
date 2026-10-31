// Kept out of archive.cpp so the statically linked emulator does not pull in the writer and the
// zstd compressor.
#include "common/archive.h"
#include "common/stringUtils.h"

#include <algorithm>
#include <zarchive/zarchivewriter.h>

namespace Common {

namespace {

// Comparing existing directories also handles case-insensitive hosts and directory aliases.
bool ContainsDestination(const std::filesystem::path& directory,
                         std::filesystem::path        destination_parent) {
	std::error_code error;
	while (!destination_parent.empty()) {
		if (std::filesystem::equivalent(directory, destination_parent, error) || error) {
			return true;
		}
		const auto parent = destination_parent.parent_path();
		if (parent == destination_parent) {
			break;
		}
		destination_parent = parent;
	}
	return false;
}

} // namespace

bool PackArchive(const std::filesystem::path& directory, const std::filesystem::path& archive,
                 const std::function<bool(uint64_t done, uint64_t total)>& progress) {
	namespace fs = std::filesystem;
	struct Output {
		File file;
		bool ok = true;
	} output;
	struct Entry {
		fs::path path;
		uint64_t size;
		bool     is_file;
	};
	std::vector<Entry> entries;
	uint64_t           total = 0;
	std::error_code    error;
	const auto         source = fs::canonical(directory, error);
	if (error || !fs::is_directory(source, error) || error || archive.filename().empty()) {
		return false;
	}
	const auto absolute_archive = fs::absolute(archive, error);
	if (error) {
		return false;
	}
	const auto destination_parent = fs::canonical(absolute_archive.parent_path(), error);
	if (error || ContainsDestination(source, destination_parent)) {
		return false;
	}
	// Validate the complete source walk before creating the destination. A followed directory
	// link may include the output even when the output lies outside the lexical source path.
	std::vector<fs::path> ancestors {source};
	// Follow directory links, as the emulator does when it runs the folder.
	fs::recursive_directory_iterator it(directory, fs::directory_options::follow_directory_symlink,
	                                    error);
	for (; !error && it != fs::end(it); it.increment(error)) {
		const bool is_file = !it->is_directory(error);
		if (!is_file && !error) {
			const auto resolved = fs::canonical(it->path(), error);
			if (error || ContainsDestination(resolved, destination_parent)) {
				return false;
			}
			ancestors.resize(static_cast<size_t>(it.depth()) + 1);
			for (const auto& ancestor: ancestors) {
				if (fs::equivalent(ancestor, resolved, error) || error) {
					return false; // A followed directory link cycles into its own ancestors.
				}
			}
			ancestors.push_back(resolved);
		}
		const auto size = is_file && !error ? it->file_size(error) : 0;
		if (error) {
			break;
		}
		entries.push_back({it->path(), size, is_file});
		total += size;
	}
	if (error || !output.file.CreateExclusive(archive)) {
		return false;
	}
	// Only this call's successfully created file can reach the cleanup below.
	ZArchiveWriter       writer([](int32_t, void*) {},
	                            [](const void* data, size_t size, void* opaque) {
		                      auto&    out     = *static_cast<Output*>(opaque);
		                      uint32_t written = 0;
		                      out.file.Write(data, static_cast<uint32_t>(size), &written);
		                      out.ok = out.ok && written == size;
	                            },
	                            &output);
	std::vector<uint8_t> buffer(1u << 20u);
	uint64_t             done = 0;
	for (size_t index = 0; output.ok && index < entries.size(); index++) {
		const auto& entry = entries[index];
		const auto  name  = PathToGenericString(entry.path.lexically_relative(directory));
		File        input;
		output.ok = entry.is_file ? writer.StartNewFile(name.c_str()) &&
		                                input.Open(entry.path, File::Mode::Read)
		                          : writer.MakeDir(name.c_str());
		for (uint64_t left = entry.size; output.ok && left != 0;) {
			uint32_t read = 0;
			input.Read(buffer.data(),
			           static_cast<uint32_t>(std::min<uint64_t>(left, buffer.size())), &read);
			writer.AppendData(buffer.data(), read);
			done += read;
			left -= read;
			output.ok = output.ok && read != 0 && progress(done, total);
		}
	}
	if (output.ok) {
		writer.Finalize();
	}
	output.ok = output.ok && output.file.Flush();
	output.file.Close();
	if (!output.ok) {
		File::DeleteFile(archive);
	}
	return output.ok;
}

} // namespace Common
