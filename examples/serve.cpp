// rdaccel_serve: the host side across a real network. Waits for one viewer
// (rdaccel_view) and streams the screen, or the built-in test pattern, to
// it. Prints one line of statistics per second.
//
//   rdaccel_serve [--udp=PORT | --tcp=PORT] [--source=screen|synthetic] [--seconds=0]
//                 (--key=<64 hex digits> | --plaintext)
//                 [--size=WxH] [--fps=60] [--bitrate=8000000]
//
// UDP (the default, port 7300) has congestion control and adaptive bitrate;
// TCP sends at the configured bitrate. --size is the test pattern's size, or
// with --source=screen the size the screen is captured at (default: native). --seconds=0 runs until the viewer
// disconnects or the process is killed. Uses only the public API.

#include <rdaccel/rdaccel.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "args.h"

namespace {

std::atomic<bool> g_stop{false};

void PrintLine(const rdaccel::HostStats& now, const rdaccel::HostStats& before, double secs) {
  std::printf("%6.1fs  %3llu fps (%llu dropped for congestion)  %7.0f kbps  target %6.0f kbps  "
              "estimate %6.0f kbps  rtt %3u ms  cursor %llu\n",
              secs, static_cast<unsigned long long>(now.frames_encoded - before.frames_encoded),
              static_cast<unsigned long long>(now.frames_dropped_congestion - before.frames_dropped_congestion),
              (now.bytes_sent - before.bytes_sent) * 8 / 1000.0, now.target_bitrate_bps / 1000.0,
              now.bandwidth_estimate_bps / 1000.0, now.rtt_us / 1000,
              static_cast<unsigned long long>(now.cursor_updates - before.cursor_updates));
  std::fflush(stdout);
}

}  // namespace

int main(int argc, char** argv) {
  rdaccel::examples::Args args(argc, argv);
  const bool tcp = args.has("tcp");
  const auto port = static_cast<uint16_t>(args.num(tcp ? "tcp" : "udp", 7300));
  const std::string source = args.str("source", "screen");
  const double seconds = args.real("seconds", 0);
  const int fps = static_cast<int>(args.num("fps", 60));
  int w = 1920, h = 1080;
  args.size("size", w, h);
  std::signal(SIGINT, [](int) { g_stop = true; });
  std::signal(SIGTERM, [](int) { g_stop = true; });

  std::printf("waiting for a viewer on %s port %u ...\n", tcp ? "TCP" : "UDP", port);
  std::fflush(stdout);
  std::string err;
  // Both return at once; the state handler below reports when the viewer
  // has connected.
  auto transport = tcp ? rdaccel::BindTcp(port, &err) : rdaccel::BindUdp(port, &err);
  if (!transport) {
    std::fprintf(stderr, "%s\n", err.c_str());
    return 1;
  }

  rdaccel::HostConfig cfg;
  if (!args.key("key", cfg.encryption_key)) return 2;
  cfg.allow_plaintext = args.has("plaintext");
  if (cfg.encryption_key.empty() && !cfg.allow_plaintext) {
    std::fprintf(stderr, "pass --key=<64 hex digits> (the same on both sides) or --plaintext\n");
    return 2;
  }
  cfg.fps = fps;
  cfg.bitrate_bps = args.num("bitrate", 8'000'000);
  if (source == "screen" && args.has("size")) {
    // Capture scaled to this size (the cursor image is scaled to match).
    cfg.capture_width = w;
    cfg.capture_height = h;
  }
  rdaccel::Host host(std::move(transport), cfg);
  std::atomic<bool> failed{false};
  host.SetErrorHandler([&](const std::string& e) {
    std::fprintf(stderr, "host: %s\n", e.c_str());
    // A missing cursor is not fatal; anything else stops the stream.
    if (e.rfind("cursor", 0) != 0) failed = true;
  });
  std::atomic<bool> viewer_left{false};
  host.SetStateHandler([&](rdaccel::ConnectionState st, rda_status reason) {
    if (st == rdaccel::ConnectionState::kConnected) std::printf("viewer said hello\n");
    if (st == rdaccel::ConnectionState::kDisconnected) {
      std::printf("viewer left: %s\n", rda_status_str(reason));
      viewer_left = true;
    }
    std::fflush(stdout);
  });
  host.SetMessageHandler([](uint32_t type, std::span<const uint8_t> p) {
    std::printf("message %u from the viewer, %zu bytes\n", type, p.size());
  });
  host.SetInputHandler([](const rdaccel::InputEvent& ev) {
    // A real application would inject the event into the OS here.
    (void)ev;
  });

  if (source == "screen") {
    if (!host.StartScreenCapture(&err)) {
      std::fprintf(stderr, "screen capture: %s\n", err.c_str());
      return 1;
    }
  } else if (source != "synthetic") {
    std::fprintf(stderr, "unknown --source=%s\n", source.c_str());
    return 2;
  }

  const auto t0 = std::chrono::steady_clock::now();
  auto next_print = t0 + std::chrono::seconds(1);
  rdaccel::HostStats last;
  std::vector<uint8_t> px;
  for (uint64_t i = 0; !g_stop && !failed && !viewer_left; ++i) {
    const auto now = std::chrono::steady_clock::now();
    const double elapsed = std::chrono::duration<double>(now - t0).count();
    if (seconds > 0 && elapsed >= seconds) break;
    if (source == "synthetic") {
      const auto dirty = rdaccel::RenderTestPattern(i, w, h, px);
      if (!host.SubmitFrame({px.data(), w, h, w * 4}, rdaccel::NowMicros(), dirty, &err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 1;
      }
      std::this_thread::sleep_until(t0 + std::chrono::microseconds((i + 1) * 1'000'000 / fps));
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (std::chrono::steady_clock::now() >= next_print) {
      next_print += std::chrono::seconds(1);
      const auto st = host.stats();
      PrintLine(st, last, elapsed);
      last = st;
    }
  }
  host.StopScreenCapture();
  std::printf("encoder %s, %llu frames, %.1f MB sent\n", host.encoder_name().c_str(),
              static_cast<unsigned long long>(host.stats().frames_encoded), host.stats().bytes_sent / 1e6);
  return failed ? 1 : 0;
}
