#include "graphics/presentation/window/lowLatency.h"

#include "common/emulatorConfig.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace Libs::Graphics::LowLatency {

namespace {

enum class Mode { Off, On, Measure };

struct State {
	std::atomic<bool>              enabled {false};
	VkDevice                       device          = VK_NULL_HANDLE;
	PFN_vkSetLatencySleepModeNV    set_sleep_mode  = nullptr;
	PFN_vkLatencySleepNV           sleep           = nullptr;
	PFN_vkSetLatencyMarkerNV       marker          = nullptr;
	PFN_vkGetLatencyTimingsNV      timings         = nullptr;
	VkSemaphore                    semaphore       = VK_NULL_HANDLE;
	uint64_t                       semaphore_value = 0; // under mutex
	std::mutex                     mutex;               // the swapchain's calls
	VkSwapchainKHR                 swapchain = VK_NULL_HANDLE;
	VkSwapchainLatencyCreateInfoNV create_info {};
	// Guest frames: flip packets set, flip packets accounted to a submission, frames ended.
	std::atomic<uint64_t> flips_set {0};
	std::atomic<uint64_t> flips_accounted {0};
	std::atomic<uint64_t> guest_frame {0};
	std::atomic<uint64_t> gpu_frame {0};
	// Presenter (under mutex).
	uint64_t next_present_frame = 0;
	uint64_t last_present_id    = 0;
	int64_t  report_ns          = 0;
	// Statistics.
	std::atomic<uint64_t> sleeps {0};
	std::atomic<uint64_t> sleep_us {0};
	std::atomic<uint64_t> sleep_timeouts {0};
};

State& Get() {
	static State state;
	return state;
}

Mode CurrentMode() {
	static const Mode mode = [] {
		if (!Config::LowLatencyEnabled()) {
			const char* measure = std::getenv("KYTY_LOW_LATENCY_MEASURE");
			return measure != nullptr && std::strcmp(measure, "1") == 0 ? Mode::Measure : Mode::Off;
		}
		return Mode::On;
	}();
	return mode;
}

int64_t NowNs() {
	return std::chrono::duration_cast<std::chrono::nanoseconds>(
	           std::chrono::steady_clock::now().time_since_epoch())
	    .count();
}

void MarkLocked(State& s, uint64_t frame, VkLatencyMarkerNV marker) {
	if (s.swapchain == VK_NULL_HANDLE || frame == 0) {
		return;
	}
	VkSetLatencyMarkerInfoNV info {};
	info.sType     = VK_STRUCTURE_TYPE_SET_LATENCY_MARKER_INFO_NV;
	info.presentID = frame;
	info.marker    = marker;
	s.marker(s.device, s.swapchain, &info);
}

void Mark(uint64_t frame, VkLatencyMarkerNV marker) {
	auto&                       s = Get();
	std::lock_guard<std::mutex> lock(s.mutex);
	MarkLocked(s, frame, marker);
}

// The end of guest frame `frame`: its marker, the driver's sleep, the next frame's start.
void EndGuestFrame(uint64_t frame) {
	auto& s = Get();
	Mark(frame, VK_LATENCY_MARKER_SIMULATION_END_NV);
	uint64_t value = 0;
	if (CurrentMode() == Mode::On) {
		std::lock_guard<std::mutex> lock(s.mutex);
		if (s.swapchain != VK_NULL_HANDLE) {
			value = ++s.semaphore_value;
			VkLatencySleepInfoNV info {};
			info.sType           = VK_STRUCTURE_TYPE_LATENCY_SLEEP_INFO_NV;
			info.signalSemaphore = s.semaphore;
			info.value           = value;
			if (s.sleep(s.device, s.swapchain, &info) != VK_SUCCESS) {
				value = 0;
			}
		}
	}
	if (value != 0) {
		const auto          start = NowNs();
		VkSemaphoreWaitInfo wait {};
		wait.sType          = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
		wait.semaphoreCount = 1;
		wait.pSemaphores    = &s.semaphore;
		wait.pValues        = &value;
		// Bounded: the sleep never holds a frame back longer than 100 ms.
		if (VULKAN_HPP_DEFAULT_DISPATCHER.vkWaitSemaphores(s.device, &wait, 100'000'000ULL) ==
		    VK_TIMEOUT) {
			s.sleep_timeouts.fetch_add(1, std::memory_order_relaxed);
		}
		s.sleeps.fetch_add(1, std::memory_order_relaxed);
		s.sleep_us.fetch_add(static_cast<uint64_t>((NowNs() - start) / 1000),
		                     std::memory_order_relaxed);
	}
	Mark(frame + 1, VK_LATENCY_MARKER_SIMULATION_START_NV);
}

// The driver's per-frame reports, once every 10 s (presenter, mutex held).
void ReportLocked(State& s) {
	const auto now = NowNs();
	if (s.report_ns == 0) {
		s.report_ns = now;
		return;
	}
	if (now - s.report_ns < 10'000'000'000LL) {
		return;
	}
	s.report_ns = now;
	VkGetLatencyMarkerInfoNV query {};
	query.sType = VK_STRUCTURE_TYPE_GET_LATENCY_MARKER_INFO_NV;
	s.timings(s.device, s.swapchain, &query);
	std::vector<VkLatencyTimingsFrameReportNV> reports(query.timingCount);
	for (auto& report: reports) {
		report       = {};
		report.sType = VK_STRUCTURE_TYPE_LATENCY_TIMINGS_FRAME_REPORT_NV;
	}
	query.pTimings = reports.data();
	if (!reports.empty()) {
		s.timings(s.device, s.swapchain, &query);
	}
	uint64_t filled = 0;
	uint64_t total  = 0;
	uint64_t sim    = 0;
	uint64_t gpu    = 0;
	for (uint32_t i = 0; i < query.timingCount; i++) {
		const auto& r = reports[i];
		if (r.simStartTimeUs == 0 || r.gpuRenderEndTimeUs < r.simStartTimeUs) {
			continue;
		}
		filled++;
		total += r.gpuRenderEndTimeUs - r.simStartTimeUs;
		if (r.simEndTimeUs >= r.simStartTimeUs) {
			sim += r.simEndTimeUs - r.simStartTimeUs;
		}
		if (r.gpuRenderStartTimeUs != 0 && r.gpuRenderEndTimeUs >= r.gpuRenderStartTimeUs) {
			gpu += r.gpuRenderEndTimeUs - r.gpuRenderStartTimeUs;
		}
	}
	const auto   sleeps   = s.sleeps.exchange(0, std::memory_order_relaxed);
	const auto   sleep_us = s.sleep_us.exchange(0, std::memory_order_relaxed);
	const double frames   = static_cast<double>(filled == 0 ? 1 : filled);
	std::printf(
	    "LowLatency 10s (%s): %llu of %u driver reports complete, simulation start to GPU "
	    "end %.2f ms, simulation %.2f ms, GPU %.2f ms | sleeps %llu, average %.2f ms, "
	    "%llu timed out\n",
	    CurrentMode() == Mode::On ? "on" : "measure", static_cast<unsigned long long>(filled),
	    query.timingCount, static_cast<double>(total) / frames / 1000.0,
	    static_cast<double>(sim) / frames / 1000.0, static_cast<double>(gpu) / frames / 1000.0,
	    static_cast<unsigned long long>(sleeps),
	    sleeps == 0 ? 0.0 : static_cast<double>(sleep_us) / static_cast<double>(sleeps) / 1000.0,
	    static_cast<unsigned long long>(s.sleep_timeouts.exchange(0)));
	std::fflush(stdout);
}

thread_local uint64_t t_cpu_flip_frame = 0;

} // namespace

