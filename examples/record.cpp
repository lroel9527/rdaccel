// rdaccel_record: capture -> encode -> .rdstream recording, using only the
// public API (rdaccel/rdaccel.hpp).
//
//   rdaccel_record --out=a.rdstream [--source=synthetic|screen] [--frames=300]
//                  [--size=1920x1080] [--fps=60] [--bitrate=8000000] [--encoder=libx265]
//
// The synthetic source is deterministic and runs as fast as the encoder
// allows, so the same command gives the same stream on every machine.

#include <rdaccel/rdaccel.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <vector>

#include "args.h"

namespace {

void PrintStats(const rdaccel::HostStats& s, double seconds, int fps) {
  const double video_seconds = static_cast<double>(s.frames_captured) / fps;
  std::printf("frames captured : %llu\n", static_cast<unsigned long long>(s.frames_captured));
  std::printf("frames skipped  : %llu (no tile changed)\n",
              static_cast<unsigned long long>(s.frames_skipped));
  std::printf("frames encoded  : %llu (%llu keyframes)\n",
              static_cast<unsigned long long>(s.frames_encoded),
              static_cast<unsigned long long>(s.keyframes));
  std::printf("bytes           : %llu\n", static_cast<unsigned long long>(s.bytes_sent));
  if (video_seconds > 0) {
    std::printf("bitrate         : %.1f kbps over %.1f s of content\n",
                s.bytes_sent * 8.0 / 1000.0 / video_seconds, video_seconds);
  }
  if (s.frames_encoded > 0) {
    std::printf("encode time     : %.2f ms/frame avg\n",
                s.encode_us_total / 1000.0 / static_cast<double>(s.frames_encoded));
  }
  std::printf("wall time       : %.1f s\n", seconds);
}

}  // namespace

int main(int argc, char** argv) {
  rdaccel::examples::Args args(argc, argv);
  const std::string out = args.str("out");
  if (out.empty()) {
    std::fprintf(stderr,
                 "usage: rdaccel_record --out=FILE [--source=synthetic|screen] [--frames=N]\n"
                 "       [--size=WxH] [--fps=N] [--bitrate=BPS] [--encoder=NAME]\n");
    return 2;
  }
  const std::string source = args.str("source", "synthetic");
  const auto frames = static_cast<uint64_t>(args.num("frames", 300));
  const int fps = static_cast<int>(args.num("fps", 60));
  int w = 1920, h = 1080;
  args.size("size", w, h);

  std::string err;
  auto file = rdaccel::CreateRecordingTransport(out, &err);
  if (!file) {
    std::fprintf(stderr, "%s\n", err.c_str());
    return 1;
  }

  rdaccel::HostConfig cfg;
  cfg.fps = fps;
  cfg.bitrate_bps = args.num("bitrate", 8'000'000);
  cfg.encoder = args.str("encoder");
  rdaccel::Host host(std::move(file), cfg);

  const auto t0 = std::chrono::steady_clock::now();
  if (source == "synthetic") {
    std::vector<uint8_t> px;
    for (uint64_t i = 0; i < frames; ++i) {
      const auto dirty = rdaccel::RenderTestPattern(i, w, h, px);
      // Content time, not wall time: frame i is shown at i / fps.
      const uint64_t t = i * 1'000'000ull / static_cast<uint64_t>(fps);
      if (!host.SubmitFrame({px.data(), w, h, w * 4}, t, dirty, &err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 1;
      }
    }
  } else if (source == "screen") {
    std::mutex mu;
    std::condition_variable cv;
    bool failed = false;
    host.SetErrorHandler([&](rda_status s, const std::string& e) {
      if (s == RDA_ERR_UNSUPPORTED) return;  // no cursor channel: keeps recording
      std::lock_guard<std::mutex> lock(mu);
      err = e;
      failed = true;
      cv.notify_all();
    });
    if (!host.StartScreenCapture(&err)) {
      std::fprintf(stderr, "capture failed: %s\n", err.c_str());
      return 1;
    }
    std::unique_lock<std::mutex> lock(mu);
    // Poll the frame count; errors wake the wait right away.
    while (!failed && host.stats().frames_captured < frames) {
      cv.wait_for(lock, std::chrono::milliseconds(50));
    }
    lock.unlock();
    host.StopScreenCapture();
    if (failed) {
      std::fprintf(stderr, "%s\n", err.c_str());
      return 1;
    }
  } else {
    std::fprintf(stderr, "unknown --source=%s\n", source.c_str());
    return 2;
  }
  const double secs =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

  std::printf("platform        : %s\n", rdaccel::PlatformName());
  std::printf("encoder         : %s\n", host.encoder_name().c_str());
  std::printf("source          : %s @ %d fps\n", source.c_str(), fps);
  PrintStats(host.stats(), secs, fps);
  std::printf("wrote           : %s\n", out.c_str());
  if (host.stats().frames_encoded == 0) {
    std::fprintf(stderr, "error: no frame was encoded\n");
    return 1;
  }
  return 0;
}
