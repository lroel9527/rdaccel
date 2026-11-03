// rdaccel_bench: repeatable bitrate / quality / CPU comparisons on desktop-like
// scenes, using only the public API (rdaccel/rdaccel.h).
//
//   rdaccel_bench [--background=SCREEN.ppm] [--scenario=all|idle|typing|scroll|drag|video|pause]
//                 [--frames=300] [--fps=60] [--bitrate=8000000] [--min-qp=24]
//                 [--lossless=1] [--max-colors=256] [--max-colors-hot=4] [--moves=1] [--refine=8] [--cache=1] [--refresh=0]
//                 [--regions=1] [--region-min-qp=27] [--region-bitrate=0]
//                 [--tmp=bench.rdstream] [--dump=DIR]
//
// --dump writes the viewer's final picture and the last source frame of each
// scenario as PPM files.
//
// Every scenario animates a still screenshot (a real one via --background,
// e.g. a frame dumped with rdaccel_play --dump; otherwise the test pattern),
// so the pixels are real desktop pixels and the motion is the same on every
// run. Each frame is encoded by a Host into a recording, the recording is
// decoded by a Viewer, and the composed picture is compared with the source.

#include <rdaccel/rdaccel.h>
#include <sys/resource.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "args.h"
#include "scenes.h"

