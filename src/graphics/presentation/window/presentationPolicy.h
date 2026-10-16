#pragma once

#include <array>
#include <cstdint>
#include <cstring>

namespace Libs::Graphics {

[[nodiscard]] inline bool StreamOnlyRequested(const char* requested, const char* socket) noexcept {
	return requested != nullptr && std::strcmp(requested, "1") == 0 && socket != nullptr &&
	       *socket != '\0';
}

struct RemoteOverlayInputRoute {
	bool               connected       = false;
	uint32_t           guest_buttons   = 0;
	uint32_t           overlay_buttons = 0;
	uint32_t           changed_buttons = 0;
	std::array<int, 6> guest_axes {128, 128, 128, 128, 0, 0};
	std::array<int, 6> overlay_axes {128, 128, 128, 128, 0, 0};
};

// Owns only remote snapshots. Local SDL/controller holds remain in their existing source.
class RemoteOverlayInputRouter {
public:
	RemoteOverlayInputRoute Route(uint64_t capture_session, bool connected, uint32_t buttons,
	                              const std::array<int, 6>& axes) noexcept {
		RemoteOverlayInputRoute result;
		result.connected = connected;
		if (capture_session != m_session || connected != m_connected) {
			m_release_pending = m_release_pending || m_connected || capture_session != 0;
			m_session         = capture_session;
			m_overlay_buttons = 0;
		}
		m_connected = connected;
		if (!connected) {
			m_release_pending = true;
			return result;
		}
		if (m_release_pending) {
			bool neutral = buttons == 0 && axes[4] == 0 && axes[5] == 0;
			for (size_t i = 0; i < 4; i++)
				neutral &= axes[i] >= 112 && axes[i] <= 144;
			if (!neutral) return result;
			m_release_pending = false;
		}
		if (capture_session != 0) {
			result.overlay_buttons = buttons;
			result.changed_buttons = m_overlay_buttons ^ buttons;
			result.overlay_axes    = axes;
			m_overlay_buttons      = buttons;
		} else {
			result.guest_buttons = buttons;
			result.guest_axes    = axes;
		}
		return result;
	}

private:
	uint64_t m_session         = 0;
	uint32_t m_overlay_buttons = 0;
	bool     m_connected       = false;
	bool     m_release_pending = false;
};

} // namespace Libs::Graphics