bool WantExtensions(bool has_low_latency2, bool has_present_id) {
	if (CurrentMode() == Mode::Off) {
		return false;
	}
	if (!has_low_latency2 || !has_present_id) {
		std::printf("LowLatency: unavailable on this device (VK_NV_low_latency2 %d, "
		            "VK_KHR_present_id %d): off\n",
		            has_low_latency2 ? 1 : 0, has_present_id ? 1 : 0);
		return false;
	}
	return true;
}

void OnDeviceCreated(VkDevice device) {
	auto&      s   = Get();
	const auto get = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr;
	s.device       = device;
	s.set_sleep_mode =
	    reinterpret_cast<PFN_vkSetLatencySleepModeNV>(get(device, "vkSetLatencySleepModeNV"));
	s.sleep   = reinterpret_cast<PFN_vkLatencySleepNV>(get(device, "vkLatencySleepNV"));
	s.marker  = reinterpret_cast<PFN_vkSetLatencyMarkerNV>(get(device, "vkSetLatencyMarkerNV"));
	s.timings = reinterpret_cast<PFN_vkGetLatencyTimingsNV>(get(device, "vkGetLatencyTimingsNV"));
	if (s.set_sleep_mode == nullptr || s.sleep == nullptr || s.marker == nullptr ||
	    s.timings == nullptr) {
		std::printf("LowLatency: VK_NV_low_latency2 entry points missing: off\n");
		return;
	}
	VkSemaphoreTypeCreateInfo type {};
	type.sType         = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
	type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
	VkSemaphoreCreateInfo create {};
	create.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
	create.pNext = &type;
	if (VULKAN_HPP_DEFAULT_DISPATCHER.vkCreateSemaphore(device, &create, nullptr, &s.semaphore) !=
	    VK_SUCCESS) {
		std::printf("LowLatency: timeline semaphore creation failed: off\n");
		return;
	}
	s.create_info.sType             = VK_STRUCTURE_TYPE_SWAPCHAIN_LATENCY_CREATE_INFO_NV;
	s.create_info.latencyModeEnable = VK_TRUE;
	s.enabled.store(true, std::memory_order_release);
}

