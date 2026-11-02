// rdaccel_bench: repeatable bitrate / quality / CPU comparisons on desktop-like
// scenes, using only the public API (rdaccel/rdaccel.h).
//
//   rdaccel_bench [--background=SCREEN.ppm] [--scenario=all|idle|typing|scroll|drag|video]
//                 [--frames=300] [--fps=60] [--bitrate=8000000] [--min-qp=24]
//                 [--lossless=1] [--max-colors=256] [--max-colors-hot=4] [--moves=1] [--refine=8] [--cache=1] [--refresh=0]
//                 [--tmp=bench.rdstream] [--dump=DIR]
//
// --dump writes the viewer's last picture and the matching source frame of
// each scenario as PPM files.
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

namespace {

using Image = std::vector<uint8_t>;  // BGRA, tightly packed

bool LoadPpm(const std::string& path, Image& out, int& w, int& h) {
  std::FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
  int maxval = 0;
  const bool ok = std::fscanf(f, "P6 %d %d %d", &w, &h, &maxval) == 3 && maxval == 255 &&
                  std::fgetc(f) != EOF && w > 0 && h > 0;
  std::vector<uint8_t> rgb(ok ? static_cast<size_t>(w) * h * 3 : 0);
  const bool read = ok && std::fread(rgb.data(), 1, rgb.size(), f) == rgb.size();
  std::fclose(f);
  if (!read) return false;
  out.resize(static_cast<size_t>(w) * h * 4);
  for (size_t i = 0, n = static_cast<size_t>(w) * h; i < n; ++i) {
    out[i * 4 + 0] = rgb[i * 3 + 2];
    out[i * 4 + 1] = rgb[i * 3 + 1];
    out[i * 4 + 2] = rgb[i * 3 + 0];
    out[i * 4 + 3] = 255;
  }
  return true;
}

struct Scene {
  const Image* bg;
  int w, h;

  uint8_t* px(Image& img, int x, int y) const { return &img[(static_cast<size_t>(y) * w + x) * 4]; }
  const uint8_t* bgpx(int x, int y) const { return &(*bg)[(static_cast<size_t>(y) * w + x) * 4]; }

