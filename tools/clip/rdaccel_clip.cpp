// rdaccel_clip: test clips of a screen (tools/clip/clip.h), and how the host
// does on them. A clip is played into a HostSession and a ViewerSession the
// way the Host runs them on a real screen, so two versions of rdaccel can be
// compared on exactly the same screen content. See docs/clips.md.
//
//   rdaccel_clip record OUT.clip [--seconds=60] [--display=0] [--fps=60]
//       Records the screen as the capturer delivers it (only frames that
//       changed), losslessly. Needs the Screen Recording permission.
//   rdaccel_clip render OUT.clip --scenario=NAME [--background=SCREEN.ppm] [--seconds=20]
//       One of the example scenes (examples/scenes.h: idle typing scroll drag
//       zoom video pause) as a clip, at 60 Hz.
//   rdaccel_clip eval CLIP [--text=0] [--bitrate=8000000] [--label=NAME] [--csv=FRAMES.csv]
//       Sends the clip through a host and a viewer with the platform's video
//       encoder and decoder, as the Host does it: each frame when the screen
//       changed, and the last one every 100 ms while something is owed.
//       Prints one line: what was sent (video, lossless), the share of
//       pixels the viewer shows exactly and its PSNR (weighted by time), and
//       for the pauses (0.5 s or more without a change) how much is exact at
//       their end. --csv writes what each frame sent.
//   rdaccel_clip frame CLIP --index=N --out=FRAME.ppm
//       Frame N of the clip as a PPM, to look at what was recorded.
//
// eval works on the internals (session::HostSession), not the public API, so
// it sees every frame the host decides on; the numbers are what the Host
// sends on a fast path (no congestion, a fixed bitrate).

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "args.h"
#include "clip.h"
#include "platform.h"
#include "scenes.h"
#include "session/host_session.h"
#include "session/viewer_session.h"
#include "transport/local_pipe.h"

using namespace rdacore;
using rdaccel::clip::ClipReader;
using rdaccel::clip::ClipWriter;
using rdaccel::examples::Args;

namespace {

int Usage() {
  std::fprintf(stderr,
               "usage: rdaccel_clip record OUT.clip [--seconds=60] [--display=0] [--fps=60]\n"
               "       rdaccel_clip render OUT.clip --scenario=NAME [--background=SCREEN.ppm] [--seconds=20]\n"
               "       rdaccel_clip eval CLIP [--text=0] [--bitrate=8000000] [--label=NAME] [--csv=FRAMES.csv]\n"
               "       rdaccel_clip frame CLIP --index=N --out=FRAME.ppm\n");
  return 2;
}

int Record(const Args& args) {
  ClipWriter out(args.positional().c_str());
  if (!out.ok()) {
    std::fprintf(stderr, "cannot write %s\n", args.positional().c_str());
    return 1;
  }
  std::string err;
  auto cap = platform::CreateScreenCapturer(&err);
  if (!cap) {
    std::fprintf(stderr, "%s\n", err.c_str());
    return 1;
  }
  std::mutex mu;
  uint64_t frames = 0, bytes = 0;
  capture::CaptureConfig cfg;
  cfg.display_index = static_cast<int32_t>(args.num("display", 0));
  cfg.fps = static_cast<int32_t>(args.num("fps", 60));
  if (!cap->Start(cfg, [&](const capture::CapturedFrame& f) {
        std::lock_guard<std::mutex> lock(mu);
        bytes += out.Write(f.capture_time_us, f.bgra.data, f.bgra.width, f.bgra.height, f.bgra.stride, f.dirty);
        ++frames;
      })) {
    std::fprintf(stderr, "capture: %s\n", cap->last_error().c_str());
    return 1;
  }
  const long long seconds = args.num("seconds", 60);
  for (long long s = 1; s <= seconds; ++s) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    if (s % 15 == 0 && s < seconds) {
      std::lock_guard<std::mutex> lock(mu);
      std::fprintf(stderr, "%llds: %llu frames, %.1f MB\n", s, static_cast<unsigned long long>(frames), bytes / 1e6);
    }
  }
  cap->Stop();
  std::lock_guard<std::mutex> lock(mu);
  std::printf("recorded %llu frames, %.1f MB\n", static_cast<unsigned long long>(frames), bytes / 1e6);
  return 0;
}

