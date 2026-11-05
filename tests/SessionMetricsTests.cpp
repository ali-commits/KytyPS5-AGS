// AGS session socket: environment selection, tolerant packet handling, live metrics, the
// counter/overlay split and the metrics-only connection.
#include "common/debugCounters.h"
#include "streaming/metrics.h"
#include "streaming/protocol.h"
#include "streaming/sessionEnv.h"
#include "streaming/transport.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <functional>
#include <mutex>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

using namespace Streaming;

static void Check(bool ok, const char* what) {
	if (!ok) {
		std::fprintf(stderr, "FAIL: %s\n", what);
		std::abort();
	}
}

static bool Wait(const std::function<bool()>& condition, int milliseconds = 3000) {
	for (int i = 0; i < milliseconds / 2; i++) {
		if (condition()) return true;
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}
	return false;
}

// ---- environment selection ----------------------------------------------------------------

static void TestEnvSelection() {
	auto c = SelectSessionSocket("/run/ags.sock", "1", nullptr);
	Check(c.source == SessionSocketSource::Ags && std::strcmp(c.path, "/run/ags.sock") == 0,
	      "AGS socket with protocol 1 is used");
	c = SelectSessionSocket("/run/ags.sock", "1", "/run/old.sock");
	Check(c.source == SessionSocketSource::Ags && std::strcmp(c.path, "/run/ags.sock") == 0,
	      "AGS name wins over the legacy name");
	for (const char* protocol: {static_cast<const char*>(nullptr), "", "2", "0", "1 ", "01"}) {
		c = SelectSessionSocket("/run/ags.sock", protocol, nullptr);
		Check(c.path == nullptr && c.source == SessionSocketSource::ProtocolMismatch,
		      "AGS socket without protocol 1 is ignored");
	}
	c = SelectSessionSocket("/run/ags.sock", "2", "/run/old.sock");
	Check(c.path == nullptr && c.source == SessionSocketSource::ProtocolMismatch,
	      "a protocol mismatch does not fall back to the legacy socket");
	c = SelectSessionSocket(nullptr, nullptr, "/run/old.sock");
	Check(c.source == SessionSocketSource::Legacy && std::strcmp(c.path, "/run/old.sock") == 0,
	      "KYTY_STREAM_SOCKET is the fallback when AGS_SESSION_SOCKET is unset");
	c = SelectSessionSocket("", "1", "/run/old.sock");
	Check(c.source == SessionSocketSource::Legacy, "an empty AGS socket counts as unset");
	c = SelectSessionSocket(nullptr, "1", "");
	Check(c.path == nullptr && c.source == SessionSocketSource::None, "empty legacy is unset");
	c = SelectSessionSocket(nullptr, nullptr, nullptr);
	Check(c.path == nullptr && c.source == SessionSocketSource::None, "nothing set");

	Check(!OverlayToggleAllowed(SelectSessionSocket("/s", "1", nullptr)) &&
	          !OverlayToggleAllowed(SelectSessionSocket(nullptr, nullptr, "/s")) &&
	          OverlayToggleAllowed(SelectSessionSocket(nullptr, nullptr, nullptr)) &&
	          OverlayToggleAllowed(SelectSessionSocket("/s", "2", nullptr)),
	      "F2 toggles are off exactly while a usable session socket is set");
	Check(FramesRequested(nullptr, false) && FramesRequested("1", false) &&
	          !FramesRequested("0", false) && FramesRequested("0", true),
	      "KYTY_STREAM_FRAMES=0 opts out of frames unless stream-only");

	// The real lookup used by all four read sites.
	unsetenv("AGS_SESSION_SOCKET");
	unsetenv("AGS_SESSION_PROTOCOL");
	unsetenv("KYTY_STREAM_SOCKET");
	Check(SessionSocket() == nullptr, "no environment, no socket");
	setenv("KYTY_STREAM_SOCKET", "/run/old.sock", 1);
	Check(std::strcmp(SessionSocket(), "/run/old.sock") == 0, "environment fallback");
	setenv("AGS_SESSION_SOCKET", "/run/ags.sock", 1);
	Check(SessionSocket() == nullptr, "AGS socket without protocol is ignored");
	setenv("AGS_SESSION_PROTOCOL", "1", 1);
	Check(std::strcmp(SessionSocket(), "/run/ags.sock") == 0, "environment AGS name");
	unsetenv("AGS_SESSION_SOCKET");
	unsetenv("AGS_SESSION_PROTOCOL");
	unsetenv("KYTY_STREAM_SOCKET");
}