  void Fill(Image& img, rdaccel::Rect r, const uint8_t* color) const {
    for (int y = r.y; y < r.bottom(); ++y) {
      for (int x = r.x; x < r.right(); ++x) std::memcpy(px(img, x, y), color, 4);
    }
  }
  // Copies background rect `src` to (dx, dy) in `img`, clipped.
  void Blit(Image& img, rdaccel::Rect src, int dx, int dy) const {
    for (int y = 0; y < src.h; ++y) {
      const int ty = dy + y;
      if (ty < 0 || ty >= h) continue;
      for (int x = 0; x < src.w; ++x) {
        const int tx = dx + x;
        if (tx < 0 || tx >= w) continue;
        std::memcpy(px(img, tx, ty), bgpx(src.x + x, src.y + y), 4);
      }
    }
  }
};

// A busy indicator in the top-right corner: what kept the "idle" real screen
// from ever being skipped in the M0 baseline.
void Spinner(const Scene& s, Image& img, uint64_t i) {
  const int cx = s.w - 40, cy = 20;
  for (int k = 0; k < 8; ++k) {
    const double a = k * 3.14159265 / 4;
    const int x = cx + static_cast<int>(std::lround(7 * std::cos(a)));
    const int y = cy + static_cast<int>(std::lround(7 * std::sin(a)));
    const uint8_t g = static_cast<uint8_t>(((k + 8 - static_cast<int>(i % 8)) % 8) * 28);
    const uint8_t c[4] = {g, g, g, 255};
    s.Fill(img, {x - 1, y - 1, 3, 3}, c);
  }
}

// Reveals the text of a block of the screenshot left to right, line by line,
// one 8-pixel step every 4 frames (~15 characters a second).
void Typing(const Scene& s, Image& img, uint64_t i) {
  const rdaccel::Rect area{s.w / 4, s.h / 6, s.w / 2, s.h / 2};
  constexpr int kLine = 20, kStep = 8;
  s.Fill(img, area, s.bgpx(area.x, area.y));
  const int per_line = area.w / kStep;
  const int steps = static_cast<int>(i / 4);
  const int lines = area.h / kLine;
  for (int l = 0; l <= std::min(steps / per_line, lines - 1); ++l) {
    const int n = l < steps / per_line ? per_line : steps % per_line;
    s.Blit(img, {area.x, area.y + l * kLine, n * kStep, kLine}, area.x, area.y + l * kLine);
  }
}

// Scrolls everything below a fixed header up by 3 pixels a frame for two
// seconds, then pauses for one (reading), and again.
void Scroll(const Scene& s, Image& img, uint64_t i) {
  const int top = s.h / 10;
  const int span = s.h - top;
  const uint64_t moving = (i / 180) * 120 + std::min<uint64_t>(i % 180, 120);
  const int off = static_cast<int>((moving * 3) % static_cast<uint64_t>(span));
  s.Blit(img, {0, top + off, s.w, span - off}, 0, top);
  s.Blit(img, {0, top, s.w, off}, 0, top + span - off);
}

// Drags a window-sized block of the screenshot around, bouncing at the edges.
void Drag(const Scene& s, Image& img, uint64_t i) {
  const rdaccel::Rect win{s.w / 3, s.h / 3, s.w / 3, s.h / 3};
  auto bounce = [](uint64_t t, int range) {
    const int p = static_cast<int>(t % static_cast<uint64_t>(2 * range));
    return p < range ? p : 2 * range - p;
  };
  const int x = bounce(i * 5, s.w - win.w);
  const int y = bounce(i * 3, s.h - win.h);
  s.Blit(img, win, x, y);
}

// A "video" window: a slowly panning, zooming, colour-shifting view of the
// screenshot, bilinearly resampled, so every pixel changes every frame.
void Video(const Scene& s, Image& img, uint64_t i) {
  const rdaccel::Rect v{s.w - s.w / 3 - 24, s.h - s.h / 3 - 24, s.w / 3, s.h / 3};
  const double zoom = 0.55 + 0.1 * std::sin(i * 0.02);
  const double ox = (s.w - v.w * zoom) * (0.5 + 0.4 * std::sin(i * 0.013));
  const double oy = (s.h - v.h * zoom) * (0.5 + 0.4 * std::cos(i * 0.017));
  const int shift = static_cast<int>(20 * std::sin(i * 0.05));
  for (int y = 0; y < v.h; ++y) {
    for (int x = 0; x < v.w; ++x) {
      const double fx = ox + x * zoom, fy = oy + y * zoom;
      const int x0 = std::clamp(static_cast<int>(fx), 0, s.w - 2);
      const int y0 = std::clamp(static_cast<int>(fy), 0, s.h - 2);
      const double ax = fx - x0, ay = fy - y0;
      uint8_t* d = s.px(img, v.x + x, v.y + y);
      for (int c = 0; c < 3; ++c) {
        const double top = s.bgpx(x0, y0)[c] * (1 - ax) + s.bgpx(x0 + 1, y0)[c] * ax;
        const double bot = s.bgpx(x0, y0 + 1)[c] * (1 - ax) + s.bgpx(x0 + 1, y0 + 1)[c] * ax;
        d[c] = static_cast<uint8_t>(std::clamp(top * (1 - ay) + bot * ay + shift * (c - 1), 0.0, 255.0));
      }
      d[3] = 255;
    }
  }
}

using SceneFn = std::function<void(const Scene&, Image&, uint64_t)>;

struct Scenario {
  const char* name;
  SceneFn fn;
};

const Scenario kScenarios[] = {
    {"idle", [](const Scene& s, Image& img, uint64_t i) { Spinner(s, img, i); }},
    {"typing", Typing},
    {"scroll", Scroll},
    {"drag", Drag},
    {"video", [](const Scene& s, Image& img, uint64_t i) {
       Spinner(s, img, i);
       Video(s, img, i);
     }},
};

void Render(const Scene& s, const SceneFn& fn, uint64_t i, Image& out) {
  out = *s.bg;
  fn(s, out, i);
}

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
  uint64_t decode_errors = 0;
  bool ok = false;
};