namespace {

using namespace rdaccel::examples;

double CpuSeconds() {
  rusage ru{};
  getrusage(RUSAGE_SELF, &ru);
  return ru.ru_utime.tv_sec + ru.ru_stime.tv_sec + (ru.ru_utime.tv_usec + ru.ru_stime.tv_usec) / 1e6;
}

struct Result {
  rdaccel::HostStats host;
  uint64_t first_frame_bytes = 0;  // the initial full refresh
  double host_cpu = 0, viewer_cpu = 0;
  double psnr_avg = 0, psnr_min = 1e9, exact = 0;
  double psnr_in = 0, psnr_out = 0;  // inside / outside Scenario::focus
  uint64_t decode_errors = 0;
  bool ok = false;
};

uint64_t FrameTime(uint64_t index, int fps) { return index * 1'000'000ull / static_cast<uint64_t>(fps); }
uint64_t FrameIndex(uint64_t time_us, int fps) {
  return (time_us * static_cast<uint64_t>(fps) + 500'000) / 1'000'000;
}

Result Run(const Scene& s, const Scenario& sc, uint64_t frames, const rdaccel::HostConfig& cfg,
           const std::string& tmp, const std::string& dump) {
  Result res;
  std::string err;
  Image frame;
  {
    auto file = rdaccel::CreateRecordingTransport(tmp, &err);
    if (!file) {
      std::fprintf(stderr, "%s\n", err.c_str());
      return res;
    }
    rdaccel::Host host(std::move(file), cfg);
    const double c0 = CpuSeconds();
    double render_cpu = 0;
    for (uint64_t i = 0; i < frames; ++i) {
      const double r0 = CpuSeconds();
      Render(s, sc.fn, i, frame);
      render_cpu += CpuSeconds() - r0;
      // Content time, not wall time: the encoder's rate control needs real
      // frame spacing, and the viewer side recovers the frame index from it.
      if (!host.SubmitFrame({frame.data(), s.w, s.h, s.w * 4}, FrameTime(i, cfg.fps), {}, &err)) {
        std::fprintf(stderr, "%s: %s\n", sc.name, err.c_str());
        return res;
      }
      if (i == 0) res.first_frame_bytes = host.stats().bytes_sent;
    }
    res.host_cpu = CpuSeconds() - c0 - render_cpu;
    res.host = host.stats();
  }

  std::vector<uint8_t> bytes;
  if (!rdaccel::ReadRecording(tmp, bytes, &err)) {
    std::fprintf(stderr, "%s\n", err.c_str());
    return res;
  }
  Image last;  // the last picture shown, for --dump
  double psnr_sum = 0, psnr_in_sum = 0, psnr_out_sum = 0;
  uint64_t shown = 0, exact_px = 0, total_px = 0;
  rdaccel::ViewerHandlers handlers;
  Image ref;
  double compare_cpu = 0;
  handlers.on_frame = [&](const rdaccel::ImageView& img, std::span<const rdaccel::Rect>,
                          const rdaccel::FrameInfo& info) {
    const double c = CpuSeconds();
    const uint64_t index = FrameIndex(info.capture_time_us, cfg.fps);
    Render(s, sc.fn, index, ref);
    double se = 0, se_in = 0;
    uint64_t n_in = 0;
    const rdaccel::Rect focus = sc.focus ? sc.focus(s) : rdaccel::Rect{};
    for (int y = 0; y < img.height; ++y) {
      const uint8_t* a = img.data + static_cast<size_t>(y) * img.stride;
      const uint8_t* b = ref.data() + static_cast<size_t>(y) * s.w * 4;
      for (int x = 0; x < img.width; ++x, a += 4, b += 4) {
        int px_se = 0;
        for (int k = 0; k < 3; ++k) px_se += (a[k] - b[k]) * (a[k] - b[k]);
        se += px_se;
        exact_px += px_se == 0;
        if (x >= focus.x && x < focus.right() && y >= focus.y && y < focus.bottom()) {
          se_in += px_se;
          ++n_in;
        }
      }
    }
    auto to_psnr = [](double sq, double n) {
      return sq == 0 || n == 0 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / (sq / (n * 3)));
    };
    if (n_in > 0) {
      psnr_in_sum += to_psnr(se_in, static_cast<double>(n_in));
      psnr_out_sum += to_psnr(se - se_in, static_cast<double>(img.width) * img.height - n_in);
    }
    total_px += static_cast<uint64_t>(img.width) * img.height;
    const double mse = se / (static_cast<double>(img.width) * img.height * 3);
    const double p = mse == 0 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
    psnr_sum += p;
    res.psnr_min = std::min(res.psnr_min, p);
    ++shown;
    if (!dump.empty()) {
      last.resize(static_cast<size_t>(img.width) * img.height * 4);
      for (int y = 0; y < img.height; ++y) {
        std::memcpy(last.data() + static_cast<size_t>(y) * img.width * 4,
                    img.data + static_cast<size_t>(y) * img.stride, static_cast<size_t>(img.width) * 4);
      }
    }
    compare_cpu += CpuSeconds() - c;
  };
  auto viewer = rdaccel::Viewer::Create(nullptr, std::move(handlers), &err);
  if (!viewer) {
    std::fprintf(stderr, "%s\n", err.c_str());
    return res;
  }
  const double c0 = CpuSeconds();
  viewer->FeedRecording(bytes);
  res.viewer_cpu = CpuSeconds() - c0 - compare_cpu;
  res.decode_errors = viewer->stats().decode_errors;
  if (!dump.empty() && !last.empty()) {
    // What the viewer shows at the end against the last source frame (later
    // frames may have been skipped as unchanged).
    Render(s, sc.fn, frames - 1, ref);
    WritePpm(dump + "/" + sc.name + "_viewer.ppm", last.data(), s.w, s.h, s.w * 4);
    WritePpm(dump + "/" + sc.name + "_source.ppm", ref.data(), s.w, s.h, s.w * 4);
  }
  res.psnr_avg = shown ? psnr_sum / shown : 0;
  res.psnr_in = shown ? psnr_in_sum / shown : 0;
  res.psnr_out = shown ? psnr_out_sum / shown : 0;
  res.exact = total_px ? 100.0 * exact_px / total_px : 0;
  res.ok = shown > 0 && res.decode_errors == 0;
  std::remove(tmp.c_str());
  return res;
}

}  // namespace

