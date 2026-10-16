#pragma once
#include "receiver.h"

#include <map>
namespace ControllerAudio::Receiver {
using UsbReport = std::array<uint8_t, 48>;
UsbReport          SpeakerReport(bool enabled);
UsbReport          RumbleReport(uint16_t large, uint16_t small);
std::vector<float> MapQuad(Kind, std::span<const uint8_t>);
struct UsbDevice {
	std::string usb_path, hid_path, audio_name;
};
struct UsbNode {
	std::string usb_path, path;
};
std::optional<UsbDevice> ResolveUsbBinding(const std::vector<std::string>& controllers,
                                           const std::vector<UsbNode>&     hids,
                                           const std::vector<UsbNode>&     audio,
                                           const std::string&              usb_selector   = "",
                                           const std::string&              audio_selector = "");
std::optional<std::pair<uint32_t, uint32_t>> ParseAlsaPlaybackNode(const std::string&);
std::optional<std::string> ExplicitAlsaPlaybackSpec(const std::string& card_id, uint32_t device);
struct AlsaPlayback {
	uint64_t buffer_frames = 0, period_frames = 0, played_frames = 0, latency_us = 0;
};
std::optional<AlsaPlayback> ParseAlsaPlayback(const std::string& parameters,
                                              const std::string& status, uint32_t owner_pid,
                                              uint32_t sdl_period);
bool                        IsOwnedReport(const UsbReport&);
// Physical boundary is injected in tests; null output never constructs it.
class UsbOutput {
public:
	virtual ~UsbOutput()                                               = default;
	virtual bool        Prepare()                                      = 0;
	virtual bool        Open(uint32_t, uint32_t)                       = 0;
	virtual bool        Write(uint32_t, std::span<const float>, float) = 0;
	virtual uint64_t    QueuedUs(uint32_t)                             = 0;
	virtual uint64_t    PlaybackLatencyUs() { return 0; }
	virtual uint64_t    PlayedUs() { return 0; }
	virtual std::string Error() const { return {}; }
	virtual bool        Report(const UsbReport&) = 0;
	virtual void        Close(uint32_t)          = 0;
	virtual void        Shutdown()               = 0;
	virtual bool        Alive()                  = 0;
};
class UsbSink final: public Sink {
public:
	explicit UsbSink(std::unique_ptr<UsbOutput> output): output_(std::move(output)) {}
	~UsbSink() override;
	bool               Prepare() override;
	bool               Open(const Header&) override;
	bool               Pcm(const Header&, std::span<const uint8_t>, uint64_t) override;
	bool               Close(uint32_t) override;
	void               Reset() override;
	bool               Rumble(uint16_t, uint16_t, uint32_t, uint64_t) override;
	bool               Tick(uint64_t) override;
	uint64_t           QueuedUs(uint32_t) override;
	uint64_t           PendingFrames(uint32_t port, uint32_t rate) override;
	uint64_t           PlaybackLatencyUs() override { return output_->PlaybackLatencyUs(); }
	uint64_t           PlayedUs() override { return output_->PlayedUs(); }
	static float       FrequencyRatio(uint64_t queued_us);
	const std::string& Error() const { return error_; }

private:
	bool                       Apply(uint64_t);
	bool                       Fail(const char*, const std::string&);
	std::string                error_;
	std::unique_ptr<UsbOutput> output_;
	std::map<uint32_t, Header> ports_;
	uint64_t                   pcm_until_ = 0, rumble_until_ = 0;
	uint16_t                   large_ = 0, small_ = 0;
	UsbReport                  last_ {};
	bool                       prepared_ = false;
};
std::unique_ptr<UsbOutput> MakeSdlUsbOutput(const Config&);
} // namespace ControllerAudio::Receiver
