// rdaccel_record: capture -> tile diff -> HEVC encode -> .rdstream file.
//
//   rdaccel_record --out=a.rdstream [--source=synthetic|screen] [--frames=300]
//                  [--size=1920x1080] [--fps=60] [--bitrate=8000000] [--encoder=libx265]
//
// The synthetic source is deterministic and runs as fast as the encoder
// allows, so the same command gives the same stream on every machine.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <memory>
#include <mutex>
#include <thread>

#include "args.h"
#include "capture/synthetic_capturer.h"
#include "io/stream_file.h"
#include "platform.h"
#include "session/host_session.h"

using namespace rdaccel;

namespace {

void PrintStats(const session::HostStats& s, double seconds, int fps) {
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
  tools::Args args(argc, argv);
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

  io::StreamFileWriter file;
  std::string err;
  if (!file.Open(out, &err)) {
    std::fprintf(stderr, "%s\n", err.c_str());
    return 1;
  }

  codec::EncoderConfig cfg;
  cfg.fps = fps;
  cfg.bitrate_bps = args.num("bitrate", 8'000'000);
  std::unique_ptr<session::HostSession> host;
  std::string encoder_name;
  auto ensure_host = [&](int fw, int fh) -> bool {
    if (host) return true;
    cfg.width = fw;
    cfg.height = fh;
    auto enc = platform::CreateHevcEncoder(cfg, args.str("encoder"), &err);
    if (!enc) return false;
    encoder_name = enc->name();
    host = std::make_unique<session::HostSession>(&file, std::move(enc), fw, fh);
    return true;
  };

  const auto t0 = std::chrono::steady_clock::now();
  if (source == "synthetic") {
    if (!ensure_host(w, h)) {
      std::fprintf(stderr, "%s\n", err.c_str());
      return 1;
    }
    std::vector<uint8_t> px;
    for (uint64_t i = 0; i < frames; ++i) {
      capture::CapturedFrame f;
      f.dirty = capture::RenderSyntheticFrame(i, w, h, px);
      f.bgra = {px.data(), w, h, w * 4};
      // Content time, not wall time: frame i is shown at i / fps.
      f.capture_time_us = i * 1'000'000ull / static_cast<uint64_t>(fps);
      host->OnFrame(f);
    }
  } else if (source == "screen") {
    auto cap = platform::CreateScreenCapturer(&err);
    if (!cap) {
      std::fprintf(stderr, "%s\n", err.c_str());
      return 1;
    }
    std::mutex mu;
    std::condition_variable cv;
    std::atomic<uint64_t> seen{0};
    bool failed = false;
    capture::CaptureConfig cc;
    cc.fps = fps;
    const bool started = cap->Start(cc, [&](const capture::CapturedFrame& f) {
      if (seen >= frames) return;
      if (!ensure_host(f.bgra.width, f.bgra.height)) {
        std::lock_guard<std::mutex> lock(mu);
        failed = true;
        seen = frames;
        cv.notify_all();
        return;
      }
      host->OnFrame(f);
      if (++seen >= frames) cv.notify_all();
    });
    if (!started) {
      std::fprintf(stderr, "capture failed: %s\n", cap->last_error().c_str());
      return 1;
    }
    std::unique_lock<std::mutex> lock(mu);
    cv.wait(lock, [&] { return seen >= frames; });
    lock.unlock();
    cap->Stop();
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

  std::printf("platform        : %s\n", platform::PlatformName());
  std::printf("encoder         : %s\n", encoder_name.c_str());
  std::printf("source          : %s %dx%d @ %d fps\n", source.c_str(), cfg.width, cfg.height, fps);
  PrintStats(host->stats(), secs, fps);
  std::printf("wrote           : %s\n", out.c_str());
  return 0;
}
