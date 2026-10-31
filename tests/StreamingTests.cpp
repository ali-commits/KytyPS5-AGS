#include "libs/controllerSourceState.h"
#include "streaming/feedback.h"
#include "streaming/protocol.h"
#include "streaming/transport.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <latch>
#include <limits>
#include <poll.h>
#include <set>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
namespace Streaming {
struct LeaseRingTestAccess {
  static std::unique_lock<std::mutex> Hold(LeaseRing &ring) {
    return std::unique_lock(ring.m_mutex);
  }
};
} // namespace Streaming
using namespace Streaming;
static void Check(bool ok, const char *what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    std::abort();
  }
}
static Json Hello() {
  return {{"type", "hello"},
          {"version", 1},
          {"epoch", 1},
          {"width", 1920},
          {"height", 1080},
          {"stride", 8192},
          {"fourcc", "BGRA"},
          {"gpu_uuid", "00112233445566778899aabbccddeeff"},
          {"slots", 3},
          {"allocation_sizes", {8847360, 8847360, 8847360}},
          {"offset", 0},
          {"dedicated", true},
          {"transport", "vulkan-buffer-fd"}};
}
static void TestContendedReserveSkips() {
  LeaseRing ring(99);
  std::latch held(1), release(1);
  std::thread ipc([&] {
    auto lock = LeaseRingTestAccess::Hold(ring);
    held.count_down();
    release.wait();
  });
  held.wait();
  // The IPC owner cannot unlock until reserve returns. A blocking
  // implementation deadlocks deterministically; the test runner watchdog
  // catches that regression.
  const auto contended = ring.TryAcquire();
  release.count_down();
  ipc.join();
  Check(!contended,
        "contended reservation skips without waiting for IPC owner");
  const auto available = ring.TryAcquire();
  Check(available && available->sequence == 1 && available->slot == 0 &&
            !available->recycled,
        "skipped contention does not consume a slot or sequence");
}
static Json MotionPacket() {
	return {{"type", "motion"},      {"controller", 0}, {"sensor", "gyro"}, {"sequence", 1},
	        {"timestamp_us", 10000}, {"x", 3.14159265}, {"y", 0},           {"z", -1}};
}
static void TestMotionAndEffects() {
	Motion motion;
	auto   packet = MotionPacket();
	Check(ParseMotion(packet, motion) && motion.sensor == Motion::Sensor::Gyro &&
	          std::abs(motion.value[0] - 3.14159265f) < 0.00001f,
	      "motion preserves SI radians without a second conversion");
	for (auto invalid: {Json(nullptr), Json(true), Json("1"), Json(40.01),
	                    Json(std::numeric_limits<double>::infinity()),
	                    Json(std::numeric_limits<double>::quiet_NaN())}) {
		auto bad = packet;
		bad["x"] = invalid;
		Check(!ParseMotion(bad, motion), "reject nonfinite/out-of-range gyro");
	}
	for (auto key: {"controller", "sequence", "timestamp_us"}) {
		auto bad = packet;
		bad[key] = key == std::string("controller") ? Json(1) : Json(0);
		Check(!ParseMotion(bad, motion), "validate controller/order identity");
		bad[key] = 1.5;
		Check(!ParseMotion(bad, motion), "reject float identity");
	}
	packet["sensor"] = "accel";
	packet["x"]      = 160.0;
	Check(ParseMotion(packet, motion), "allow bounded acceleration");
	packet["x"] = 160.01;
	Check(!ParseMotion(packet, motion), "reject acceleration outside sensor range");
	MotionOrder order;
	Check(ParseMotion(MotionPacket(), motion) && order.Accept(motion), "first motion accepted");
	Check(!order.Accept(motion), "duplicate motion ignored");
	motion.sequence++;
	Check(!order.Accept(motion), "nonincreasing timestamp ignored");
	motion.timestamp_us++;
	Check(order.Accept(motion), "fresh sample accepted");
	order.Reset();
	Check(ParseMotion(MotionPacket(), motion) && order.Accept(motion),
	      "disconnect resets motion ordering");
	Check(!HasCapability(Json::object(), "motion-v1") &&
	          HasCapability({{"capabilities", {"motion-v1", "future"}}}, "motion-v1") &&
	          !HasCapability({{"capabilities", "motion-v1"}}, "motion-v1"),
	      "capability opt-in only");
	adaptive_enabled = true;
	ClearAdaptiveTriggers();
	std::array<uint8_t, 11> left {0x21, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
	std::array<uint8_t, 11> right {0x25, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1};
	PublishAdaptiveTriggers(0x08, left.data(), right.data());
	PublishAdaptiveTriggers(0x04, right.data(), right.data());
	auto effect = ReadAdaptiveTriggers();
	Check(effect.enable_bits == 0x0c && effect.left == left && effect.right == right,
	      "latest mailbox merges masked updates with all eleven effect bytes");
	ClearAdaptiveTriggers();
	effect = ReadAdaptiveTriggers();
	Check(effect.enable_bits == 0x0c && effect.left[0] == 5 && effect.right[0] == 5 &&
	          effect.left[10] == 0 && effect.right[10] == 0,
	      "reset disables and zeros both effects");
	adaptive_enabled = false;
}
int main() {
	TestMotionAndEffects();
  TestContendedReserveSkips();
  PoolDescriptor desc;
	auto           hello = Hello();
  Check(ValidateHello(hello, 9, desc), "valid hello");
  Check(!ValidateHello(hello, 8, desc), "reject missing fd");
	for (auto bad: {Json(-1), Json(0), Json(4097), Json("1920"), Json(1920.0)}) {
		auto p     = hello;
    p["width"] = bad;
    Check(!ValidateHello(p, 9, desc), "reject dimension type/range");
  }
  for (auto bad : {0, 7680, 8191}) {
    auto p = hello;
    p["stride"] = bad;
    Check(!ValidateHello(p, 9, desc), "reject unaligned pitch");
  }
  {
    auto p = hello;
    p["allocation_sizes"][1] = 8847359;
    Check(!ValidateHello(p, 9, desc), "reject short allocation");
    p["allocation_sizes"][1] = 268435457;
    Check(!ValidateHello(p, 9, desc), "reject oversized allocation");
  }
  {
    auto p = hello;
    p["gpu_uuid"] = "00112233445566778899AABBCCDDEEFF";
    Check(!ValidateHello(p, 9, desc), "reject UUID case");
    p = hello;
    p["dedicated"] = false;
    Check(!ValidateHello(p, 9, desc), "require dedicated");
  }
  LeaseRing ring(7);
  std::array<Lease, 3> leases{*ring.TryAcquire(), *ring.TryAcquire(),
                              *ring.TryAcquire()};
  Check(std::set<uint32_t>{leases[0].slot, leases[1].slot, leases[2].slot}
                .size() == 3,
        "distinct slots");
  Check(!ring.TryAcquire().has_value(), "three-slot exhaustion skips");
  Check(!ring.Release(6, leases[0].slot, leases[0].sequence),
        "old epoch cannot release");
  Check(!ring.Release(7, leases[0].slot, leases[0].sequence + 1),
        "stale sequence cannot release");
  Check(ring.Release(7, leases[0].slot, leases[0].sequence),
        "correct lease release");
  Check(!ring.Release(7, leases[0].slot, leases[0].sequence),
        "duplicate release ignored");
  auto reused = ring.TryAcquire();
  Check(reused && reused->recycled && reused->sequence > leases[2].sequence,
        "recycle tracks DONE wait");
  ring.Disconnect();
  Check(!ring.TryAcquire(), "disconnected consumer cannot acquire");
  Json input = {{"type", "input"},
                {"controller", 0},
                {"connected", true},
                {"sequence", 1},
                {"buttons", {"a", "up", "lb"}},
                {"lx", -32768},
                {"ly", 32767},
                {"rx", 0},
                {"ry", 0},
                {"lt", 0},
                {"rt", 255}};
  Input state;
  Check(ParseInput(input, state), "valid input");
  input["controller_type"]         = 2;
  input["controller_capabilities"] = 0x32;
  input["supported_buttons"]       = 0xffff;
  Check(ParseInput(input, state) && state.controller_type == 2 &&
	        state.controller_capabilities == 0x32 && state.supported_buttons == 0xffff,
	    "preserve controller arrival metadata");
  for (auto key: {"controller_type", "controller_capabilities", "supported_buttons"}) {
	  auto bad = input;
	  bad[key] = -1;
	  Check(!ParseInput(bad, state), "reject invalid arrival metadata");
  }
  Check(state.axes[0] == 0 && state.axes[1] == 255 && state.axes[2] == 128 &&
            state.axes[5] == 255,
        "normalized axes");
  for (auto key : {"lx", "ly", "rx", "ry"}) {
    auto p = input;
    p[key] = 32768;
    Check(!ParseInput(p, state), "stick range");
  }
  {
    auto p = input;
    p["buttons"] = {"a", "a"};
    Check(!ParseInput(p, state), "duplicate button");
    p["buttons"] = {"bogus"};
    Check(!ParseInput(p, state), "unknown button");
    p = input;
    p["rt"] = 256;
    Check(!ParseInput(p, state), "trigger range");
  }
  Libs::Controller::SourceState sources;
  sources.LocalButton(0x4000, true);
  sources.LocalAxis(0, 200);
  sources.Remote(0x6000, {0, 128, 128, 128, 0, 0});
  Check(sources.Buttons() == 0x6000 && sources.Axis(0) == 0,
        "remote merges local holds and latest axis");
  sources.ResetRemote();
  Check(sources.Buttons() == 0x4000 && sources.Axis(0) == 200,
        "remote reset preserves local holds");
  sources.Remote(0x2000, {0, 128, 128, 128, 0, 0});
  sources.LocalAxis(0, 220);
  sources.ResetRemote();
  Check(sources.Axis(0) == 220, "remote reset preserves newer local axis");
  {
    int sockets[2];
    Check(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets) == 0,
          "socketpair");
    Check(SameUser(sockets[0]), "same UID credentials");
    Check(SendPacket(sockets[0], {{"type", "welcome"}, {"version", 1}}),
          "send packet");
    {
      Packet packet;
      Check(ReceivePacket(sockets[1], packet) == ReceiveResult::Packet &&
                packet.json["version"] == 1,
            "receive packet");
    }
    std::array<int, 9> fds;
    for (auto &fd : fds)
      fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    Check(SendPacket(sockets[0], Hello(), &fds), "send pool FDs");
    std::vector<int> rejected;
    {
      Packet packet;
      Check(ReceivePacket(sockets[1], packet) == ReceiveResult::Invalid,
            "unexpected FDs rejected");
      rejected = packet.fds;
      Check(rejected.size() == 9, "all rights collected for closure");
    }
    for (int fd : rejected)
      Check(fcntl(fd, F_GETFD) == -1 && errno == EBADF, "rejected FD closed");
    // Empty datagrams may deliver SCM_RIGHTS; rejection must still close them.
    {
      alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))]{};
      msghdr message{};
      message.msg_control = control;
      message.msg_controllen = sizeof(control);
      auto *rights = CMSG_FIRSTHDR(&message);
      rights->cmsg_level = SOL_SOCKET;
      rights->cmsg_type = SCM_RIGHTS;
      rights->cmsg_len = CMSG_LEN(sizeof(int));
      std::memcpy(CMSG_DATA(rights), &fds[0], sizeof(int));
      Check(sendmsg(sockets[0], &message, 0) == 0,
            "send empty rights datagram");
      int received = -1;
      {
        Packet packet;
        Check(ReceivePacket(sockets[1], packet) == ReceiveResult::Invalid &&
                  packet.fds.size() == 1,
              "empty datagram rights retained for RAII rejection");
        received = packet.fds[0];
      }
      Check(fcntl(received, F_GETFD) == -1 && errno == EBADF,
            "empty datagram FD closed");
    }
    {
      alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int) * 32)]{};
      char json[] = "{}";
      iovec data{json, 2};
      msghdr message{};
      message.msg_iov = &data;
      message.msg_iovlen = 1;
      message.msg_control = control;
      message.msg_controllen = sizeof(control);
      auto *rights = CMSG_FIRSTHDR(&message);
      rights->cmsg_level = SOL_SOCKET;
      rights->cmsg_type = SCM_RIGHTS;
      rights->cmsg_len = CMSG_LEN(sizeof(int) * 32);
      std::array<int, 32> surplus;
      surplus.fill(fds[0]);
      std::memcpy(CMSG_DATA(rights), surplus.data(), sizeof(int) * 32);
      Check(sendmsg(sockets[0], &message, 0) == 2,
            "send truncated rights fixture");
      std::vector<int> imported;
      {
        Packet packet;
        Check(ReceivePacket(sockets[1], packet, 16) == ReceiveResult::Invalid &&
                  packet.fds.size() == 16,
              "MSG_CTRUNC rejected even when delivered FD count matches");
        imported = packet.fds;
      }
      for (int fd : imported)
        Check(fcntl(fd, F_GETFD) == -1 && errno == EBADF,
              "truncated ancillary FD closed");
    }
    const std::string too_large(8193, 'x');
    Check(send(sockets[0], too_large.data(), too_large.size(), 0) == 8193,
          "send oversized");
    {
      Packet packet;
      Check(ReceivePacket(sockets[1], packet) == ReceiveResult::Invalid,
            "truncated packet rejected");
    }
    for (int fd : fds)
      close(fd);
    close(sockets[0]);
    {
      Packet packet;
      Check(ReceivePacket(sockets[1], packet) == ReceiveResult::Closed,
            "peer closure");
    }
    close(sockets[1]);
  }
  for (bool extended: {false, true}) {
    char directory[] = "/tmp/kyty-stream-XXXXXX";
    Check(mkdtemp(directory) != nullptr, "private socket directory");
    const std::string path = std::string(directory) + "/feed.sock";
	  int listener           = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    Check(listener >= 0, "listener");
	  sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
	  Check(bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0 &&
              chmod(path.c_str(), 0600) == 0 && listen(listener, 1) == 0,
          "protected listener");
	  std::atomic<int>  inputs {0}, resets {0}, motions {0}, motion_resets {0};
	  std::atomic<bool> completed {false};
	  auto              producer = std::make_unique<Transport>(
        path,
		  [&](const Input* i) {
          if (i)
            inputs++;
          else
            resets++;
        },
		  [&](uint64_t tick) { return tick == 123 && completed.load(); },
		  [&](const Motion* m) {
			  if (m)
				  motions++;
			  else
				  motion_resets++;
		  });
    auto wait = [&](auto condition) {
      for (int i = 0; i < 1000; i++) {
			  if (condition()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
      }
      return false;
    };
    int peer = -1;
    Check(wait([&] {
			    peer = accept4(listener, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
            return peer >= 0;
          }),
          "worker connects");
	  auto welcome = Json {{"type", "welcome"}, {"version", 1}};
	  if (extended) welcome["capabilities"] = {"motion-v1", "adaptive-triggers-v1"};
	  Check(SendPacket(peer, welcome), "welcome");
    Check(wait([&] { return producer->Generation() > 0; }), "welcome observed");
	  auto           ring = std::make_shared<LeaseRing>(9);
    PoolDescriptor descriptor;
    Check(ValidateHello(Hello(), 9, descriptor), "fixture descriptor");
    descriptor.epoch = 9;
    std::array<int, 9> fds;
	  for (auto& fd: fds)
      fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    producer->SetPool(descriptor, fds, ring, producer->Generation());
    {
		  Packet        packet;
      ReceiveResult result = ReceiveResult::Again;
      Check(wait([&] {
              result = ReceivePacket(peer, packet, 9);
              return result != ReceiveResult::Again;
            }) &&
                result == ReceiveResult::Packet && packet.fds.size() == 9 &&
                packet.json["epoch"] == 9,
            "nine FD handshake");
		  Check(HasCapability(packet.json, "motion-v1") == extended &&
			        HasCapability(packet.json, "adaptive-triggers-v1") == extended,
			    "hello only opts in to advertised extensions");
    }
	  auto accepted = Json {
		  {"type", "accepted"}, {"version", 1}, {"epoch", 9}, {"transport", "vulkan-buffer-fd"}};
	  if (extended) accepted["capabilities"] = welcome["capabilities"];
	  Check(SendPacket(peer, accepted), "accept");
    Check(wait([&] { return producer->Attached(); }), "attached");
    auto lease = ring->TryAcquire();
    Check(lease.has_value(), "lease for async publish");
    producer->Publish({*lease, 123, 987});
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    {
      Packet packet;
      Check(ReceivePacket(peer, packet) == ReceiveResult::Again,
            "frame waits observable completion");
    }
    completed = true;
    {
		  Packet        packet;
      ReceiveResult result = ReceiveResult::Again;
      Check(wait([&] {
              result = ReceivePacket(peer, packet);
              return result != ReceiveResult::Again;
            }) &&
			        result == ReceiveResult::Packet && packet.json["type"] == "frame" &&
                packet.json["sequence"] == lease->sequence,
            "completed frame published");
    }
    input["sequence"] = 2;
    Check(SendPacket(peer, input), "input send");
    Check(wait([&] { return inputs == 1; }), "input dispatched");
    input["sequence"] = 1;
    Check(SendPacket(peer, input), "stale input send");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    Check(inputs == 1, "stale input ignored");
	  if (extended) {
		  Check(SendPacket(peer, MotionPacket()), "motion send");
		  Check(wait([&] { return motions == 1; }), "motion delivered");
		  Check(SendPacket(peer, MotionPacket()), "duplicate motion send");
		  std::this_thread::sleep_for(std::chrono::milliseconds(20));
		  Check(motions == 1, "stale motion ignored");
		  std::array<uint8_t, 11> left {0x21, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
		  std::array<uint8_t, 11> right {0x25, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1};
		  PublishAdaptiveTriggers(0x0c, left.data(), right.data());
		  Packet effect;
		  Check(wait([&] {
				    return ReceivePacket(peer, effect) == ReceiveResult::Packet &&
				           effect.json["type"] == "adaptive-triggers" &&
				           effect.json["left"] == left;
			    }),
			    "translated feedback delivered");
		  Check(effect.json["enable_bits"] == 12 && effect.json["right"] == right,
			    "feedback retains enable mask and full right payload");
		  const int previous = motion_resets;
		  Check(SendPacket(peer, {{"type", "reset"}, {"controller", 0}}), "reset send");
		  Check(wait([&] { return motion_resets > previous; }), "reset clears remote motion");
		  Packet clear;
		  Check(wait([&] {
				    return ReceivePacket(peer, clear) == ReceiveResult::Packet &&
				           clear.json["type"] == "adaptive-triggers";
			    }),
			    "reset feedback sent");
		  Check(clear.json["left"][0] == 5 && clear.json["right"][0] == 5 &&
			        clear.json["left"][10] == 0,
			    "reset disables and zeros effects");
	  } else {
		  std::array<uint8_t, 11> off {};
		  PublishAdaptiveTriggers(12, off.data(), off.data());
		  std::this_thread::sleep_for(std::chrono::milliseconds(20));
		  Packet packet;
		  Check(ReceivePacket(peer, packet) == ReceiveResult::Again,
			    "legacy peer receives no extension messages");
	  }
	  const int before        = resets;
	  const int motion_before = motion_resets;
    close(peer);
    Check(wait([&] { return !producer->Attached() && resets > before; }),
          "disconnect resets remote input");
	  Check(motion_resets > motion_before, "disconnect resets remote motion sink");
    Check(!ring->TryAcquire(), "disconnected lease pool disabled");
    producer.reset();
	  for (int fd: fds)
      close(fd);
    close(listener);
    unlink(path.c_str());
    rmdir(directory);
  }
  sources.ResetRemote();
  sources.Remote(0, {128, 128, 128, 128, 0, 255});
  Check((sources.Buttons() & 0x200) != 0, "analog trigger sets its button");
  sources.LocalAxis(5, 0);
  Check(sources.Axis(5) == 0 && (sources.Buttons() & 0x200) == 0,
        "newer local trigger zero overrides remote analog button");
  std::puts("StreamingTests passed");
}
