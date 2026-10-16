#include "receiver.h"
#include "usbSink.h"

#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
using namespace ControllerAudio;
using namespace ControllerAudio::Receiver;
void Check(bool yes, const char* message) {
	if (!yes) {
		std::fprintf(stderr, "FAIL %s\n", message);
		std::exit(1);
	}
}
std::vector<uint8_t> Samples(std::initializer_list<float> values) {
	std::vector<uint8_t> out;
	for (float v: values) {
		auto b = std::bit_cast<uint32_t>(v);
		for (int i = 0; i < 4; i++)
			out.push_back(b >> (i * 8));
	}
	return out;
}
struct Fake final: UsbOutput {
	int                    prepares = 0, opens = 0, shutdowns = 0, closes = 0;
	bool                   alive = true, fail = false;
	uint64_t               queued = 0, latency = 0;
	std::vector<UsbReport> reports;
	std::vector<float>     pcm;
	bool                   Prepare() override {
		prepares++;
		return true;
	}
	bool Open(uint32_t, uint32_t) override {
		opens++;
		return true;
	}
	bool Write(uint32_t, std::span<const float> p, float ratio) override {
		pcm.assign(p.begin(), p.end());
		Check(ratio >= .995f && ratio <= 1.005f, "servo bounded");
		return !fail;
	}
	uint64_t QueuedUs(uint32_t) override { return queued; }
	uint64_t PlaybackLatencyUs() override { return latency; }
	bool     Report(const UsbReport& r) override {
		reports.push_back(r);
		return !fail;
	}
	void Close(uint32_t) override { closes++; }
	void Shutdown() override { shutdowns++; }
	bool Alive() override { return alive; }
};
Header Port(Type t, Kind k = Kind::Actuator) {
	return {t, k, 2, 7, 9, 0, 48000, 1, 1, 0, 1000000};
}
Frame State(uint64_t gen = 7, uint64_t epoch = 9) {
	std::string s = "{\"state\":\"preparing\"}";
	return {{Type::State, Kind::Control, 0, gen, epoch, 0, 0, 0, 1, 0, 0}, {s.begin(), s.end()}};
}
Frame Active(uint64_t gen = 7, uint64_t epoch = 9) {
	auto        f    = State(gen, epoch);
	std::string text = "{\"state\":\"active\"}";
	f.payload.assign(text.begin(), text.end());
	return f;
}
int main() {
	auto bound = ResolveUsbBinding({"/usb/3-1"}, {{"/usb/3-1", "/dev/hidraw7"}},
	                               {{"/usb/3-1", "Wireless Controller:USB Audio"}});
	Check(bound && bound->hid_path == "/dev/hidraw7", "dynamic same-parent USB binding");
	Check(!ResolveUsbBinding({"/usb/3-1"}, {{"/usb/4-2", "/dev/hidraw7"}}, {{"/usb/3-1", "audio"}}),
	      "unrelated hid refused");
	Check(!ResolveUsbBinding({"/usb/3-1", "/usb/4-2"}, {{"/usb/3-1", "hid"}},
	                         {{"/usb/3-1", "audio"}}),
	      "multiple controllers ambiguous");
	Check(!ResolveUsbBinding({"/usb/3-1"}, {{"/usb/3-1", "hid"}},
	                         {{"/usb/3-1", "a"}, {"/usb/3-1", "b"}}),
	      "multiple playback endpoints refused");
	auto stereo = Samples({.25f, -.5f});
	Check(MapQuad(Kind::Speaker, stereo) == std::vector<float>({.25f, -.5f, 0, 0}),
	      "speaker front map");
	Check(MapQuad(Kind::Actuator, stereo) == std::vector<float>({0, 0, .25f, -.5f}),
	      "actuator rear map");
	auto route = SpeakerReport(true);
	Check(route.size() == 48 && route[0] == 2 && route[1] == 0xa0 && route[2] == 0x80 &&
	          route[6] == 100 && route[8] == 0x30 && route[38] == 2,
	      "full USB speaker report");
	for (size_t i = 0; i < route.size(); i++)
		if (i != 0 && i != 1 && i != 2 && i != 6 && i != 8 && i != 38)
			Check(route[i] == 0, "no trigger/LED writes");
	Check(IsOwnedReport(route), "constructed speaker report accepted");
	auto injected = route;
	injected[11]  = 1;
	Check(!IsOwnedReport(injected), "trigger injection rejected");
	injected    = route;
	injected[5] = 1;
	Check(!IsOwnedReport(injected), "headphone injection rejected");
	auto reset = SpeakerReport(false);
	Check(reset[0] == 2 && reset[1] == 0x80 && reset[2] == 0x80 && reset[38] == 0,
	      "routing reset masks");
	auto rumble = RumbleReport(65535, 32768);
	Check(rumble[1] == 3 && rumble[3] == 64 && rumble[4] == 127,
	      "classic rumble matches SDL scaling");
	auto    routing_fake = std::make_unique<Fake>();
	auto*   rf           = routing_fake.get();
	UsbSink routing(std::move(routing_fake));
	Check(routing.Prepare(), "routing prepare");
	Check(routing.Rumble(65535, 0, 1000, NowUs()), "routing rumble cache");
	auto speaker = Port(Type::Open, Kind::Speaker);
	Check(routing.Open(speaker), "speaker mid rumble");
	Check(rf->reports.back()[1] == 3 && rf->reports.back()[4] == 127,
	      "speaker mode report reapplies cached ordinary rumble");
	routing.Close(0);
	Check(rf->reports.back()[1] == 3, "speaker reset reapplies cached ordinary rumble");
	routing.Reset();
	auto    fake = std::make_unique<Fake>();
	auto*   f    = fake.get();
	UsbSink usb(std::move(fake));
	Check(f->prepares == 0, "sink construction stays silent");
	Check(usb.Prepare(), "prepare fake");
	auto h = Port(Type::Open);
	Check(usb.Open(h), "open actuator");
	Check(usb.Rumble(65535, 32768, 400, 1000000), "ordinary rumble accepted");
	Check(f->reports.back()[4] == 127, "rumble before PCM");
	h.type = Type::Pcm;
	Check(usb.Pcm(h, stereo, 1000000), "fake PCM");
	Check(f->pcm == std::vector<float>({0, 0, .25f, -.5f}), "fake boundary quad");
	Check(f->reports.back()[1] == 0, "PCM restores audio haptics");
	Check(usb.Tick(1260000), "tick after PCM");
	Check(f->reports.back()[4] == 127, "cached rumble resumes");
	Check(usb.Tick(1400001), "expiry");
	Check(f->reports.back()[4] == 0, "rumble expires");
	f->latency = 12000;
	Check(usb.QueuedUs(0) == 12000, "deadline includes physical playback buffer");
	f->latency = 0;
	f->queued  = 21000;
	Check(!usb.Pcm(h, stereo, 1110000), "queue overflow explicit failure");
	f->queued = 0;
	Check(UsbSink::FrequencyRatio(0) >= .995f && UsbSink::FrequencyRatio(1000000) <= 1.005f,
	      "drift clamp");
	usb.Reset();
	Check(f->shutdowns > 0 && f->closes > 0, "reset closes device");
	// Silent PCM must not steal an ordinary effect; audible PCM holds native 250 ms.
	Check(usb.Prepare(), "reprepare");
	Check(usb.Open(h), "reopen");
	Check(usb.Rumble(65535, 0, 400, 2000000), "cached effect");
	Check(usb.Pcm(h, Samples({0, 0}), 2000000), "silent PCM");
	Check(f->reports.back()[4] == 127, "silent PCM preserves ordinary rumble");
	Check(usb.Pcm(h, stereo, 2010000), "audible PCM");
	Check(usb.Tick(2200000) && f->reports.back()[4] == 0, "native 250ms hold");
	f->alive = false;
	Check(!usb.Tick(2300000), "device loss");
	usb.Reset();
	// Simulated +/-1000ppm USB clocks run for ten minutes without overflowing 20ms.
	for (double physical: {.999, 1.001}) {
		double queued = 0;
		for (int block = 0; block < 60000; block++) {
			Check(queued + 10000 <= 20000, "slow clock stays under bounded queue");
			float ratio = UsbSink::FrequencyRatio(uint64_t(queued + 10000));
			queued      = std::max(0.0, queued + 10000 - 10000 * physical * ratio);
		}
		Check(queued < 10000, "servo reaches stable queue");
	}
	ClockMap refreshed;
	Check(refreshed.Sample(1000000, 1000100, 1000000, 1000000), "initial minimum RTT");
	for (uint64_t t = 2000000; t <= 14000000; t += 1000000)
		Check(refreshed.Sample(t, t + 200, t, t), "clock refresh");
	Check(refreshed.Fresh(14000300), "healthy clock does not expire with slightly slower RTT");
	ClockMap clock;
	Check(clock.Sample(1000, 3000, 2100, 2200), "clock sample");
	Check(clock.uncertainty == 950 && clock.offset == 150, "clock midpoint");
	NullSink silent;
	Check(silent.Hash(Kind::Actuator).empty(), "hashes require explicit diagnostics");
	NullSink null("", true);
	Session  session(null);
	Check(!session.Handle(State(), 1000000), "uncertain clock refuses Ready");
	session.clock.Sample(999000, 1000000, 999500, 999500);
	Check(session.Handle(State(), 1000000) && session.Ready(), "validated State readiness");
	auto early           = Port(Type::Open);
	early.source_time_us = 0;
	Session preactive(null);
	preactive.clock = session.clock;
	Check(preactive.Handle(State(), 1000000), "preparing accepted");
	Check(!preactive.Handle({early, {}}, 1000000), "Open before active rejected");
	Check(session.Handle(Active(), 1000000), "active state accepted");
	auto open           = Port(Type::Open);
	open.source_time_us = 0;
	Check(session.Handle({open, {}}, 1000000), "open session");
	auto pcm     = Port(Type::Pcm);
	pcm.sequence = 2;
	Check(session.Handle({pcm, stereo}, 1000000), "contiguous PCM");
	Check(null.Bytes(Kind::Actuator) == 8, "null byte counter");
	Check(null.Hash(Kind::Actuator).size() == 64, "hash diagnostic");
	pcm.sequence = 3;
	Check(!session.Handle({pcm, stereo}, 1000000) && !session.Ready(), "bad continuity revokes");
	Check(!session.Handle(State(), 1000000), "same failed lease cannot replay");
	Check(session.Handle(State(8, 10), 1000000), "new lease");
	Check(session.Handle(Active(8, 10), 1000000), "new active");
	open.generation     = 8;
	open.producer_epoch = 10;
	Check(session.Handle({open, {}}, 1000000), "new port");
	pcm.generation     = 8;
	pcm.producer_epoch = 10;
	pcm.sequence       = 2;
	pcm.source_time_us = 900000;
	Check(!session.Handle({pcm, stereo}, 1000000), "source deadline enforced");
	session.Disconnect();
	Check(!session.Ready(), "disconnect resets");
	Framer framer;
	int    frames = 0;
	auto   wire   = Encode({open, {}});
	for (auto b: wire)
		Check(framer.Feed({&b, 1},
		                  [&](const Frame&) {
			                  frames++;
			                  return true;
		                  }),
		      "partial header");
	Check(frames == 1, "one incremental frame");
	auto malformed = wire;
	malformed[12]  = 0xff;
	Check(!framer.Feed(malformed, [](const Frame&) { return true; }),
	      "header bounded before allocation");
	Session paths(null);
	paths.clock = session.clock;
	Check(paths.Handle(State(9, 11), 1000000) && paths.Handle(Active(9, 11), 1000000),
	      "close/reset setup");
	open.generation     = 9;
	open.producer_epoch = 11;
	Check(paths.Handle({open, {}}, 1000000), "close setup");
	auto close     = open;
	close.type     = Type::Close;
	close.sequence = 2;
	Check(paths.Handle({close, {}}, 1000000), "close flushes");
	open.sequence = 3;
	Check(!paths.Handle({open, {}}, 1000000), "recycled port generation cannot reopen");
	paths.clock = session.clock;
	Check(paths.Handle(State(10, 12), 1000000) && paths.Handle(Active(10, 12), 1000000),
	      "direction setup");
	Check(!paths.Handle({{Type::Hello, Kind::Control, 0, 10, 12, 0, 0, 0, 1, 0, 0}, {}}, 1000000),
	      "server Hello prohibited");
	std::puts("ControllerAudioReceiverTests passed (fake output / null only)");
}
