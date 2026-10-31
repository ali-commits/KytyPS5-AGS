#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
namespace Libs::Controller {
class SourceState {
public:
	uint32_t LocalButton(uint32_t button, bool down) {
		m_local_buttons = down ? m_local_buttons | button : m_local_buttons & ~button;
		return Buttons();
	}
	void LocalAxis(size_t axis, int value) {
		m_local_axes[axis]  = value;
		m_remote_axis[axis] = false;
	}
	void Remote(uint32_t buttons, const std::array<int, 6>& axes) {
		m_remote_buttons = buttons;
		m_remote_axes    = axes;
		m_remote_axis.fill(true);
	}
	void ResetLocal() {
		m_local_buttons = 0;
		m_local_axes    = {128, 128, 128, 128, 0, 0};
	}
	void ResetRemote() {
		m_remote_buttons = 0;
		m_remote_axis.fill(false);
	}
	uint32_t Buttons() const {
		return m_local_buttons | m_remote_buttons | (Axis(4) > 0 ? 0x100U : 0U) |
		       (Axis(5) > 0 ? 0x200U : 0U);
	}
	int Axis(size_t axis) const {
		return m_remote_axis[axis] ? m_remote_axes[axis] : m_local_axes[axis];
	}

private:
	uint32_t           m_local_buttons = 0, m_remote_buttons = 0;
	std::array<int, 6> m_local_axes {128, 128, 128, 128, 0, 0},
	    m_remote_axes {128, 128, 128, 128, 0, 0};
	std::array<bool, 6> m_remote_axis {};
};
} // namespace Libs::Controller
