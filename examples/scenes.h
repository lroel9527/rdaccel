#pragma once

// Desktop-like test scenes shared by rdaccel_bench and rdaccel_netsim. Each
// animates a still screenshot (a real one, or the built-in test pattern), so
// the pixels are real desktop pixels and the motion is the same on every run.

#include <rdaccel/rdaccel.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace rdaccel::examples {


using Image = std::vector<uint8_t>;  // BGRA, tightly packed

inline bool LoadPpm(const std::string& path, Image& out, int& w, int& h) {
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
inline void Spinner(const Scene& s, Image& img, uint64_t i) {
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
inline void Typing(const Scene& s, Image& img, uint64_t i) {
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
inline void Scroll(const Scene& s, Image& img, uint64_t i) {
  const int top = s.h / 10;
  const int span = s.h - top;
  const uint64_t moving = (i / 180) * 120 + std::min<uint64_t>(i % 180, 120);
  const int off = static_cast<int>((moving * 3) % static_cast<uint64_t>(span));
  s.Blit(img, {0, top + off, s.w, span - off}, 0, top);
  s.Blit(img, {0, top, s.w, off}, 0, top + span - off);
}

// Drags a window-sized block of the screenshot around, bouncing at the edges.
inline void Drag(const Scene& s, Image& img, uint64_t i) {
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
inline rdaccel::Rect VideoRect(const Scene& s) {
  return {s.w - s.w / 3 - 24, s.h - s.h / 3 - 24, s.w / 3, s.h / 3};
}

inline void Video(const Scene& s, Image& img, uint64_t i) {
  const rdaccel::Rect v = VideoRect(s);
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
  // Optional: an area whose quality is reported separately ("in" vs "out").
  rdaccel::Rect (*focus)(const Scene&) = nullptr;
};

const Scenario kScenarios[] = {
    {"idle", [](const Scene& s, Image& img, uint64_t i) { Spinner(s, img, i); }},
    {"typing", Typing},
    {"scroll", Scroll},
    {"drag", Drag},
    {"video", [](const Scene& s, Image& img, uint64_t i) {
       Spinner(s, img, i);
       Video(s, img, i);
     }, VideoRect},
    // Plays for 3 seconds, pauses for 2: the paused picture should end up
    // sharp, not stay at motion quality.
    {"pause", [](const Scene& s, Image& img, uint64_t i) {
       Video(s, img, i % 300 < 180 ? i : i - i % 300 + 179);
     }, VideoRect},
    // Nothing on the screen changes but the mouse pointer (use with
    // --cursor=frame or --cursor=channel).
    {"pointer", [](const Scene&, Image&, uint64_t) {}},
};

// A mouse pointer for the cursor comparisons: an arrow, and a text beam
// while it is over the left third of the screen (the "document").
inline rdaccel::CursorImage PointerImage(bool beam) {
  rdaccel::CursorImage c;
  c.width = 24;
  c.height = 32;
  c.bgra.assign(static_cast<size_t>(c.width) * c.height * 4, 0);
  auto put = [&](int x, int y, uint8_t v) {
    uint8_t* p = &c.bgra[(static_cast<size_t>(y) * c.width + x) * 4];
    p[0] = p[1] = p[2] = v;
    p[3] = 255;
  };
  if (beam) {
    c.hotspot_x = 4;
    c.hotspot_y = 10;
    for (int y = 0; y < 20; ++y) put(4, y, 0);
    for (int x = 1; x < 8; ++x) {
      if (x != 4) {
        put(x, 0, 0);
        put(x, 19, 0);
      }
    }
  } else {
    // Black outline, white inside, 1-pixel-per-row wider triangle.
    for (int y = 0; y < 26; ++y) {
      const int wdt = std::min(y, 16) + 1;
      for (int x = 0; x < wdt && x < c.width; ++x) put(x, y, (x == 0 || x == wdt - 1 || y == 25) ? 0 : 255);
    }
  }
  return c;
}

// Where the pointer is at frame `i`: sweeps the whole screen at the speed
// of a hand moving a mouse (a few hundred pixels per second, faster across).
inline void PointerAt(uint64_t i, int w, int h, int& x, int& y, bool& beam) {
  const double t = static_cast<double>(i);
  x = static_cast<int>(w * (0.5 + 0.42 * std::sin(t * 0.021)));
  y = static_cast<int>(h * (0.5 + 0.40 * std::sin(t * 0.033 + 1.0)));
  beam = x < w / 3;
}

inline void Render(const Scene& s, const SceneFn& fn, uint64_t i, Image& out) {
  out = *s.bg;
  fn(s, out, i);
}

// The scene with the pointer drawn in, as the user sees it.
inline void DrawPointer(const Scene& s, uint64_t i, Image& img) {
  static const rdaccel::CursorImage kArrow = PointerImage(false), kBeam = PointerImage(true);
  int x, y;
  bool beam;
  PointerAt(i, s.w, s.h, x, y, beam);
  rdaccel::DrawCursor(beam ? kBeam : kArrow, x, y, img.data(), s.w, s.h, s.w * 4);
}

inline void WritePpm(const std::string& path, const uint8_t* bgra, int w, int h, int stride) {
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

}  // namespace rdaccel::examples