// ---- subscription parsing and the metrics message ----------------------------------------

static void TestCatalog() {
	const std::array<std::array<const char*, 3>, 4> expected {{{"game_fps", "fps", "number"},
	                                                           {"frame_time_ms", "ms", "number"},
	                                                           {"guest_memory_mb", "MB", "number"},
	                                                           {"shader_compiles", "count",
	                                                            "integer"}}};
	for (size_t i = 0; i < MetricCount; i++)
		Check(MetricCatalog[i].key == expected[i][0] && MetricCatalog[i].unit == expected[i][1] &&
		          MetricCatalog[i].type == expected[i][2],
		      "catalog key, unit and type");
	Check(MetricsMinIntervalMs == 1000, "catalog minimum interval");
#ifdef KYTY_ADAPTER_SOURCE
	// The adapter's hello catalog is the reference: one Metric(key, label, unit, type) per entry.
	std::ifstream     file(KYTY_ADAPTER_SOURCE);
	std::stringstream text;
	text << file.rdbuf();
	const auto source = text.str();
	if (source.empty()) {
		std::puts("note: adapter source not found, catalog cross-check skipped");
		return;
	}
	for (size_t i = 0; i < MetricCount; i++) {
		const auto at = source.find("Metric(\"" + std::string(MetricCatalog[i].key) + "\"");
		Check(at != std::string::npos, "adapter lists the metric");
		const auto line = source.substr(at, source.find(')', at) - at);
		Check(line.find(", \"" + std::string(MetricCatalog[i].unit) + "\", \"" +
		                std::string(MetricCatalog[i].type) + "\"") != std::string::npos,
		      "adapter unit and type match the emulator catalog");
	}
#endif
}

static void TestSubscribeParsing() {
	MetricsSubscription s;
	Check(ParseMetricsSubscribe({{"type", "metrics.subscribe"},
	                             {"keys", {"game_fps", "frame_time_ms"}},
	                             {"interval_ms", 2500}},
	                            s) &&
	          s.keys == (MetricBit(Metric::GameFps) | MetricBit(Metric::FrameTimeMs)) &&
	          s.interval_ms == 2500,
	      "keys and interval parsed");
	Check(ParseMetricsSubscribe({{"keys", {"game_fps"}}, {"interval_ms", 250}}, s) &&
	          s.interval_ms == 1000,
	      "interval clamped up to the catalog minimum");
	Check(ParseMetricsSubscribe({{"keys", {"game_fps"}}, {"interval_ms", -5}}, s) &&
	          s.interval_ms == 1000,
	      "negative interval clamped");
	Check(ParseMetricsSubscribe({{"keys", {"game_fps"}}, {"interval_ms", 4000000000LL}}, s) &&
	          s.interval_ms == MetricsMaxIntervalMs,
	      "huge interval clamped down");
	Check(ParseMetricsSubscribe({{"keys", {"game_fps"}}}, s) && s.interval_ms == 1000,
	      "missing interval is the minimum");
	Check(ParseMetricsSubscribe({{"keys", {"game_fps"}}, {"interval_ms", "fast"}}, s) &&
	          s.interval_ms == 1000,
	      "non-integer interval is the minimum");
	Check(ParseMetricsSubscribe({{"keys", {"nope", "cpu_percent", 7, nullptr, "shader_compiles"}},
	                             {"interval_ms", 1000}},
	                            s) &&
	          s.keys == MetricBit(Metric::ShaderCompiles),
	      "unknown and non-string keys are ignored");
	Check(ParseMetricsSubscribe({{"keys", Json::array()}, {"interval_ms", 1000}}, s) && s.keys == 0,
	      "an empty list stops metrics");
	s.keys = 5;
	Check(!ParseMetricsSubscribe({{"keys", "game_fps"}}, s) && s.keys == 5,
	      "keys that is not an array is rejected, old value kept");
	Check(!ParseMetricsSubscribe({{"interval_ms", 1000}}, s) && s.keys == 5, "missing keys rejected");
	Check(!ParseMetricsSubscribe(Json::array(), s), "non-object rejected");
}