int main(int argc, char** argv) {
  rdaccel::examples::Args args(argc, argv);
  const auto frames = static_cast<uint64_t>(args.num("frames", 300));
  const int fps = static_cast<int>(args.num("fps", 60));
  const std::string which = args.str("scenario", "all");
  const std::string tmp = args.str("tmp", "rdaccel_bench.rdstream");

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

  rdaccel::HostConfig cfg;
  cfg.fps = fps;
  cfg.bitrate_bps = args.num("bitrate", 8'000'000);
  cfg.min_qp = static_cast<int32_t>(args.num("min-qp", 24));
  cfg.lossless = args.num("lossless", 1) != 0;
  cfg.lossless_max_colors = static_cast<int32_t>(args.num("max-colors", 256));
  cfg.lossless_max_colors_hot = static_cast<int32_t>(args.num("max-colors-hot", 4));
  cfg.detect_moves = args.num("moves", 1) != 0;
  cfg.refine_frames = static_cast<int32_t>(args.num("refine", 8));
  cfg.tile_cache = args.num("cache", 1) != 0;
  cfg.refresh = args.num("refresh", 0) != 0;
  cfg.video_regions = args.num("regions", 1) != 0;
  cfg.video_region_min_qp = static_cast<int32_t>(args.num("region-min-qp", 27));
  cfg.video_region_bitrate_bps = args.num("region-bitrate", 0);

  std::printf("%dx%d, %llu frames @ %d fps, %s, lossless %s (max %d/%d colours), moves %s, refine %d, cache %s\n", w,
              h, static_cast<unsigned long long>(frames), fps,
              bg_path.empty() ? "test pattern" : bg_path.c_str(), cfg.lossless ? "on" : "off",
              cfg.lossless_max_colors, cfg.lossless_max_colors_hot, cfg.detect_moves ? "on" : "off", cfg.refine_frames,
              cfg.tile_cache ? "on" : "off");
  std::printf("%-8s %9s %9s %9s %9s %7s %7s %7s %8s %8s %9s %7s %7s %7s %7s\n", "scenario", "kbps", "steady", "video",
              "lossless", "vframes", "moved", "cached", "enc ms", "ll ms", "regions", "hostcpu", "PSNR", "minPSNR",
              "exact%");
  bool all_ok = true;
  for (const auto& sc : kScenarios) {
    if (which != "all" && which != sc.name) continue;
    const Result r = Run(scene, sc, frames, cfg, tmp, args.str("dump"));
    const double secs = static_cast<double>(frames) / fps;
    const auto& st = r.host;
    std::printf("%-8s %9.1f %9.1f %9.1f %9.1f %7llu %7llu %7llu %8.2f %8.2f %4llu/%-4llu %6.0f%% %7.2f %7.2f %7.2f%s\n",
                sc.name, st.bytes_sent * 8 / 1000.0 / secs,
                // Without the first frame, and over the remaining frames' time.
                (st.bytes_sent - r.first_frame_bytes) * 8 / 1000.0 / (secs - 1.0 / fps),
                st.video_bytes * 8 / 1000.0 / secs,
                st.lossless_bytes * 8 / 1000.0 / secs,
                static_cast<unsigned long long>(st.video_frames + st.region_pictures),
                static_cast<unsigned long long>(st.moved_tiles),
                static_cast<unsigned long long>(st.cached_tiles),
                // Encoder time per frame that needed any video encode.
                (st.video_frames + st.region_pictures)
                    ? (st.encode_us_total + st.region_encode_us_total) / 1000.0 /
                          std::max(st.video_frames, st.region_pictures)
                    : 0.0,
                st.frames_encoded ? st.lossless_us_total / 1000.0 / st.frames_encoded : 0.0,
                static_cast<unsigned long long>(st.regions_opened),
                static_cast<unsigned long long>(st.regions_closed),
                // Host CPU per second of content, in % of one core, without
                // the time spent rendering the scene.
                100.0 * r.host_cpu / secs, r.psnr_avg, r.psnr_min, r.exact,
                r.ok ? "" : "  FAIL");
    if (sc.focus) {
      std::printf("%-8s PSNR inside the moving area %.2f dB, elsewhere %.2f dB\n", "", r.psnr_in,
                  r.psnr_out);
    }
    all_ok = all_ok && r.ok;
  }
  return all_ok ? 0 : 1;
}
