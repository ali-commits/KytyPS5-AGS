#include "receiver.h"
#include "usbSink.h"

#include <bit>
#include <charconv>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <thread>
using namespace ControllerAudio;
using namespace ControllerAudio::Receiver;
namespace {
uint32_t Number(const char* value, uint32_t max) {
	uint32_t         n = 0;
	std::string_view s(value);
	auto             result = std::from_chars(s.data(), s.data() + s.size(), n);
	if (result.ec != std::errc {} || result.ptr != s.data() + s.size() || !n || n > max)
		throw std::runtime_error("invalid bounded integer");
	return n;
}
volatile std::sig_atomic_t reference_stopped = 0;
void                       StopReference(int) {
	reference_stopped = 1;
}
int Reference(const Config& config, uint32_t seconds, Kind kind) {
	reference_stopped = 0;
	std::signal(SIGINT, StopReference);
	std::signal(SIGTERM, StopReference);
	UsbSink sink(MakeSdlUsbOutput(config));
	struct Cleanup {
		UsbSink& sink;
		~Cleanup() { sink.Reset(); }
	} cleanup {sink};
	if (!sink.Prepare()) throw std::runtime_error("USB reference unavailable");
	Header h {Type::Open, kind, 2, 1, 1, 0, 48000, 1, 1, 0, 0};
	if (!sink.Open(h)) throw std::runtime_error("reference open failed");
	auto     until  = NowUs() + uint64_t(seconds) * 1000000;
	uint64_t sample = 0;
	while (!reference_stopped && NowUs() < until) {
		std::vector<uint8_t> bytes;
		bytes.reserve(240 * 8);
		for (unsigned i = 0; i < 240; i++, sample++) {
			float v = .02f * std::sin(2 * 3.141592653589793 * 220 * double(sample) / 48000);
			auto  b = std::bit_cast<uint32_t>(v);
			for (int ch = 0; ch < 2; ch++)
				for (int j = 0; j < 4; j++)
					bytes.push_back(b >> (8 * j));
		}
		if (!sink.Pcm(h, bytes, NowUs()))
			throw std::runtime_error("reference PCM failed at frame " +
			                         std::to_string(sample - 240) + ": " + sink.Error());
		if (!sink.Tick(NowUs()))
			throw std::runtime_error("reference service failed after frame " +
			                         std::to_string(sample) + ": " + sink.Error());
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	sink.Reset();
	return 0;
}
} // namespace
int main(int argc, char** argv) {
	try {
		if (argc == 3 && std::string(argv[1]) == "--generate-identity") {
			GenerateIdentity(argv[2]);
			return 0;
		}
		if (argc == 1 || (argc == 2 && std::string(argv[1]) == "--help")) {
			std::puts("controller-audio-receiver --generate-identity "
			          "NEW_PRIVATE_DIR\ncontroller-audio-receiver --config FILE [--sink null|usb] "
			          "[--run-seconds 1..86400] [--diagnostics] [--raw "
			          "NEW_FILE]\ncontroller-audio-receiver --config FILE --reference-seconds 1..3 "
			          "[--reference-kind speaker|actuator]\n--status prints bounded local config "
			          "only, never opens hardware. USB is opened only after authenticated session "
			          "preparation.");
			return 0;
		}
		std::string config_path, sink, raw, reference_kind = "speaker";
		uint32_t    duration = 0, reference = 0;
		bool        diagnostics = false, status = false;
		for (int i = 1; i < argc; i++) {
			std::string arg = argv[i];
			if (arg == "--diagnostics") {
				diagnostics = true;
				continue;
			}
			if (arg == "--status") {
				status = true;
				continue;
			}
			if (i + 1 >= argc) throw std::runtime_error("option needs value");
			auto* value = argv[++i];
			if (arg == "--reference-kind")
				reference_kind = value;
			else if (arg == "--config")
				config_path = value;
			else if (arg == "--sink")
				sink = value;
			else if (arg == "--raw")
				raw = value;
			else if (arg == "--run-seconds")
				duration = Number(value, 86400);
			else if (arg == "--reference-seconds")
				reference = Number(value, 3);
			else
				throw std::runtime_error("unknown option");
		}
		if (config_path.empty()) throw std::runtime_error("--config required");
		auto config        = ReadConfig(config_path);
		config.run_seconds = duration;
		config.raw_path    = raw;
		config.diagnostics = diagnostics;
		if (!sink.empty()) config.sink = sink;
		if (config.sink != "null" && config.sink != "usb")
			throw std::runtime_error("unsupported sink");
		if (!raw.empty() && (!diagnostics || config.sink != "null"))
			throw std::runtime_error("raw requires --diagnostics --sink null");
		if (status) {
			std::printf("receiver_id=%.64s sink=%.8s configured=true hardware_open=false\n",
			            config.receiver_id.c_str(), config.sink.c_str());
			return 0;
		}
		if (reference) {
			if (config.sink != "usb")
				throw std::runtime_error("reference requires explicit --sink usb");
			if (reference_kind != "speaker" && reference_kind != "actuator")
				throw std::runtime_error("unsupported reference kind");
			return Reference(config, reference,
			                 reference_kind == "speaker" ? Kind::Speaker : Kind::Actuator);
		}
		return Run(config);
	} catch (const std::exception& error) {
		std::fprintf(stderr, "controller-audio-receiver: %.128s\n", error.what());
		return 1;
	}
}
