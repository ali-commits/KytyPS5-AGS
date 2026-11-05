#ifndef KYTY_STREAMING_SESSION_ENV_H
#define KYTY_STREAMING_SESSION_ENV_H
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace Streaming {

// Where the session socket path came from. AGS passes AGS_SESSION_SOCKET together with
// AGS_SESSION_PROTOCOL=1; KYTY_STREAM_SOCKET is the name the installed ags-host still uses.
enum class SessionSocketSource { None, Ags, Legacy, ProtocolMismatch };

struct SessionSocketChoice {
	const char*         path   = nullptr; // Null unless a usable socket was chosen.
	SessionSocketSource source = SessionSocketSource::None;
};

// Pure selection from the three environment values (null or empty counts as unset).
// AGS_SESSION_SOCKET wins and needs AGS_SESSION_PROTOCOL=1; with any other protocol the socket is
// ignored and KYTY_STREAM_SOCKET is not consulted. KYTY_STREAM_SOCKET applies only when
// AGS_SESSION_SOCKET is unset.
[[nodiscard]] inline SessionSocketChoice SelectSessionSocket(const char* ags_socket,
                                                             const char* ags_protocol,
                                                             const char* legacy_socket) noexcept {
	const auto set = [](const char* value) { return value != nullptr && *value != '\0'; };
	if (set(ags_socket)) {
		if (ags_protocol != nullptr && std::strcmp(ags_protocol, "1") == 0)
			return {ags_socket, SessionSocketSource::Ags};
		return {nullptr, SessionSocketSource::ProtocolMismatch};
	}
	if (set(legacy_socket)) return {legacy_socket, SessionSocketSource::Legacy};
	return {};
}

// The one environment lookup for every read site. Logs a protocol mismatch once.
[[nodiscard]] inline SessionSocketChoice SessionSocketFromEnvironment() noexcept {
	const auto choice = SelectSessionSocket(std::getenv("AGS_SESSION_SOCKET"),
	                                        std::getenv("AGS_SESSION_PROTOCOL"),
	                                        std::getenv("KYTY_STREAM_SOCKET"));
	if (choice.source == SessionSocketSource::ProtocolMismatch) {
		static std::atomic_flag logged = ATOMIC_FLAG_INIT;
		if (!logged.test_and_set(std::memory_order_relaxed))
			std::fprintf(stderr,
			             "AGS session socket ignored: AGS_SESSION_PROTOCOL is not 1 (got \"%s\")\n",
			             std::getenv("AGS_SESSION_PROTOCOL") != nullptr
			                 ? std::getenv("AGS_SESSION_PROTOCOL")
			                 : "");
	}
	return choice;
}

// Session socket path, or null when none is usable.
[[nodiscard]] inline const char* SessionSocket() noexcept {
	return SessionSocketFromEnvironment().path;
}

// KYTY_STREAM_FRAMES=0 asks for a metrics-only connection (Gamescope captures the frames). A
// stream-only launch needs its frame pool, so the opt-out never applies there.
[[nodiscard]] inline bool FramesRequested(const char* value, bool stream_only) noexcept {
	return stream_only || value == nullptr || std::strcmp(value, "0") != 0;
}

// F2 / Shift+F2 do nothing while a session socket is set: the client draws its own overlay.
[[nodiscard]] inline constexpr bool OverlayToggleAllowed(const SessionSocketChoice& choice) noexcept {
	return choice.path == nullptr;
}

} // namespace Streaming
#endif
