#ifndef KYTY_COMMON_FPS_STATS_H_
#define KYTY_COMMON_FPS_STATS_H_

#include <array>
#include <cstdint>
#include <optional>
#include <string>

namespace Common {

// The frame statistics the launcher host-metrics endpoint exposes over the last window.
struct FpsWindow {
	double   current     = 0.0; // Most recent one-second rate.
	double   avg         = 0.0; // Mean over the window.
	double   min         = 0.0; // Lowest one-second rate in the window.
	double   max         = 0.0; // Highest one-second rate in the window.
	double   frame_ms    = 0.0; // Latest frame interval.
	uint32_t window_s    = 0;   // How many seconds the window covers.
	bool     has_samples = false;
};

// Called for each main-bus guest flip, on the present thread. Cost is one atomic exchange and one
// atomic increment: no lock, no allocation. Safe to call before the graphics subsystem starts a
// game; the first call lazily starts the once-per-second writer thread.
void FpsStatsRecordFlip() noexcept;

// The window the writer thread aggregated last (once per second), or nullopt before the first
// tick. Cheap: one short mutex. Feeds the game_fps and frame_time_ms session metrics.
[[nodiscard]] std::optional<FpsWindow> FpsStatsLatest() noexcept;

// Stop the writer thread and remove the stats file. Idempotent.
void FpsStatsShutdown() noexcept;

// The file the writer thread emits under $XDG_RUNTIME_DIR/kyty-host.
[[nodiscard]] std::string FpsStatsFilePath();

// Build the JSON body the writer thread stores, from a window. Pure, for testing.
[[nodiscard]] std::string FpsStatsEncode(const FpsWindow& window);

// Parse a stats file body. Empty/invalid/stale (ts older than max_age_s) yields nullopt. Pure.
[[nodiscard]] std::optional<FpsWindow> FpsStatsParse(const std::string& json, int64_t now_s,
                                                     int64_t max_age_s);

// Aggregate a window from a ring of per-second flip counts plus the latest frame interval. Pure,
// exposed for testing the window math independent of threads and the clock.
[[nodiscard]] FpsWindow FpsStatsAggregate(const std::array<uint32_t, 10>& per_second, size_t count,
                                          double latest_frame_ms);

} // namespace Common

#endif /* KYTY_COMMON_FPS_STATS_H_ */