uint64_t FrameTime(uint64_t index, int fps) { return index * 1'000'000ull / static_cast<uint64_t>(fps); }
uint64_t FrameIndex(uint64_t time_us, int fps) {
  return (time_us * static_cast<uint64_t>(fps) + 500'000) / 1'000'000;
}

void WritePpm(const std::string& path, const uint8_t* bgra, int w, int h, int stride) {
  std::FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return;
  std::fprintf(f, "P6\n%d %d\n255\n", w, h);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const uint8_t* p = bgra + static_cast<size_t>(y) * stride + x * 4;
      const uint8_t rgb[3] = {p[2], p[1], p[0]};
      std::fwrite(rgb, 1, 3, f);
    }
  }
  std::fclose(f);
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
  double psnr_sum = 0;
  uint64_t shown = 0, exact_px = 0, total_px = 0;
  rdaccel::ViewerHandlers handlers;
  Image ref;
  double compare_cpu = 0;
  handlers.on_frame = [&](const rdaccel::ImageView& img, std::span<const rdaccel::Rect>,
                          const rdaccel::FrameInfo& info) {
    const double c = CpuSeconds();
    const uint64_t index = FrameIndex(info.capture_time_us, cfg.fps);
    Render(s, sc.fn, index, ref);
    double se = 0;
    for (int y = 0; y < img.height; ++y) {
      const uint8_t* a = img.data + static_cast<size_t>(y) * img.stride;
      const uint8_t* b = ref.data() + static_cast<size_t>(y) * s.w * 4;
      for (int x = 0; x < img.width; ++x, a += 4, b += 4) {
        int px_se = 0;
        for (int k = 0; k < 3; ++k) px_se += (a[k] - b[k]) * (a[k] - b[k]);
        se += px_se;
        exact_px += px_se == 0;
      }
    }
    total_px += static_cast<uint64_t>(img.width) * img.height;
    const double mse = se / (static_cast<double>(img.width) * img.height * 3);
    const double p = mse == 0 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
    psnr_sum += p;
    res.psnr_min = std::min(res.psnr_min, p);
    ++shown;
    if (!dump.empty() && index + 1 == frames) {
      WritePpm(dump + "/" + sc.name + "_viewer.ppm", img.data, img.width, img.height, img.stride);
      WritePpm(dump + "/" + sc.name + "_source.ppm", ref.data(), s.w, s.h, s.w * 4);
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
  res.psnr_avg = shown ? psnr_sum / shown : 0;
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

  std::printf("%dx%d, %llu frames @ %d fps, %s, lossless %s (max %d/%d colours), moves %s, refine %d, cache %s\n", w,
              h, static_cast<unsigned long long>(frames), fps,
              bg_path.empty() ? "test pattern" : bg_path.c_str(), cfg.lossless ? "on" : "off",
              cfg.lossless_max_colors, cfg.lossless_max_colors_hot, cfg.detect_moves ? "on" : "off", cfg.refine_frames,
              cfg.tile_cache ? "on" : "off");
  std::printf("%-8s %9s %9s %9s %9s %7s %7s %7s %8s %8s %7s %7s %7s %7s\n", "scenario", "kbps", "steady", "video",
              "lossless", "vframes", "moved", "cached", "enc ms", "ll ms", "hostcpu", "PSNR", "minPSNR",
              "exact%");
  bool all_ok = true;
  for (const auto& sc : kScenarios) {
    if (which != "all" && which != sc.name) continue;
    const Result r = Run(scene, sc, frames, cfg, tmp, args.str("dump"));
    const double secs = static_cast<double>(frames) / fps;
    const auto& st = r.host;
    std::printf("%-8s %9.1f %9.1f %9.1f %9.1f %7llu %7llu %7llu %8.2f %8.2f %6.0f%% %7.2f %7.2f %7.2f%s\n",
                sc.name, st.bytes_sent * 8 / 1000.0 / secs,
                // Without the first frame, and over the remaining frames' time.
                (st.bytes_sent - r.first_frame_bytes) * 8 / 1000.0 / (secs - 1.0 / fps),
                st.video_bytes * 8 / 1000.0 / secs,
                st.lossless_bytes * 8 / 1000.0 / secs,
                static_cast<unsigned long long>(st.video_frames),
                static_cast<unsigned long long>(st.moved_tiles),
                static_cast<unsigned long long>(st.cached_tiles),
                st.video_frames ? st.encode_us_total / 1000.0 / st.video_frames : 0.0,
                st.frames_encoded ? st.lossless_us_total / 1000.0 / st.frames_encoded : 0.0,
                // Host CPU per second of content, in % of one core, without
                // the time spent rendering the scene.
                100.0 * r.host_cpu / secs, r.psnr_avg, r.psnr_min, r.exact,
                r.ok ? "" : "  FAIL");
    all_ok = all_ok && r.ok;
  }
  return all_ok ? 0 : 1;
}