int Render(const Args& args) {
  using namespace rdaccel::examples;
  Image bg;
  int w = 1920, h = 1080;
  const std::string bg_path = args.str("background");
  if (!bg_path.empty()) {
    if (!LoadPpm(bg_path, bg, w, h)) {
      std::fprintf(stderr, "cannot read %s\n", bg_path.c_str());
      return 1;
    }
  } else {
    rdaccel::RenderTestPattern(600, w, h, bg);
  }
  const Scene scene{&bg, w, h};
  const Scenario* sc = nullptr;
  for (const auto& s : kScenarios) {
    if (args.str("scenario") == s.name) sc = &s;
  }
  if (!sc) {
    std::fprintf(stderr, "unknown --scenario (idle typing scroll drag zoom video pause pointer)\n");
    return 2;
  }
  ClipWriter out(args.positional().c_str());
  if (!out.ok()) return 1;
  const auto frames = static_cast<uint64_t>(args.num("seconds", 20) * 60);
  Image img, last;
  uint64_t written = 0;
  for (uint64_t i = 0; i < frames; ++i) {
    Render(scene, sc->fn, i, img);
    if (i > 0 && img == last) continue;  // a screen source sends nothing then
    last = img;
    out.Write(1'000'000 + i * 1'000'000 / 60, img.data(), w, h, w * 4, {});
    ++written;
  }
  std::printf("%s: %llu of %llu frames\n", sc->name, static_cast<unsigned long long>(written),
              static_cast<unsigned long long>(frames));
  return 0;
}

int Frame(const Args& args) {
  ClipReader clip(args.positional().c_str());
  const long long want = args.num("index", 0);
  long long i = -1;
  while (i < want && clip.Next()) ++i;
  if (i < 0) {
    std::fprintf(stderr, "no frames\n");
    return 1;
  }
  std::FILE* f = std::fopen(args.str("out", "frame.ppm").c_str(), "wb");
  if (!f) return 1;
  std::fprintf(f, "P6 %d %d 255\n", clip.width(), clip.height());
  const auto& px = clip.frame();
  for (size_t k = 0; k < px.size(); k += 4) {
    const uint8_t rgb[3] = {px[k + 2], px[k + 1], px[k]};
    std::fwrite(rgb, 1, 3, f);
  }
  std::fclose(f);
  std::printf("frame %lld of the clip\n", i);
  return 0;
}