bool Enabled() {
	return Get().enabled.load(std::memory_order_acquire);
}

const void* SwapchainCreateNext(const void* next) {
	auto& s = Get();
	if (!Enabled()) {
		return next;
	}
	s.create_info.pNext = next;
	return &s.create_info;
}

void OnSwapchainCreated(VkSwapchainKHR swapchain) {
	auto& s = Get();
	if (!Enabled()) {
		return;
	}
	std::lock_guard<std::mutex> lock(s.mutex);
	const bool                  on    = CurrentMode() == Mode::On;
	const uint32_t              limit = Config::LowLatencyFpsLimit();
	VkLatencySleepModeInfoNV    mode {};
	mode.sType             = VK_STRUCTURE_TYPE_LATENCY_SLEEP_MODE_INFO_NV;
	mode.lowLatencyMode    = on ? VK_TRUE : VK_FALSE;
	mode.lowLatencyBoost   = on ? VK_TRUE : VK_FALSE;
	mode.minimumIntervalUs = on && limit != 0 ? 1'000'000U / limit : 0;
	const auto result      = s.set_sleep_mode(s.device, swapchain, &mode);
	std::printf("LowLatency: %s, frame limit %s -> %s\n", on ? "on" : "measure only",
	            mode.minimumIntervalUs != 0 ? (std::to_string(limit) + " FPS").c_str() : "none",
	            result == VK_SUCCESS ? "ok" : "failed");
	std::fflush(stdout);
	s.swapchain       = result == VK_SUCCESS ? swapchain : VK_NULL_HANDLE;
	s.last_present_id = 0;
}

void OnSwapchainDestroyed(VkSwapchainKHR swapchain) {
	auto& s = Get();
	if (!Enabled()) {
		return;
	}
	std::lock_guard<std::mutex> lock(s.mutex);
	if (s.swapchain == swapchain) {
		s.swapchain = VK_NULL_HANDLE;
	}
}

void NoteFlipPacket() {
	if (Enabled()) {
		Get().flips_set.fetch_add(1, std::memory_order_acq_rel);
	}
}

void AfterDcbSubmit() {
	if (!Enabled()) {
		return;
	}
	auto&      s   = Get();
	const auto set = s.flips_set.load(std::memory_order_acquire);
	auto       old = s.flips_accounted.load(std::memory_order_acquire);
	if (set == old || !s.flips_accounted.compare_exchange_strong(old, set)) {
		return;
	}
	// One frame per flip packet in the submission (normally one).
	const auto frame = s.guest_frame.fetch_add(set - old, std::memory_order_acq_rel) + (set - old);
	EndGuestFrame(frame);
}

uint64_t BeginCpuFlip() {
	if (!Enabled()) {
		return 0;
	}
	t_cpu_flip_frame = Get().guest_frame.fetch_add(1, std::memory_order_acq_rel) + 1;
	return t_cpu_flip_frame;
}

uint64_t TakeCpuFlipFrame() {
	const auto frame = t_cpu_flip_frame;
	t_cpu_flip_frame = 0;
	return frame;
}

void EndCpuFlip(uint64_t frame) {
	if (Enabled() && frame != 0) {
		EndGuestFrame(frame);
	}
}

uint64_t OnGpuFlipReserved() {
	if (!Enabled()) {
		return 0;
	}
	auto&                       s     = Get();
	const auto                  frame = s.gpu_frame.fetch_add(1, std::memory_order_acq_rel) + 1;
	std::lock_guard<std::mutex> lock(s.mutex);
	MarkLocked(s, frame, VK_LATENCY_MARKER_RENDERSUBMIT_END_NV);
	MarkLocked(s, frame + 1, VK_LATENCY_MARKER_RENDERSUBMIT_START_NV);
	return frame;
}

void SetNextPresentFrame(uint64_t frame) {
	if (!Enabled()) {
		return;
	}
	auto&                       s = Get();
	std::lock_guard<std::mutex> lock(s.mutex);
	s.next_present_frame = frame;
}

PresentScope::PresentScope(VkSwapchainKHR swapchain) {
	if (!Enabled()) {
		return;
	}
	auto& s              = Get();
	m_lock               = std::unique_lock<std::mutex>(s.mutex);
	m_swapchain          = swapchain;
	const auto frame     = s.next_present_frame;
	s.next_present_frame = 0;
	if (s.swapchain != swapchain || frame == 0 || frame <= s.last_present_id) {
		return;
	}
	s.last_present_id = frame;
	m_present_id      = frame;
	MarkLocked(s, frame, VK_LATENCY_MARKER_PRESENT_START_NV);
}

PresentScope::~PresentScope() {
	if (!m_lock.owns_lock()) {
		return;
	}
	auto& s = Get();
	if (s.swapchain == m_swapchain) {
		MarkLocked(s, m_present_id, VK_LATENCY_MARKER_PRESENT_END_NV);
		ReportLocked(s);
	}
}

} // namespace Libs::Graphics::LowLatency
