#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_WINDOW_LOWLATENCY_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_WINDOW_LOWLATENCY_H_

#include <cstdint>
#include <mutex>
#include <vulkan/vulkan_core.h>

// Low-latency mode (Config::LowLatencyEnabled, --low-latency): NVIDIA's low-latency sleep and
// latency markers through VK_NV_low_latency2 (the Vulkan side of Reflex). Used only when the
// device reports VK_NV_low_latency2 and VK_KHR_present_id; otherwise the setting is logged as
// unavailable and ignored. Ported from the AstroHybrid KytyPS5 fork's low-latency mode.
//
// Frames are numbered by guest flips, in the order the guest issues them:
//  - guest thread: a DCB submission that carries a flip packet (AgcDcbSetFlip since the previous
//    submission), or a CPU flip (sceVideoOutSubmitFlip), ends frame N (SIMULATION_END N). The
//    thread then calls vkLatencySleepNV and waits for its semaphore (at most 100 ms), which is
//    where the driver holds back the start of the next frame, and marks SIMULATION_START N+1;
//  - command processor: reserving the N-th GPU flip marks RENDERSUBMIT_END N and
//    RENDERSUBMIT_START N+1;
//  - presenter: the presented flip's frame number is the present ID (VkPresentIdKHR), and
//    PRESENT_START / PRESENT_END frame vkQueuePresentKHR. A repeated present carries no ID.
//
// Config::LowLatencyFpsLimit (--low-latency-fps-limit, 0 = none) also has the driver pace frames
// to that rate in the sleep, before the game starts a frame, instead of the game or a patch
// limiting it after the frame was simulated.
//
// KYTY_LOW_LATENCY_MEASURE=1 (diagnostic): markers and reports with the low-latency mode and
// its sleep off, as the baseline to compare against. Either way, every 10 s the presenter reads
// the driver's per-frame reports (vkGetLatencyTimingsNV) and prints one "LowLatency 10s:" line.
//
// Calls naming the swapchain are serialized by one mutex, also held around vkQueuePresentKHR
// (PresentScope), as the swapchain is externally synchronized.
namespace Libs::Graphics::LowLatency {

// Device creation: whether to enable VK_NV_low_latency2 and VK_KHR_present_id (with its
// presentId feature), given whether the device reports them.
[[nodiscard]] bool WantExtensions(bool has_low_latency2, bool has_present_id);
// After vkCreateDevice with both extensions enabled.
void               OnDeviceCreated(VkDevice device);
[[nodiscard]] bool Enabled();

// Swapchain creation: the pNext chain to pass in VkSwapchainCreateInfoKHR.
[[nodiscard]] const void* SwapchainCreateNext(const void* next);
void                      OnSwapchainCreated(VkSwapchainKHR swapchain);
void                      OnSwapchainDestroyed(VkSwapchainKHR swapchain);

// Guest side.
void NoteFlipPacket(); // AgcDcbSetFlip (any thread)
void AfterDcbSubmit(); // a DCB submission returned (guest thread)
// sceVideoOutSubmitFlip (guest thread): BeginCpuFlip numbers the frame before its reservation,
// which takes the number with TakeCpuFlipFrame; EndCpuFlip ends it once the flip is queued.
[[nodiscard]] uint64_t BeginCpuFlip();
[[nodiscard]] uint64_t TakeCpuFlipFrame();
void                   EndCpuFlip(uint64_t frame);
// Command processor: a GPU flip was reserved; its frame number (0 when off).
[[nodiscard]] uint64_t OnGpuFlipReserved();

// Presenter: the frame number of the flip presented next (0: a repeat).
void SetNextPresentFrame(uint64_t frame);

// Around vkQueuePresentKHR: holds the swapchain mutex, marks PRESENT_START / PRESENT_END and
// gives the present ID to chain (0: none).
class PresentScope {
public:
	explicit PresentScope(VkSwapchainKHR swapchain);
	~PresentScope();
	PresentScope(const PresentScope&)            = delete;
	PresentScope& operator=(const PresentScope&) = delete;

	[[nodiscard]] uint64_t PresentId() const { return m_present_id; }

private:
	std::unique_lock<std::mutex> m_lock;
	VkSwapchainKHR               m_swapchain  = VK_NULL_HANDLE;
	uint64_t                     m_present_id = 0;
};

} // namespace Libs::Graphics::LowLatency

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_WINDOW_LOWLATENCY_H_
