#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>

namespace Libs::Graphics {

[[nodiscard]] inline bool StreamOnlyRequested(const char* requested, const char* socket) noexcept {
	return requested != nullptr && std::strcmp(requested, "1") == 0 && socket != nullptr &&
	       *socket != '\0';
}

enum class ModalInputSource : uint8_t { Local, Remote };

// Called with both visibility and input locks held; extra SDL wakeups may be stale.
template <typename Visibility, typename Input>
bool ConsumeNewestOverlayVisibility(std::deque<Visibility>& visibility, std::deque<Input>& input,
                                    Visibility& update) {
	if (visibility.empty()) return false;
	update = visibility.back();
	visibility.clear();
	std::erase_if(input, [&](const auto& event) { return event.session != update.session; });
	return true;
}

template <typename Input>
bool CanCoalesceOverlayInput(const Input& previous, const Input& next) {
	return previous.session == next.session && previous.kind == next.kind &&
	       previous.id == next.id && previous.source == next.source;
}

// Per-source holds merge before ImGui sees them. A neutral/disconnected peer owns no local state.
class ModalControllerInput {
public:
	void SetButton(ModalInputSource source, int button, bool down) noexcept {
		if (button < 0 || button >= 32) return;
		auto&          buttons = m_sources[static_cast<size_t>(source)].buttons;
		const uint32_t bit     = uint32_t {1} << button;
		if (down)
			buttons |= bit;
		else
			buttons &= ~bit;
	}
	void SetAxis(ModalInputSource source, int axis, float value) noexcept {
		if (axis >= 0 && axis < 4) m_sources[static_cast<size_t>(source)].axes[axis] = value;
	}
	void Reset(ModalInputSource source) noexcept { m_sources[static_cast<size_t>(source)] = {}; }
	bool ButtonDown(int button) const noexcept {
		return button >= 0 && button < 32 &&
		       ((m_sources[0].buttons | m_sources[1].buttons) & (uint32_t {1} << button)) != 0;
	}
	float Axis(int axis) const noexcept {
		if (axis < 0 || axis >= 4) return 0.0f;
		const float local = m_sources[0].axes[axis], remote = m_sources[1].axes[axis];
		return std::abs(local) >= std::abs(remote) ? local : remote;
	}

private:
	struct Source {
		uint32_t             buttons = 0;
		std::array<float, 4> axes {};
	};
	std::array<Source, 2> m_sources {};
};

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
