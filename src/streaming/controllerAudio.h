#pragma once
#include "controller_audio/pcm.h"
#include "controller_audio/protocol.h"

#include <string>
namespace Streaming::ControllerAudio {
struct Endpoint {
	std::string path, nonce_hex;
	uint64_t    generation, producer_epoch;
};
void Configure(const Endpoint&, bool ready);
void Disable() noexcept;
bool Ready() noexcept;
struct Status {
	bool     ready;
	uint64_t bytes, discontinuities;
};
Status GetStatus() noexcept;
void   OpenPort(uint32_t id, uint64_t port_generation, ::ControllerAudio::Kind, uint32_t rate);
bool   Publish(uint32_t id, uint64_t port_generation, ::ControllerAudio::Kind,
               const ::ControllerAudio::PcmView&) noexcept;
void   ClosePort(uint32_t id, uint64_t port_generation) noexcept;
} // namespace Streaming::ControllerAudio