static void TestMetricsMessage() {
	MetricValues values;
	values.Set(Metric::GameFps, 59.9);
	values.Set(Metric::FrameTimeMs, 16.7);
	values.Set(Metric::GuestMemoryMb, 8123.5);
	values.Set(Metric::ShaderCompiles, 42);
	auto message = BuildMetricsMessage(MetricBit(Metric::GameFps) | MetricBit(Metric::ShaderCompiles),
	                                   values, 123456);
	Check(message && (*message)["type"] == "metrics" && (*message)["t_ms"] == 123456 &&
	          (*message)["values"].size() == 2 && (*message)["values"]["game_fps"] == 59.9 &&
	          (*message)["values"]["shader_compiles"] == 42 &&
	          !(*message)["values"].contains("frame_time_ms") &&
	          !(*message)["values"].contains("guest_memory_mb"),
	      "message has only the subscribed keys");
	Check((*message)["values"]["shader_compiles"].is_number_unsigned(),
	      "integer metrics are integers on the wire");
	Check((*message)["values"]["game_fps"].is_number_float(), "number metrics are numbers");
	Check(message->dump().size() <= PacketLimit, "message fits one packet");
	Check(!BuildMetricsMessage(0, values, 1), "no keys, no message");
	MetricValues none;
	Check(!BuildMetricsMessage(MetricBit(Metric::GameFps), none, 1),
	      "a subscribed key without a value is left out, and an empty message is not sent");
	none.Set(Metric::FrameTimeMs, 1.0);
	message = BuildMetricsMessage(~MetricMask {0}, none, 1);
	Check(message && (*message)["values"].size() == 1, "values the source lacks are omitted");
	const auto before = MonotonicMs();
	std::this_thread::sleep_for(std::chrono::milliseconds(5));
	Check(MonotonicMs() >= before + 4, "monotonic clock advances in milliseconds");
}

static void TestUnknownTypeLog() {
	UnknownTypeLog log;
	Check(log.First("future") && !log.First("future") && log.First("other") && !log.First("other"),
	      "each unknown type is reported once");
	for (const char* known: {"welcome", "accepted", "error", "release", "input", "motion", "reset",
	                         "controller-audio-state", "metrics.subscribe"})
		Check(IsKnownPacketType(known), "known type");
	Check(!IsKnownPacketType("future") && !IsKnownPacketType("") && !IsKnownPacketType("Release"),
	      "unknown types");
}

// ---- counters versus overlay --------------------------------------------------------------

static void TestCounterOverlaySplit() {
	using namespace Common::DebugCounters;
	SetOverlayVisible(false);
	SetMetricsCollecting(false);
	const auto base = Get(Counter::ShadersCompiled);
	Add(Counter::ShadersCompiled);
	Check(!Enabled() && !OverlayVisible() && Get(Counter::ShadersCompiled) == base,
	      "nothing collects while no consumer is on");
	SetMetricsCollecting(true);
	Add(Counter::ShadersCompiled, 3);
	Check(Enabled() && MetricsCollecting() && !OverlayVisible() &&
	          Get(Counter::ShadersCompiled) == base + 3,
	      "a metrics subscription collects counters without showing the overlay");
	SetOverlayVisible(true);
	Check(OverlayVisible() && Enabled(), "overlay can be shown on top");
	SetMetricsCollecting(false);
	Check(OverlayVisible() && Enabled(), "dropping metrics keeps a visible overlay collecting");
	SetOverlayVisible(false);
	Add(Counter::ShadersCompiled);
	Check(!Enabled() && Get(Counter::ShadersCompiled) == base + 3, "both off, collection stops");
	SetOverlayVisible(true);
	SetMetricsCollecting(true);
	SetOverlayVisible(false);
	Check(!OverlayVisible() && Enabled(), "hiding the overlay keeps metrics collection");
	SetMetricsCollecting(false);
}

// ---- transport over a real socket ---------------------------------------------------------

