// rdaccel_netsim: a Host and a Viewer in one process, connected through a
// simulated network path, in real time. Measures what a user would notice:
// capture-to-display latency, frame rate, stalls and picture quality.
// Uses only the public API (rdaccel/rdaccel.hpp).
//
//   rdaccel_netsim [--background=SCREEN.ppm] [--scenario=video] [--seconds=10] [--fps=30]
//                  [--bandwidth=4000000] [--delay=20] [--jitter=0] [--loss=0] [--queue-ms=200]
//                  [--drop-to=BPS --drop-at=SECONDS]   bandwidth change mid-run
//                  [--bitrate=8000000] [--min-bitrate=300000] [--cc=1] [--abr=1] [--max-queue-ms=50] [--regions=1] [--progressive=1]
//                  [--warmup=3]   seconds left out of the steady-state numbers
//                  [--cursor=1]   also move the mouse pointer (cursor channel) and
//                                 report how long positions take to arrive
//
// The first picture (a full screen, mostly lossless) takes a while on a slow
// link; it is reported separately, and the other numbers cover the frames
// captured after --warmup seconds.
//
// --cc=0 sends without congestion control; --abr=0 keeps the encoder at
// --bitrate whatever the network does.

#include <rdaccel/rdaccel.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#include "args.h"
#include "scenes.h"

using namespace rdaccel::examples;

