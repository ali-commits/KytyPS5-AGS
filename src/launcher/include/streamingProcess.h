#ifndef KYTY_STREAMING_PROCESS_H
#define KYTY_STREAMING_PROCESS_H
// Linux process identities are kernel observations, never trusted session-file PIDs.
#ifdef __linux__
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <iterator>
#include <limits>
#include <poll.h>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <vector>
namespace StreamingProcess {
struct Identity {
	int           pid   = 0;
	int           pgid  = 0;
	std::uint64_t start = 0;
	char          state = 0;
	std::string   executable;
};
inline std::string ReadFile(const std::string& path) {
	std::ifstream file(path, std::ios::binary);
	return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
inline bool Number(const std::string& text, std::uint64_t* number) {
	if (text.empty() || text[0] < '0' || text[0] > '9') return false;
	char* end        = nullptr;
	errno            = 0;
	const auto value = std::strtoull(text.c_str(), &end, 10);
	if (errno != 0 || end == nullptr || *end != '\0') return false;
	*number = value;
	return true;
}
inline bool ReadStat(int pid, Identity* identity) {
	if (pid < 2) return false;
	const auto  directory = "/proc/" + std::to_string(pid);
	struct stat info {};
	if (::stat(directory.c_str(), &info) != 0 || info.st_uid != ::getuid()) return false;
	const auto text  = ReadFile(directory + "/stat");
	const auto close = text.rfind(") ");
	if (close == std::string::npos) return false;
	std::istringstream       stream(text.substr(close + 2));
	std::vector<std::string> fields;
	for (std::string value; stream >> value;)
		fields.push_back(value);
	std::uint64_t pgid = 0, start = 0;
	if (fields.size() < 20 || !Number(fields[2], &pgid) || pgid > std::numeric_limits<int>::max() ||
	    !Number(fields[19], &start))
		return false;
	*identity = {pid, int(pgid), start, fields[0][0], {}};
	return true;
}
inline bool ReadIdentity(int pid, Identity* identity) {
	if (!ReadStat(pid, identity)) return false;
	char       executable[4096];
	const auto path   = "/proc/" + std::to_string(pid) + "/exe";
	const auto length = ::readlink(path.c_str(), executable, sizeof(executable));
	if (length <= 0 || length == sizeof(executable)) return false;
	identity->executable.assign(executable, std::size_t(length));
	return true;
}
inline bool HasNonce(int pid, const std::string& nonce) {
	if (nonce.empty()) return false;
	const auto environment = ReadFile("/proc/" + std::to_string(pid) + "/environ");
	const auto expected    = "KYTY_STREAM_HOST_NONCE=" + nonce;
	for (std::size_t offset = 0; offset < environment.size();) {
		const auto end = environment.find('\0', offset);
		if (end == std::string::npos) break;
		if (environment.compare(offset, end - offset, expected) == 0) return true;
		offset = end + 1;
	}
	return false;
}
inline bool Exited(int pidfd) {
	pollfd watched {pidfd, POLLIN, 0};
	return ::poll(&watched, 1, 0) > 0 && (watched.revents & (POLLIN | POLLHUP | POLLNVAL)) != 0;
}
inline int Open(const Identity& expected, const std::string& nonce = {}) {
	const int fd = int(::syscall(SYS_pidfd_open, expected.pid, 0));
	Identity  current;
	if (fd < 0) return -1;
	if (!ReadIdentity(expected.pid, &current) || current.start != expected.start ||
	    current.executable != expected.executable ||
	    (!nonce.empty() && !HasNonce(expected.pid, nonce)) || Exited(fd)) {
		::close(fd);
		return -1;
	}
	return fd;
}
inline void Signal(int pidfd, int signal) {
	::syscall(SYS_pidfd_send_signal, pidfd, signal, nullptr, 0);
}
inline std::vector<Identity> Processes() {
	std::vector<Identity> result;
	DIR*                  directory = ::opendir("/proc");
	if (!directory) return result;
	while (const auto* entry = ::readdir(directory)) {
		std::uint64_t pid = 0;
		Identity      identity;
		if (Number(entry->d_name, &pid) && pid <= std::numeric_limits<int>::max() &&
		    ReadStat(int(pid), &identity))
			result.push_back(identity);
	}
	::closedir(directory);
	return result;
}
inline bool GroupAlive(int pgid, int exclude = 0) {
	for (const auto& process: Processes()) {
		if (process.pgid != pgid || process.pid == exclude) continue;
		if (process.state != 'Z' && process.state != 'X') return true;
		// A dead group leader can still have live threads. pidfd readiness is
		// the resource-lifetime test; already-exited zombies hold no game FDs.
		const int fd = int(::syscall(SYS_pidfd_open, process.pid, 0));
		if (fd >= 0) {
			const bool alive = !Exited(fd);
			::close(fd);
			if (alive) return true;
		}
	}
	return false;
}
} // namespace StreamingProcess
#endif
#endif