struct Listener {
	char        directory[32] = "/tmp/kyty-metrics-XXXXXX";
	std::string path;
	int         fd   = -1;
	int         peer = -1;
	Listener() {
		Check(mkdtemp(directory) != nullptr, "private socket directory");
		path = std::string(directory) + "/feed.sock";
		fd   = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
		Check(fd >= 0, "listener");
		sockaddr_un address {};
		address.sun_family = AF_UNIX;
		std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
		Check(bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 &&
		          chmod(path.c_str(), 0600) == 0 && listen(fd, 1) == 0,
		      "protected listener");
	}
	void Accept() {
		Check(Wait([&] {
			      peer = accept4(fd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
			      return peer >= 0;
		      }),
		      "worker connects");
	}
	// Next packet of `type` (other types, such as the once-a-second status, are skipped).
	bool Next(const char* type, Json& out, int milliseconds = 3000, size_t fds = 0) {
		return Wait(
		    [&] {
			    Packet packet;
			    const auto result = ReceivePacket(peer, packet, fds);
			    if (result == ReceiveResult::Packet && packet.json["type"] == type) {
				    out = packet.json;
				    return true;
			    }
			    return false;
		    },
		    milliseconds);
	}
	bool Silent(const char* type, int milliseconds) {
		Json ignored;
		return !Next(type, ignored, milliseconds);
	}
	~Listener() {
		if (peer >= 0) close(peer);
		close(fd);
		unlink(path.c_str());
		rmdir(directory);
	}
};

struct Hooks {
	std::atomic<uint32_t> mask {0};
	std::atomic<int>      changes {0}, samples {0};
	MetricsHooks          Make() {
        MetricsHooks hooks;
        hooks.subscribed = [this](MetricMask keys) {
            mask = keys;
            changes++;
        };
        hooks.sample = [this](MetricMask keys) {
            samples++;
            MetricValues values;
            values.Set(Metric::GameFps, 60.0);
            values.Set(Metric::FrameTimeMs, 16.6);
            values.Set(Metric::GuestMemoryMb, 1024.0);
            values.Set(Metric::ShaderCompiles, 7);
            (void)keys;
            return values;
        };
        return hooks;
	}
};

static Json SubscribePacket(std::initializer_list<const char*> keys, int interval) {
	Json list = Json::array();
	for (auto key: keys)
		list.push_back(key);
	return {{"type", "metrics.subscribe"}, {"keys", list}, {"interval_ms", interval}};
}

static void TestUnknownPacketsIgnoredWithPool() {
	Listener           listener;
	Hooks              hooks;
	std::atomic<int>   resets {0};
	auto               transport = std::make_unique<Transport>(
        listener.path, [&](const Input* i) { if (!i) resets++; }, [](uint64_t) { return true; },
        Transport::MotionSink {}, hooks.Make());
	listener.Accept();
	// Before the pool is accepted an unknown type is ignored, not fatal.
	Check(SendPacket(listener.peer, {{"type", "welcome"}, {"version", 1}}), "welcome");
	Check(Wait([&] { return transport->Generation() > 0; }), "welcome observed");
	Check(SendPacket(listener.peer, {{"type", "future.thing"}, {"x", 1}}),
	      "unknown type before attach");
	Check(SendPacket(listener.peer, {{"type", "other"}}), "second unknown type before attach");
	auto ring = std::make_shared<LeaseRing>(9);
	PoolDescriptor descriptor;
	descriptor.epoch = 9;
	descriptor.width = 1920;
	descriptor.height = 1080;
	descriptor.stride = 8192;
	descriptor.allocation_sizes = {8847360, 8847360, 8847360};
	descriptor.gpu_uuid = "00112233445566778899aabbccddeeff";
	std::array<int, 9> fds;
	for (auto& fd: fds)
		fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
	transport->SetPool(descriptor, fds, ring, transport->Generation());
	Json hello;
	Check(listener.Next("hello", hello, 3000, 9) && hello["epoch"] == 9, "hello with nine fds");
	Check(!hello.contains("frames"), "a pool hello does not carry the frames marker");
	Check(SendPacket(listener.peer, {{"type", "accepted"}, {"version", 1}, {"epoch", 9},
	                                 {"transport", "vulkan-buffer-fd"}}),
	      "accept");
	Check(Wait([&] { return transport->Attached(); }), "attached");
	Check(SendPacket(listener.peer, {{"type", "future.thing"}}), "unknown type while attached");
	// Subscribing while attached works and a pool-mode metrics message arrives.
	Check(SendPacket(listener.peer, SubscribePacket({"game_fps", "bogus", "frame_time_ms"}, 250)),
	      "subscribe");
	Json metrics;
	Check(listener.Next("metrics", metrics), "metrics arrive in pool mode");
	Check(metrics["values"].size() == 2 && metrics["values"]["game_fps"] == 60.0 &&
	          metrics["values"]["frame_time_ms"] == 16.6 && metrics["t_ms"].is_number_unsigned(),
	      "only the subscribed catalog keys are sent");
	Check(hooks.mask == (MetricBit(Metric::GameFps) | MetricBit(Metric::FrameTimeMs)),
	      "subscription reaches the emulator hook with unknown keys dropped");
	Check(listener.Silent("metrics", 600), "interval 250 is clamped to the 1000 ms minimum");
	Check(listener.Next("metrics", metrics, 1500), "next message after the interval");
	// Known types keep their handling: a motion packet without the capability drops the link.
	Check(SendPacket(listener.peer, {{"type", "motion"}}), "motion without capability");
	Check(Wait([&] { return !transport->Attached(); }), "known gated type still disconnects");
	Check(Wait([&] { return hooks.mask == 0; }), "disconnect clears the subscription");
	Check(resets >= 1, "disconnect resets input");
	Check(!ring->TryAcquire(), "pool ring released");
	transport.reset();
	for (int fd: fds)
		close(fd);
}

static void TestUnsubscribeStopsMetrics() {
	Listener listener;
	Hooks    hooks;
	auto     transport = std::make_unique<Transport>(
        listener.path, [](const Input*) {}, [](uint64_t) { return true; }, Transport::MotionSink {},
        hooks.Make(), true);
	listener.Accept();
	Check(SendPacket(listener.peer, {{"type", "welcome"}, {"version", 1}}), "welcome");
	Json hello;
	Check(listener.Next("hello", hello), "metrics-only hello");
	Check(hello["version"] == 1 && hello["frames"] == false && hello.size() == 3,
	      "hello is marked frames:false and carries no pool fields");
	Check(SendPacket(listener.peer, SubscribePacket({"shader_compiles", "guest_memory_mb"}, 1000)),
	      "subscribe");
	Json metrics;
	Check(listener.Next("metrics", metrics), "first metrics immediately");
	Check(metrics["values"].size() == 2 && metrics["values"]["shader_compiles"] == 7 &&
	          metrics["values"]["shader_compiles"].is_number_unsigned() &&
	          metrics["values"]["guest_memory_mb"] == 1024.0,
	      "values for the subscribed keys");
	Check(hooks.mask == (MetricBit(Metric::ShaderCompiles) | MetricBit(Metric::GuestMemoryMb)),
	      "hook sees the key set");
	// Frame and controller packets mean nothing here; they must not end the connection.
	Check(SendPacket(listener.peer, {{"type", "accepted"}, {"version", 1}, {"epoch", 1},
	                                 {"transport", "vulkan-buffer-fd"}}),
	      "accepted");
	Check(SendPacket(listener.peer, {{"type", "release"}, {"epoch", 1}, {"slot", 0}, {"sequence", 1}}),
	      "release");
	Check(SendPacket(listener.peer, {{"type", "future.thing"}}), "unknown");
	Check(listener.Next("metrics", metrics, 2500), "still serving metrics after stray packets");
	// A malformed subscription leaves the old one in force.
	Check(SendPacket(listener.peer, {{"type", "metrics.subscribe"}, {"keys", 3}}), "malformed");
	Check(listener.Next("metrics", metrics, 2500), "malformed subscribe is ignored");
	Check(hooks.mask != 0, "old subscription kept");
	// An empty list stops everything.
	Check(SendPacket(listener.peer, SubscribePacket({}, 1000)), "empty subscribe");
	Check(Wait([&] { return hooks.mask == 0; }), "hook told to stop collecting");
	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	while (listener.Next("metrics", metrics, 20)) {
	} // Drain anything sent before the change landed.
	const int samples = hooks.samples;
	Check(listener.Silent("metrics", 1300), "an empty key list sends nothing");
	Check(hooks.samples == samples, "and the source is not read");
	// A resubscribe works on the same connection.
	Check(SendPacket(listener.peer, SubscribePacket({"game_fps"}, 1000)), "resubscribe");
	Check(listener.Next("metrics", metrics, 2500) && metrics["values"].size() == 1,
	      "metrics resume");
	close(listener.peer);
	listener.peer = -1;
	Check(Wait([&] { return hooks.mask == 0; }), "disconnect clears collection");
	transport.reset();
	Check(hooks.mask == 0, "destruction leaves no collection on");
}

static void TestMetricsOnlyAfterPoolFailure() {
	Listener listener;
	Hooks    hooks;
	auto     transport = std::make_unique<Transport>(
        listener.path, [](const Input*) {}, [](uint64_t) { return true; }, Transport::MotionSink {},
        hooks.Make());
	listener.Accept();
	transport->SetMetricsOnly(); // The pool could not be created.
	Check(SendPacket(listener.peer, {{"type", "welcome"}, {"version", 1}}), "welcome");
	Json hello;
	Check(listener.Next("hello", hello) && hello["frames"] == false, "metrics-only hello");
	transport.reset();
}

int main() {
	TestEnvSelection();
	TestCatalog();
	TestSubscribeParsing();
	TestMetricsMessage();
	TestUnknownTypeLog();
	TestCounterOverlaySplit();
	TestUnknownPacketsIgnoredWithPool();
	TestUnsubscribeStopsMetrics();
	TestMetricsOnlyAfterPoolFailure();
	std::puts("SessionMetricsTests passed");
}