int main(int argc, char** argv) {
  rdaccel::examples::Args args(argc, argv);
  const int fps = static_cast<int>(args.num("fps", 30));
  const double seconds = args.real("seconds", 10);
  const std::string which = args.str("scenario", "video");

  Image bg;
  int w = 1920, h = 1080;
  const std::string bg_path = args.str("background");
  if (!bg_path.empty()) {
    if (!LoadPpm(bg_path, bg, w, h)) {
      std::fprintf(stderr, "cannot read %s (binary PPM expected)\n", bg_path.c_str());
      return 1;
    }
  } else {
    args.size("size", w, h);
    rdaccel::RenderTestPattern(600, w, h, bg);
  }
  const Scene scene{&bg, w, h};
  const Scenario* sc = nullptr;
  for (const auto& s : kScenarios) {
    if (which == s.name) sc = &s;
  }
  if (!sc) {
    std::fprintf(stderr, "unknown scenario %s\n", which.c_str());
    return 1;
  }

  rdaccel::NetworkConditions down;
  down.bandwidth_bps = args.num("bandwidth", 4'000'000);
  down.delay_ms = static_cast<int32_t>(args.num("delay", 20));
  down.jitter_ms = static_cast<int32_t>(args.num("jitter", 0));
  down.loss = args.real("loss", 0);
  down.queue_ms = static_cast<int32_t>(args.num("queue-ms", 200));
  if (args.num("drop-to", 0) > 0) {
    down.bandwidth_schedule.push_back(
        {static_cast<int64_t>(args.real("drop-at", 5) * 1000), args.num("drop-to", 0)});
  }
  rdaccel::NetworkConditions up = down;
  up.bandwidth_bps = 0;  // input and acknowledgements
  up.bandwidth_schedule.clear();
  up.seed = 2;
  auto [host_end, viewer_end] =
      rdaccel::CreateSimulatedNetwork(down, up, args.num("cc", 1) != 0);

  // Frames in flight: capture time -> scene frame index.
  std::mutex mu;
  std::map<uint64_t, uint64_t> index_of;
  std::vector<double> latency_ms, psnr;
  // Per second of capture time: frames shown and their worst latency (--verbose).
  std::map<uint64_t, std::pair<int, double>> by_second;
  std::vector<uint64_t> shown_at;
  // --trace=FILE: one CSV row per captured frame (host side), with the
  // latency at which the viewer showed it (-1 = never).
  struct TraceRow {
    double t_ms;
    uint64_t video_b, lossless_b, bytes_b;
    bool dropped, key;
    int64_t target, estimate;
    uint32_t rtt_us;
    double shown_ms = -1;
    double psnr = -1;   // of the picture shown for it
    double exact = -1;  // % of its pixels shown exactly
  };
  std::vector<TraceRow> trace;
  const std::string trace_path = args.str("trace", "");
  const bool tracing = !trace_path.empty();
  const auto warmup_frames = static_cast<uint64_t>(args.real("warmup", 3) * fps);
  double first_frame_ms = -1;
  std::vector<std::pair<double, double>> first_psnr;  // (ms since start, PSNR) during warm-up
  uint64_t start_us = rdaccel::NowMicros();          // reset when the first frame is captured
  Image ref;
  std::vector<double> cursor_ms;
  const bool with_cursor = args.num("cursor", 0) != 0;
  uint64_t steady_from_us = UINT64_MAX;  // host clock; set when warm-up ends
  rdaccel::ViewerHandlers handlers;
  handlers.on_cursor = [&](const rdaccel::CursorState& c) {
    const uint64_t now = rdaccel::NowMicros();
    std::lock_guard<std::mutex> lock(mu);
    if (c.time_us >= steady_from_us) cursor_ms.push_back((now - c.time_us) / 1000.0);
  };
  handlers.on_frame = [&](const rdaccel::ImageView& img, std::span<const rdaccel::Rect>,
                          const rdaccel::FrameInfo& info) {
    const uint64_t now = rdaccel::NowMicros();
    uint64_t index;
    {
      std::lock_guard<std::mutex> lock(mu);
      auto it = index_of.find(info.capture_time_us);
      if (it == index_of.end()) return;
      index = it->second;
      // From the first capture, like the start line (not this frame's own
      // latency: the first frames may never be shown).
      if (first_frame_ms < 0) first_frame_ms = (now - start_us) / 1000.0;
      auto& sec = by_second[index / static_cast<uint64_t>(fps)];
      ++sec.first;
      sec.second = std::max(sec.second, (now - info.capture_time_us) / 1000.0);
      if (index < trace.size()) trace[index].shown_ms = (now - info.capture_time_us) / 1000.0;
      if (index >= warmup_frames) {
        latency_ms.push_back((now - info.capture_time_us) / 1000.0);
        shown_at.push_back(now);
      }
    }
    // Quality on a sample of frames, to keep the viewer thread real time;
    // more often during warm-up, to see how fast the first picture sharpens.
    const bool warm = index < warmup_frames;
    const bool sampled = warm ? index % 5 == 0 || first_psnr.empty() : index % 10 == 0;
    // With --trace, every shown frame (the trace's psnr column).
    if (sampled || tracing) {
      Render(scene, sc->fn, index, ref);
      double se = 0;
      uint64_t exact = 0;  // pixels shown exactly (lossless, or untouched)
      for (int y = 0; y < img.height; ++y) {
        const uint8_t* a = img.data + static_cast<size_t>(y) * img.stride;
        const uint8_t* b = ref.data() + static_cast<size_t>(y) * w * 4;
        for (int x = 0; x < img.width; ++x) {
          int same = 1;
          for (int c = 0; c < 3; ++c) {
            const int d = a[x * 4 + c] - b[x * 4 + c];
            se += d * d;
            same &= d == 0;
          }
          exact += static_cast<uint64_t>(same);
        }
      }
      const double px = static_cast<double>(img.width) * img.height;
      const double mse = se / (px * 3);
      const double p = mse == 0 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
      std::lock_guard<std::mutex> lock(mu);
      if (index < trace.size()) {
        trace[index].psnr = p;
        trace[index].exact = 100.0 * static_cast<double>(exact) / px;
      }
      if (!sampled) return;
      if (warm) {
        first_psnr.push_back({(now - start_us) / 1000.0, p});
      } else {
        psnr.push_back(p);
      }
    }
  };
  std::string err;
  auto viewer = rdaccel::Viewer::Create(std::move(viewer_end), std::move(handlers), &err);
  if (!viewer) {
    std::fprintf(stderr, "%s\n", err.c_str());
    return 1;
  }

  rdaccel::HostConfig cfg;
  cfg.fps = fps;
  cfg.bitrate_bps = args.num("bitrate", 8'000'000);
  cfg.adaptive_bitrate = args.num("abr", 1) != 0;
  cfg.min_bitrate_bps = args.num("min-bitrate", cfg.min_bitrate_bps);
  cfg.lossless_max_colors_hot = static_cast<int32_t>(args.num("hot-colors", cfg.lossless_max_colors_hot));
  cfg.max_queue_ms = static_cast<int32_t>(args.num("max-queue-ms", 50));
  cfg.video_regions = args.num("regions", 1) != 0;
  cfg.progressive = args.num("progressive", 1) != 0;
  rdaccel::Host host(std::move(host_end), cfg);

  const auto frames = static_cast<uint64_t>(seconds * fps);
  const uint64_t t0 = rdaccel::NowMicros();
  {
    std::lock_guard<std::mutex> lock(mu);
    start_us = t0;
  }
  Image frame;
  double target_sum = 0;
  uint64_t warm_bytes = 0, warm_dropped = 0;
  int target_n = 0;
  if (!trace_path.empty()) trace.resize(frames);
  rdaccel::HostStats prev_st{};
  for (uint64_t i = 0; i < frames; ++i) {
    const uint64_t due = t0 + i * 1'000'000ull / static_cast<uint64_t>(fps);
    const uint64_t now = rdaccel::NowMicros();
    if (due > now) std::this_thread::sleep_for(std::chrono::microseconds(due - now));
    Render(scene, sc->fn, i, frame);
    const uint64_t ct = rdaccel::NowMicros();
    {
      std::lock_guard<std::mutex> lock(mu);
      index_of[ct] = i;
      if (i == warmup_frames) steady_from_us = ct;
    }
    if (with_cursor) {
      int x, y;
      bool beam;
      PointerAt(i * 2, w, h, x, y, beam);
      host.SetCursorImage(PointerImage(beam));
      host.SetCursor(x, y, true, ct);
    }
    if (!host.SubmitFrame({frame.data(), w, h, w * 4}, ct, {}, &err)) {
      std::fprintf(stderr, "frame %llu: %s\n", static_cast<unsigned long long>(i), err.c_str());
    }
    const auto st = host.stats();
    if (!trace_path.empty()) {
      std::lock_guard<std::mutex> lock(mu);
      TraceRow& r = trace[i];
      r.t_ms = (ct - t0) / 1000.0;
      r.video_b = st.video_bytes - prev_st.video_bytes;
      r.lossless_b = st.lossless_bytes - prev_st.lossless_bytes;
      r.bytes_b = st.bytes_sent - prev_st.bytes_sent;
      r.dropped = st.frames_dropped_congestion != prev_st.frames_dropped_congestion;
      r.key = st.keyframes != prev_st.keyframes;
      r.target = st.target_bitrate_bps;
      r.estimate = st.bandwidth_estimate_bps;
      r.rtt_us = st.rtt_us;
      prev_st = st;
    }
    if (args.has("verbose") && i % static_cast<uint64_t>(fps) == 0) {
      std::printf("t=%3.0fs target %5.0f kbps, estimate %5.0f kbps, rtt %3u ms, sent %7.0f kB, skipped %llu, "
                  "video frames %llu, encoder drops %llu, area pictures %llu\n",
                  static_cast<double>(i) / fps, st.target_bitrate_bps / 1000.0,
                  st.bandwidth_estimate_bps / 1000.0, st.rtt_us / 1000, st.bytes_sent / 1000.0,
                  static_cast<unsigned long long>(st.frames_dropped_congestion),
                  static_cast<unsigned long long>(st.video_frames),
                  static_cast<unsigned long long>(st.encoder_drops),
                  static_cast<unsigned long long>(st.region_pictures));
    }
    if (i == warmup_frames) {
      warm_bytes = st.bytes_sent;
      warm_dropped = st.frames_dropped_congestion;
    }
    if (i >= warmup_frames && st.target_bitrate_bps > 0) {
      target_sum += static_cast<double>(st.target_bitrate_bps);
      ++target_n;
    }
  }
  // Let the last frames arrive.
  std::this_thread::sleep_for(std::chrono::milliseconds(1500));
  const auto hs = host.stats();

  std::lock_guard<std::mutex> lock(mu);
  if (!trace_path.empty()) {
    if (FILE* f = std::fopen(trace_path.c_str(), "w")) {
      std::fprintf(f, "frame,t_ms,video_bytes,lossless_bytes,sent_bytes,skipped,keyframe,target_kbps,estimate_kbps,rtt_ms,shown_latency_ms,psnr,exact_pct\n");
      for (size_t k = 0; k < trace.size(); ++k) {
        const TraceRow& r = trace[k];
        std::fprintf(f, "%zu,%.1f,%llu,%llu,%llu,%d,%d,%.0f,%.0f,%.1f,%.1f,%.2f,%.1f\n", k, r.t_ms,
                     static_cast<unsigned long long>(r.video_b), static_cast<unsigned long long>(r.lossless_b),
                     static_cast<unsigned long long>(r.bytes_b), r.dropped ? 1 : 0, r.key ? 1 : 0,
                     r.target / 1000.0, r.estimate / 1000.0, r.rtt_us / 1000.0, r.shown_ms, r.psnr, r.exact);
      }
      std::fclose(f);
    } else {
      std::fprintf(stderr, "cannot write %s\n", trace_path.c_str());
    }
  }
  std::vector<double> lat = latency_ms;
  std::sort(lat.begin(), lat.end());
  auto pct = [&](double p) { return lat.empty() ? 0.0 : lat[static_cast<size_t>(p * (lat.size() - 1))]; };
  double max_gap = 0;
  for (size_t k = 1; k < shown_at.size(); ++k) {
    max_gap = std::max(max_gap, (shown_at[k] - shown_at[k - 1]) / 1000.0);
  }
  double psnr_avg = 0;
  for (double p : psnr) psnr_avg += p;
  if (!psnr.empty()) psnr_avg /= static_cast<double>(psnr.size());
  const double steady_s = seconds - static_cast<double>(warmup_frames) / fps;
  const uint64_t steady_frames = frames - warmup_frames;
  std::printf(
      "%-6s %4.1f Mbps %3d ms %4.1f%% loss cc %d abr %d | first picture %5.0f ms | steady: %5.0f kbps, "
      "%3.0f%% frames shown (%llu skipped), latency p50 %5.0f p95 %5.0f max %5.0f ms, stall %4.0f ms, "
      "PSNR %5.2f dB, target %5.0f kbps, estimate %5.0f kbps\n",
      sc->name, down.bandwidth_bps / 1e6, down.delay_ms, down.loss * 100, args.num("cc", 1) != 0 ? 1 : 0,
      cfg.adaptive_bitrate ? 1 : 0, first_frame_ms, (hs.bytes_sent - warm_bytes) * 8 / 1000.0 / steady_s,
      100.0 * static_cast<double>(latency_ms.size()) / static_cast<double>(steady_frames),
      static_cast<unsigned long long>(hs.frames_dropped_congestion - warm_dropped), pct(0.5), pct(0.95),
      lat.empty() ? 0.0 : lat.back(), max_gap, psnr_avg, target_n ? target_sum / target_n / 1000.0 : 0.0,
      hs.bandwidth_estimate_bps / 1000.0);
  if (args.has("verbose")) {
    // Where the latency comes from: per second of capture time, the share
    // of frames shown and the worst latency among them.
    std::printf("       by second (shown %%, worst ms):");
    for (uint64_t sec = 0; sec * fps < frames; ++sec) {
      const auto it = by_second.find(sec);
      const int n = it == by_second.end() ? 0 : it->second.first;
      std::printf(" %llu:%d/%0.0f", static_cast<unsigned long long>(sec), 100 * n / fps,
                  it == by_second.end() ? 0.0 : it->second.second);
    }
    std::printf("\n");
  }
  if (!first_psnr.empty()) {
    // How the screen looks while the session starts: PSNR of what is shown
    // at about these times after the first capture.
    std::printf("       start: ");
    for (double at : {500.0, 1000.0, 2000.0, 3000.0, 5000.0}) {
      double p = -1;
      for (const auto& [t, q] : first_psnr) {
        if (t <= at) p = q;
      }
      if (p < 0) {
        std::printf(" %4.1fs  (nothing) ", at / 1000);
      } else {
        std::printf(" %4.1fs %5.2f dB ", at / 1000, p);
      }
    }
    std::printf("\n");
  }
  if (with_cursor) {
    std::vector<double> c = cursor_ms;
    std::sort(c.begin(), c.end());
    auto cp = [&](double p) { return c.empty() ? 0.0 : c[static_cast<size_t>(p * (c.size() - 1))]; };
    std::printf("       cursor: %zu positions, latency p50 %5.0f p95 %5.0f max %5.0f ms (%llu bytes)\n", c.size(),
                cp(0.5), cp(0.95), c.empty() ? 0.0 : c.back(),
                static_cast<unsigned long long>(hs.cursor_bytes));
  }
  return 0;
}
