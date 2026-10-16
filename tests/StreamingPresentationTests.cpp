#include "graphics/presentation/window/presentationPolicy.h"

#include <array>
#include <cstdio>
#include <cstdlib>

using namespace Libs::Graphics;

namespace {
void Check(bool value, const char *message) {
  if (!value) {
    std::fprintf(stderr, "StreamingPresentationTests FAIL: %s\n", message);
    std::exit(1);
  }
}
} // namespace

namespace {
void VisibilityKeepsCurrentRemotePress() {
  struct Visibility {
    uint64_t session;
  };
  struct Input {
    uint64_t session;
    uint32_t pressed;
  };
  RemoteOverlayInputRouter router;
  const std::array<int, 6> neutral{128, 128, 128, 128, 0, 0};
  router.Route(0, true, 0, neutral);
  std::deque<Visibility> visibility{{1}, {2}};
  router.Route(1, true, 0, neutral);
  router.Route(2, true, 0, neutral);
  const auto accepted = router.Route(2, true, 0x4000, neutral);
  Check(accepted.guest_buttons == 0,
        "current modal Accept is captured from the guest");
  std::deque<Input> input{
      {2, accepted.changed_buttons & accepted.overlay_buttons}};
  Visibility latest{};
  while (ConsumeNewestOverlayVisibility(visibility, input, latest)) {
  }
  Check(
      input.size() == 1 && input.front().session == 2 &&
          input.front().pressed == 0x4000,
      "stale A visibility wake preserves exactly one current B Accept action");
  const auto held = router.Route(2, true, 0x4000, neutral);
  Check(held.changed_buttons == 0 && held.guest_buttons == 0,
        "holding captured Accept does not duplicate its action or reach the "
        "guest");
}

void ModalSourcesStayIndependent() {
  ModalControllerInput input;
  input.SetAxis(ModalInputSource::Local, 0, -0.8f);
  input.SetButton(ModalInputSource::Local, 7, true);
  input.SetAxis(ModalInputSource::Remote, 0, 0.0f);
  Check(input.Axis(0) == -0.8f && input.ButtonDown(7),
        "neutral remote snapshot preserves held local stick and Dpad");
  input.SetButton(ModalInputSource::Remote, 8, true);
  input.SetAxis(ModalInputSource::Remote, 1, 0.9f);
  input.Reset(ModalInputSource::Remote);
  Check(input.Axis(0) == -0.8f && input.ButtonDown(7) && !input.ButtonDown(8) &&
            input.Axis(1) == 0.0f,
        "remote disconnect clears remote holds while preserving local "
        "navigation");
  input.SetButton(ModalInputSource::Remote, 7, true);
  input.SetButton(ModalInputSource::Remote, 7, false);
  Check(input.ButtonDown(7),
        "remote button release cannot release a local hold of the same key");
  struct Event {
    uint64_t session;
    int kind;
    int id;
    ModalInputSource source;
  };
  const Event local{2, 1, 0, ModalInputSource::Local};
  const Event remote{2, 1, 0, ModalInputSource::Remote};
  Check(!CanCoalesceOverlayInput(local, remote),
        "axis coalescing preserves different sources");
  Check(CanCoalesceOverlayInput(remote, remote),
        "same-source latest axis remains coalescible");
}
} // namespace

int main(int argc, char **argv) {
  if (argc == 2 && std::strcmp(argv[1], "--sources-only") == 0) {
    ModalSourcesStayIndependent();
    return 0;
  }
  VisibilityKeepsCurrentRemotePress();
  ModalSourcesStayIndependent();
  Check(!StreamOnlyRequested(nullptr, nullptr),
        "old launches show their game window");
  Check(!StreamOnlyRequested("1", nullptr),
        "stream-only needs a stream endpoint");
  Check(!StreamOnlyRequested("1", ""),
        "empty endpoint preserves local presentation");
  Check(!StreamOnlyRequested("0", "/run/user/1000/stream.sock"),
        "explicit local mode");
  Check(!StreamOnlyRequested("true", "/run/user/1000/stream.sock"),
        "only exact opt-in");
  Check(StreamOnlyRequested("1", "/run/user/1000/stream.sock"),
        "valid stream-only opt-in");

  RemoteOverlayInputRouter router;
  constexpr uint32_t accept = 0x4000;
  const std::array<int, 6> neutral{128, 128, 128, 128, 0, 0};
  auto routed = router.Route(0, true, accept, neutral);
  Check(routed.guest_buttons == accept, "ordinary remote input reaches guest");
  routed = router.Route(1, true, accept, neutral);
  Check(routed.guest_buttons == 0 && routed.overlay_buttons == 0,
        "held Accept cannot enter a newly opened modal");
  routed = router.Route(1, true, 0, neutral);
  Check(routed.guest_buttons == 0, "modal captures neutral input");
  routed = router.Route(1, true, accept, neutral);
  Check(
      routed.overlay_buttons == accept && routed.changed_buttons == accept &&
          routed.guest_buttons == 0,
      "modal receives exactly one fresh Accept press and guest receives none");
  routed = router.Route(1, true, accept, neutral);
  Check(routed.changed_buttons == 0,
        "repeated snapshots do not repeat modal Accept");
  routed = router.Route(0, true, accept, neutral);
  Check(routed.guest_buttons == 0, "closing modal suppresses held Accept");
  routed = router.Route(0, true, accept, neutral);
  Check(routed.guest_buttons == 0, "release barrier persists across snapshots");
  routed = router.Route(0, true, 0, neutral);
  routed = router.Route(0, true, accept, neutral);
  Check(routed.guest_buttons == accept,
        "fresh Accept after release reaches guest");

  const std::array<int, 6> stick{255, 128, 128, 128, 0, 0};
  router.Route(2, true, 0, neutral);
  routed = router.Route(2, true, 0, stick);
  Check(routed.overlay_axes == stick && routed.guest_axes == neutral,
        "modal owns remote stick navigation");
  routed = router.Route(0, true, 0, stick);
  Check(routed.guest_axes == neutral,
        "stick must return to neutral after modal close");
  router.Route(0, true, 0, neutral);
  routed = router.Route(0, true, 0, stick);
  Check(routed.guest_axes == stick, "released stick returns to guest");
  routed = router.Route(2, false, accept, stick);
  Check(!routed.connected && routed.guest_buttons == 0 &&
            routed.guest_axes == neutral,
        "disconnect releases the remote source");
  routed = router.Route(2, true, accept, neutral);
  Check(routed.overlay_buttons == 0 && routed.guest_buttons == 0,
        "reconnecting with a held button also requires release");
  router.Route(2, false, 0, neutral);
  routed = router.Route(0, true, accept, neutral);
  Check(routed.guest_buttons == 0,
        "disconnect and modal close cannot bypass the release barrier");
  std::puts("StreamingPresentationTests PASS");
}
