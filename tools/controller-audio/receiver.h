#pragma once
#include "controller_audio/protocol.h"

#include <array>
#include <functional>
#include <memory>
#include <string>
namespace ControllerAudio::Receiver {
uint64_t NowUs();
struct ClockMap {
	int64_t  offset      = 0;
	uint64_t uncertainty = UINT64_MAX, best_rtt = UINT64_MAX;
	bool     Sample(uint64_t sent, uint64_t receive, uint64_t server_receive, uint64_t server_send);
	bool     Fresh(uint64_t now) const;
	uint64_t sampled_at = 0, best_sample_at = 0;
};
class Sink {
public:
	virtual ~Sink()                                                             = default;
	virtual bool     Prepare()                                                  = 0;
	virtual bool     Open(const Header&)                                        = 0;
	virtual bool     Pcm(const Header&, std::span<const uint8_t>, uint64_t now) = 0;
	virtual bool     Close(uint32_t)                                            = 0;
	virtual void     Reset()                                                    = 0;
	virtual bool     Rumble(uint16_t, uint16_t, uint32_t, uint64_t)             = 0;
	virtual bool     Tick(uint64_t)                                             = 0;
	virtual uint64_t QueuedUs(uint32_t)                                         = 0;
};
class NullSink final: public Sink {
public:
	explicit NullSink(const std::string& raw_path = "", bool diagnostics = false);
	~NullSink() override;
	bool        Prepare() override;
	bool        Open(const Header&) override;
	bool        Pcm(const Header&, std::span<const uint8_t>, uint64_t) override;
	bool        Close(uint32_t) override;
	void        Reset() override;
	bool        Rumble(uint16_t, uint16_t, uint32_t, uint64_t) override;
	bool        Tick(uint64_t) override;
	uint64_t    QueuedUs(uint32_t) override;
	std::string Hash(Kind) const;
	uint64_t    Bytes(Kind) const;

private:
	struct Data;
	std::unique_ptr<Data> data_;
};
class Session {
public:
	explicit Session(Sink& sink): sink_(sink) {}
	bool               Handle(const Frame&, uint64_t now);
	void               Disconnect();
	bool               Tick(uint64_t now);
	bool               Ready() const { return ready_; }
	uint64_t           Generation() const { return generation_; }
	uint64_t           Epoch() const { return epoch_; }
	uint64_t           QueuedUs();
	uint64_t           Bytes() const { return bytes_; }
	const std::string& Error() const { return error_; }
	ClockMap           clock;

private:
	struct Port {
		bool     open = false;
		Header   h {};
		uint64_t next = 0;
	};
	bool                 Fail(const char*);
	Sink&                sink_;
	std::array<Port, 32> ports_ {};
	uint64_t             bytes_      = 0;
	uint64_t             generation_ = 0, epoch_ = 0, control_sequence_ = 0, last_generation_ = 0,
	                     last_epoch_ = 0;
	bool                 ready_ = false, active_ = false;
	std::string          error_;
};
class Framer {
public:
	bool   Feed(std::span<const uint8_t>, const std::function<bool(const Frame&)>&);
	void   Clear() { bytes_.clear(); }
	size_t Buffered() const { return bytes_.size(); }

private:
	std::vector<uint8_t> bytes_;
};
struct Config {
	bool        diagnostics = false;
	std::string host, pinned_host_sha256, cert, key, receiver_id, sink = "null", usb_path,
	                                                              audio_name, raw_path;
	uint16_t    port        = 48219;
	uint32_t    run_seconds = 0;
};
Config ReadConfig(const std::string& path);
void   GenerateIdentity(const std::string& directory);
int    Run(const Config&);
} // namespace ControllerAudio::Receiver
