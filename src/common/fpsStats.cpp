#include "common/fpsStats.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <nlohmann/json.hpp>
#include <numeric>
#include <stop_token>
#include <string>
#include <thread>

namespace Common {

namespace {

using Clock = std::chrono::steady_clock;

constexpr size_t WINDOW_SECONDS = 10;
constexpr auto   TICK           = std::chrono::seconds(1);

// Render-thread state. Touched per flip with two atomics only: no lock, no allocation.
std::atomic<uint64_t> g_flip_count {0};
std::atomic<int64_t>  g_last_flip_ns {0};     // Clock::now() of the previous flip, nanoseconds.
std::atomic<int64_t>  g_last_interval_ns {0}; // Latest frame interval, nanoseconds.

std::once_flag g_writer_once;
std::jthread   g_writer;

int64_t NowNs() {
	return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch())
	    .count();
}

int64_t NowUnixS() {
	return std::chrono::duration_cast<std::chrono::seconds>(
	           std::chrono::system_clock::now().time_since_epoch())
	    .count();
}

// Atomically write the body to the stats file (write to a temp then rename), so the launcher
// never reads a half-written file.
void WriteFileAtomic(const std::string& path, const std::string& body) {
	const auto temporary = path + ".tmp";
	if (FILE* file = std::fopen(temporary.c_str(), "wb"); file != nullptr) {
		const bool ok = std::fwrite(body.data(), 1, body.size(), file) == body.size();
		std::fclose(file);
		if (ok) {
			std::rename(temporary.c_str(), path.c_str());
		} else {
			std::remove(temporary.c_str());
		}
	}
}

void WriterThread(const std::stop_token& stop) {
	std::array<uint32_t, WINDOW_SECONDS> per_second {};
	size_t                               count    = 0;
	size_t                               next     = 0;
	uint64_t                             previous = g_flip_count.load(std::memory_order_relaxed);
	const auto                           path     = FpsStatsFilePath();

	while (!stop.stop_requested()) {
		std::this_thread::sleep_for(TICK);
		if (stop.stop_requested()) {
			break;
		}
		const uint64_t now_count = g_flip_count.load(std::memory_order_relaxed);
		const auto     flips     = static_cast<uint32_t>(now_count - previous);
		previous                 = now_count;

		per_second[next] = flips;
		next             = (next + 1) % WINDOW_SECONDS;
		count            = std::min(count + 1, WINDOW_SECONDS);

		// A guest that has stopped flipping (loading screen, paused) leaves the last interval
		// stale; the launcher's own staleness check handles a wholly idle game.
		const double latest_frame_ms =
		    static_cast<double>(g_last_interval_ns.load(std::memory_order_relaxed)) / 1.0e6;

		// Reorder the ring oldest-first so aggregation and "current" are unambiguous.
		std::array<uint32_t, WINDOW_SECONDS> ordered {};
		for (size_t i = 0; i < count; ++i) {
			ordered[i] = per_second[(next + WINDOW_SECONDS - count + i) % WINDOW_SECONDS];
		}
		WriteFileAtomic(path, FpsStatsEncode(FpsStatsAggregate(ordered, count, latest_frame_ms)));
	}
	std::remove(path.c_str());
}

} // namespace

void FpsStatsRecordFlip() noexcept {
	const int64_t now  = NowNs();
	const int64_t prev = g_last_flip_ns.exchange(now, std::memory_order_relaxed);
	if (prev != 0) {
		g_last_interval_ns.store(now - prev, std::memory_order_relaxed);
	}
	g_flip_count.fetch_add(1, std::memory_order_relaxed);
	std::call_once(g_writer_once, [] { g_writer = std::jthread(WriterThread); });
}

void FpsStatsShutdown() noexcept {
	if (g_writer.joinable()) {
		g_writer.request_stop();
		g_writer.join();
	}
}

std::string FpsStatsFilePath() {
	if (const char* path = std::getenv("AGS_SESSION_FPS_FILE"); path != nullptr && path[0] == '/') return path;
	const char*       runtime = std::getenv("XDG_RUNTIME_DIR");
	const std::string base =
	    runtime != nullptr && runtime[0] != '\0' ? std::string(runtime) + "/kyty-host" : "/tmp";
	return base + "/metrics-fps.json";
}

FpsWindow FpsStatsAggregate(const std::array<uint32_t, WINDOW_SECONDS>& per_second, size_t count,
                            double latest_frame_ms) {
	FpsWindow window;
	window.window_s = WINDOW_SECONDS;
	if (count == 0) {
		return window;
	}
	window.has_samples = true;
	window.current     = per_second[count - 1];
	window.frame_ms    = latest_frame_ms > 0.0 ? latest_frame_ms : 0.0;
	double   sum       = 0.0;
	uint32_t lo        = per_second[0];
	uint32_t hi        = per_second[0];
	for (size_t i = 0; i < count; ++i) {
		sum += per_second[i];
		lo = std::min(lo, per_second[i]);
		hi = std::max(hi, per_second[i]);
	}
	window.avg = sum / static_cast<double>(count);
	window.min = lo;
	window.max = hi;
	return window;
}

std::string FpsStatsEncode(const FpsWindow& window) {
	nlohmann::json out {{"ts", NowUnixS()}};
	if (window.has_samples) {
		out["current"]  = window.current;
		out["avg"]      = window.avg;
		out["min"]      = window.min;
		out["max"]      = window.max;
		out["frame_ms"] = window.frame_ms;
		out["window_s"] = window.window_s;
	}
	return out.dump();
}

std::optional<FpsWindow> FpsStatsParse(const std::string& json, int64_t now_s, int64_t max_age_s) {
	const auto parsed = nlohmann::json::parse(json, nullptr, false);
	if (!parsed.is_object() || !parsed.contains("ts") || !parsed["ts"].is_number()) {
		return std::nullopt;
	}
	const int64_t ts = parsed["ts"].get<int64_t>();
	if (now_s - ts > max_age_s) {
		return std::nullopt; // The emulator stopped updating: the game is gone or not flipping.
	}
	if (!parsed.contains("current")) {
		return std::nullopt; // A ts-only file means no frames have been sampled yet.
	}
	const auto number = [&](const char* key) {
		return parsed.contains(key) && parsed[key].is_number() ? parsed[key].get<double>() : 0.0;
	};
	FpsWindow window;
	window.has_samples = true;
	window.current     = number("current");
	window.avg         = number("avg");
	window.min         = number("min");
	window.max         = number("max");
	window.frame_ms    = number("frame_ms");
	window.window_s    = static_cast<uint32_t>(number("window_s"));
	return window;
}

} // namespace Common
