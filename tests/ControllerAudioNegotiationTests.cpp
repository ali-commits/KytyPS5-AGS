#ifdef NDEBUG
#undef NDEBUG
#endif
#include "streaming/protocol.h"

#include <cassert>
#include <cstdio>
int main() {
	Streaming::ControllerAudioState state;
	const std::string               nonce(64, 'a');
	Streaming::Json                 ready = {{"type", "controller-audio-state"},
	                                         {"ready", true},
	                                         {"generation", 1u},
	                                         {"producer_epoch", 2u},
	                                         {"socket", "/private/audio.sock"},
	                                         {"nonce", nonce}};
	assert(Streaming::ParseControllerAudioState(ready, state) && state.ready &&
	       state.generation == 1 && state.producer_epoch == 2);
	auto bad     = ready;
	bad["nonce"] = std::string(64, 'A');
	assert(!Streaming::ParseControllerAudioState(bad, state));
	bad               = ready;
	bad["generation"] = -1;
	assert(!Streaming::ParseControllerAudioState(bad, state));
	bad           = ready;
	bad["socket"] = "relative";
	assert(!Streaming::ParseControllerAudioState(bad, state));
	bad                   = ready;
	bad["producer_epoch"] = 0u;
	assert(!Streaming::ParseControllerAudioState(bad, state));
	assert(Streaming::ParseControllerAudioState(
	           {{"type", "controller-audio-state"}, {"ready", false}, {"generation", 2u}}, state) &&
	       !state.ready);
	std::puts("ControllerAudioNegotiationTests: all assertions passed");
}