int Eval(const Args& args) {
  ClipReader clip(args.positional().c_str());
  if (!clip.ok()) {
    std::fprintf(stderr, "cannot read %s\n", args.positional().c_str());
    return 1;
  }
  const bool text = args.num("text", 0) != 0;
  const int64_t bitrate = args.num("bitrate", 8'000'000);
  const std::string label = args.str("label", args.positional());
  constexpr uint64_t kResubmitUs = 100'000;  // as the Host's idle loop
  constexpr uint64_t kPauseUs = 500'000;

  auto [host_end, viewer_end] = transport::MakeLocalPipe();
  std::string err;
  auto decoder = platform::CreateHevcDecoder(&err);
  if (!decoder) {
    std::fprintf(stderr, "decoder: %s\n", err.c_str());
    return 1;
  }
  session::ViewerSession viewer(nullptr, std::move(decoder));
  std::mutex mu;
  std::condition_variable cv;
  std::vector<uint8_t> shown;
  uint64_t shown_id = 0;
  viewer.SetFrameHandler([&](const tile::ImageView& img, const std::vector<Rect>&, uint64_t id, uint64_t) {
    std::lock_guard<std::mutex> lock(mu);
    shown.resize(static_cast<size_t>(img.width) * img.height * 4);
    for (int32_t y = 0; y < img.height; ++y) {
      std::memcpy(shown.data() + static_cast<size_t>(y) * img.width * 4, img.data + static_cast<size_t>(y) * img.stride,
                  static_cast<size_t>(img.width) * 4);
    }
    shown_id = id;
    cv.notify_all();
  });

  std::unique_ptr<session::HostSession> host;
  std::vector<uint8_t> screen;
  int32_t W = 0, H = 0;
  uint64_t first_t = 0, last_submit = 0, frames = 0, resent = 0, missed = 0;
  // Quality weighted by time: what the viewer shows from one frame to the next.
  double w_exact = 0, w_psnr = 0, w_time = 0, last_exact = 0, last_psnr = 0;
  uint64_t last_q_t = 0;
  // Each clip frame opens a stretch until the next one; those of kPauseUs or
  // more are pauses. Samples: (time, exact %) within it.
  struct Stretch {
    uint64_t start;
    std::vector<std::pair<uint64_t, double>> samples;
  };
  std::vector<Stretch> stretches;

  std::FILE* csv = nullptr;
  if (args.has("csv")) {
    csv = std::fopen(args.str("csv").c_str(), "w");
    if (csv) std::fprintf(csv, "t_ms,resent,dirty_px,video_b,lossless_b,lossless_rects,moved_tiles,cached_tiles,owed_tiles,video_frame,exact_pct\n");
  }

  auto measure = [&](uint64_t t) {
    // Wait (briefly) until the viewer shows what the host sent. Some frames
    // never reach the screen as their own picture; then what the viewer
    // shows meanwhile is what is measured.
    const uint64_t want = host->next_frame_id() - 1;
    std::unique_lock<std::mutex> lock(mu);
    if (!cv.wait_for(lock, std::chrono::milliseconds(400), [&] { return shown_id >= want; })) ++missed;
    if (shown.size() != screen.size()) return;
    if (last_q_t) {
      const double dt = static_cast<double>(t - last_q_t);
      w_exact += last_exact * dt;
      w_psnr += last_psnr * dt;
      w_time += dt;
    }
    uint64_t same = 0;
    double se = 0;
    for (size_t i = 0; i < screen.size(); i += 4) {
      const int d0 = shown[i] - screen[i], d1 = shown[i + 1] - screen[i + 1], d2 = shown[i + 2] - screen[i + 2];
      same += (d0 | d1 | d2) == 0;
      se += d0 * d0 + d1 * d1 + d2 * d2;
    }
    const double px = static_cast<double>(screen.size() / 4);
    last_exact = 100.0 * static_cast<double>(same) / px;
    const double mse = se / (px * 3);
    last_psnr = mse == 0 ? 99.0 : std::min(99.0, 10 * std::log10(255.0 * 255.0 / mse));
    last_q_t = t;
    if (!stretches.empty()) stretches.back().samples.push_back({t, last_exact});
  };
  auto submit = [&](uint64_t t, const std::vector<Rect>& dirty, bool resend) {
    const auto before = host->stats();
    host->OnFrame({{screen.data(), W, H, W * 4}, dirty, t});
    last_submit = t;
    measure(t);
    if (csv) {
      const auto a = host->stats();
      int64_t dpx = 0;
      for (const auto& r : dirty) dpx += static_cast<int64_t>(r.w) * r.h;
      std::fprintf(csv, "%.0f,%d,%lld,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%.2f\n", (t - first_t) / 1e3, resend ? 1 : 0,
                   static_cast<long long>(dpx), static_cast<unsigned long long>(a.video_bytes - before.video_bytes),
                   static_cast<unsigned long long>(a.lossless_bytes - before.lossless_bytes),
                   static_cast<unsigned long long>(a.lossless_tiles - before.lossless_tiles),
                   static_cast<unsigned long long>(a.moved_tiles - before.moved_tiles),
                   static_cast<unsigned long long>(a.cached_tiles - before.cached_tiles),
                   static_cast<unsigned long long>(a.progressive_tiles + a.refreshed_tiles - before.progressive_tiles -
                                                   before.refreshed_tiles),
                   static_cast<unsigned long long>(a.video_frames - before.video_frames), last_exact);
    }
  };

  bool attached = false;
  uint64_t clip_end = 0;
  while (clip.Next()) {
    if (!host) {
      W = clip.width();
      H = clip.height();
      first_t = clip.time_us();
      codec::EncoderConfig ec{W, H, 60, bitrate, 24};
      auto enc = platform::CreateHevcEncoder(ec, "", &err);
      if (!enc) {
        std::fprintf(stderr, "encoder: %s\n", err.c_str());
        return 1;
      }
      session::HostOptions o;
      o.text_mode = text;
      o.context_tiles = text;
      o.context_tiles_64 = text;
      o.fps = 60;
      o.bitrate_bps = bitrate;
      o.region_encoder_factory = [](const codec::EncoderConfig& c, std::string* e) {
        return platform::CreateHevcEncoder(c, "", e);
      };
      host = std::make_unique<session::HostSession>(host_end.get(), std::move(enc), W, H, o);
    } else if (clip.width() != W || clip.height() != H) {
      std::fprintf(stderr, "the clip changes size at %.1f s: not supported, stopping there\n",
                   (clip.time_us() - first_t) / 1e6);
      break;
    }
    if (!attached) {
      viewer.Attach(viewer_end.get());
      attached = true;
    }
    // The Host's idle loop between changes.
    while (frames > 0 && clip.time_us() - last_submit >= kResubmitUs &&
           (host->keyframe_pending() || host->work_pending())) {
      submit(last_submit + kResubmitUs, {}, true);
      ++resent;
    }
    screen = clip.frame();
    stretches.push_back({clip.time_us(), {}});
    submit(clip.time_us(), clip.dirty(), false);
    ++frames;
    clip_end = clip.time_us();
  }
  if (!host) {
    std::fprintf(stderr, "no frames\n");
    return 1;
  }
  // What is still owed once the clip ends (up to 3 s).
  for (int k = 0; k < 30 && (host->keyframe_pending() || host->work_pending()); ++k) {
    submit(last_submit + kResubmitUs, {}, true);
    ++resent;
  }
  const uint64_t end_t = std::max(last_submit, clip_end) + kResubmitUs;
  if (last_q_t) {
    const double dt = static_cast<double>(end_t - last_q_t);
    w_exact += last_exact * dt;
    w_psnr += last_psnr * dt;
    w_time += dt;
  }
  std::vector<double> pause_exact;
  for (size_t k = 0; k < stretches.size(); ++k) {
    const uint64_t next = k + 1 < stretches.size() ? stretches[k + 1].start : end_t;
    if (next - stretches[k].start < kPauseUs || stretches[k].samples.empty()) continue;
    pause_exact.push_back(stretches[k].samples.back().second);
  }
  double pe = 0;
  for (double v : pause_exact) pe += v;
  const auto s = host->stats();
  const double secs = static_cast<double>(end_t - first_t) / 1e6;
  std::printf("%-22s %-6s %6.1f s %5llu frames +%4llu re-sent | sent %8.0f kB %6.0f kbps (video %7.0f, lossless %7.0f kB) | "
              "exact %5.1f%% PSNR %5.1f | %zu pauses: exact %5.1f%%\n",
              label.c_str(), text ? "saver" : "normal", secs, static_cast<unsigned long long>(frames),
              static_cast<unsigned long long>(resent), s.bytes_sent / 1e3, s.bytes_sent * 8 / 1e3 / std::max(secs, 0.001),
              s.video_bytes / 1e3, s.lossless_bytes / 1e3, w_time > 0 ? w_exact / w_time : 0.0,
              w_time > 0 ? w_psnr / w_time : 0.0, pause_exact.size(), pause_exact.empty() ? 0.0 : pe / pause_exact.size());
  if (missed > 0) {
    std::printf("  (%llu frames without a picture of their own on the viewer; measured on what it showed)\n",
                static_cast<unsigned long long>(missed));
  }
  if (csv) std::fclose(csv);
  viewer.Stop();
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) return Usage();
  const std::string cmd = argv[1];
  const Args args(argc - 1, argv + 1);
  if (args.positional().empty()) return Usage();
  if (cmd == "record") return Record(args);
  if (cmd == "render") return Render(args);
  if (cmd == "eval") return Eval(args);
  if (cmd == "frame") return Frame(args);
  return Usage();
}
