#include "capture/synthetic_capturer.h"

#include <algorithm>
#include <chrono>

namespace rdacore::capture {
namespace {

constexpr int32_t kGlyphW = 14;   // Retina-ish 7pt glyph cell
constexpr int32_t kGlyphH = 28;
constexpr int32_t kMargin = 48;

// The frame being drawn. Everything drawn is clipped to it, so any size
// works (small ones just show less of the page).
struct Canvas {
  uint8_t* px;
  int32_t w, h;
};

Rect Clip(const Rect& r, int32_t w, int32_t h) {
  const int32_t x0 = std::clamp(r.x, 0, w), y0 = std::clamp(r.y, 0, h);
  const int32_t x1 = std::clamp(r.right(), 0, w), y1 = std::clamp(r.bottom(), 0, h);
  return {x0, y0, x1 - x0, y1 - y0};
}

void Fill(const Canvas& c, const Rect& area, uint8_t b, uint8_t g, uint8_t rr) {
  const Rect r = Clip(area, c.w, c.h);
  for (int32_t y = r.y; y < r.bottom(); ++y) {
    uint8_t* p = c.px + (static_cast<size_t>(y) * c.w + r.x) * 4;
    for (int32_t x = 0; x < r.w; ++x, p += 4) {
      p[0] = b;
      p[1] = g;
      p[2] = rr;
      p[3] = 255;
    }
  }
}

uint32_t Hash32(uint64_t x) {
  x ^= x >> 33;
  x *= 0xff51afd7ed558ccdull;
  x ^= x >> 33;
  return static_cast<uint32_t>(x);
}

// Draws a pseudo-glyph: a few strokes chosen from the character index, in
// near-black with one anti-aliasing grey level, like rendered text.
void DrawGlyph(const Canvas& px, int32_t x0, int32_t y0, uint64_t ch) {
  const uint32_t bits = Hash32(ch);
  if ((bits & 0x7) == 0) return;  // a space
  const int32_t sx = 3, sy = 4;
  for (int32_t i = 0; i < 9; ++i) {
    if (!(bits & (1u << (i + 3)))) continue;
    const int32_t cx = x0 + 2 + (i % 3) * sx;
    const int32_t cy = y0 + 6 + (i / 3) * (sy + 2);
    Fill(px, {cx, cy, sx, sy + 2}, 30, 30, 30);
    Fill(px, {cx + sx, cy, 1, sy + 2}, 140, 140, 140);
  }
}

int32_t CharsPerLine(int32_t w) { return std::max(1, (w - 2 * kMargin) / kGlyphW); }

Rect VideoRect(int32_t w, int32_t h) {
  const int32_t vw = std::min(w / 3, 640), vh = std::min(h / 3, 360);
  return {w - vw - kMargin, h - vh - kMargin, vw, vh};
}

void DrawVideo(const Canvas& c, const Rect& v, uint64_t index) {
  // A moving gradient with a bouncing bar: changes every frame, many colours.
  const Rect vis = Clip(v, c.w, c.h);
  for (int32_t y = vis.y - v.y; y < vis.bottom() - v.y; ++y) {
    uint8_t* p = c.px + (static_cast<size_t>(v.y + y) * c.w + vis.x) * 4;
    for (int32_t x = vis.x - v.x; x < vis.right() - v.x; ++x, p += 4) {
      p[0] = static_cast<uint8_t>(x + index * 3);
      p[1] = static_cast<uint8_t>(y * 2 + index);
      p[2] = static_cast<uint8_t>((x + y) / 2 + index * 5);
      p[3] = 255;
    }
  }
  const int32_t bar_x = static_cast<int32_t>((index * 9) % static_cast<uint64_t>(std::max(1, v.w - 40)));
  const Rect bar{v.x + bar_x, v.y, std::min(40, v.w - bar_x), v.h};  // within the video
  Fill(c, bar, 250, 250, 250);
}

}  // namespace

std::vector<Rect> RenderSyntheticFrame(uint64_t index, int32_t w, int32_t h,
                                       std::vector<uint8_t>& bgra) {
  const size_t size = static_cast<size_t>(w) * h * 4;
  const bool full = bgra.size() != size;
  if (full) bgra.assign(size, 0);
  return RenderSyntheticFrame(index, w, h, bgra.data(), full);
}

std::vector<Rect> RenderSyntheticFrame(uint64_t index, int32_t w, int32_t h, uint8_t* bgra,
                                       bool full) {
  full = full || index == 0;
  const int32_t per_line = CharsPerLine(w);
  const int32_t max_lines = std::max(1, (h - 2 * kMargin) / kGlyphH);
  const Rect video = VideoRect(w, h);
  const Canvas canvas{bgra, w, h};
  std::vector<Rect> dirty;
  auto mark = [&](const Rect& r) {
    const Rect c = Clip(r, w, h);
    if (c.w > 0 && c.h > 0) dirty.push_back(c);
  };

  if (full) {
    Fill(canvas, {0, 0, w, h}, 245, 242, 240);
    // Re-type everything up to this frame so random access is deterministic.
    for (uint64_t c = 0; c < index; ++c) {
      const auto line = static_cast<int32_t>((c / per_line) % max_lines);
      const auto col = static_cast<int32_t>(c % per_line);
      // As the incremental path does: a new line clears it first.
      if (col == 0) Fill(canvas, {kMargin, kMargin + line * kGlyphH, per_line * kGlyphW, kGlyphH}, 245, 242, 240);
      DrawGlyph(canvas, kMargin + col * kGlyphW, kMargin + line * kGlyphH, c);
    }
    mark({0, 0, w, h});
  } else {
    const uint64_t c = index - 1;
    const auto line = static_cast<int32_t>((c / per_line) % max_lines);
    const auto col = static_cast<int32_t>(c % per_line);
    if (col == 0) {  // starting a new line clears it (wraps around the page)
      const Rect row{kMargin, kMargin + line * kGlyphH, per_line * kGlyphW, kGlyphH};
      Fill(canvas, row, 245, 242, 240);
      mark(row);
    }
    const Rect cell{kMargin + col * kGlyphW, kMargin + line * kGlyphH, kGlyphW, kGlyphH};
    DrawGlyph(canvas, cell.x, cell.y, c);
    mark(cell);
    mark(video);
  }
  DrawVideo(canvas, video, index);
  return dirty;
}

uint64_t MonotonicMicros() {
  using namespace std::chrono;
  return static_cast<uint64_t>(
      duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
}

bool SyntheticCapturer::Start(const CaptureConfig& cfg, FrameFn on_frame) {
  if (running_.exchange(true)) {
    error_ = "already running";
    return false;
  }
  const int32_t w = cfg.width > 0 ? cfg.width : 1920;
  const int32_t h = cfg.height > 0 ? cfg.height : 1080;
  const int32_t fps = std::max(1, cfg.fps);
  thread_ = std::thread([this, w, h, fps, on_frame = std::move(on_frame)] {
    std::vector<uint8_t> px;
    const auto period = std::chrono::microseconds(1'000'000 / fps);
    auto next = std::chrono::steady_clock::now();
    for (uint64_t i = 0; running_ && (max_frames_ == 0 || i < max_frames_); ++i) {
      CapturedFrame f;
      f.dirty = RenderSyntheticFrame(i, w, h, px);
      f.bgra = {px.data(), w, h, w * 4};
      f.capture_time_us = MonotonicMicros();
      on_frame(f);
      next += period;
      std::this_thread::sleep_until(next);
    }
    running_ = false;
  });
  return true;
}

void SyntheticCapturer::Join() {
  if (thread_.joinable()) thread_.join();
}

void SyntheticCapturer::Stop() {
  running_ = false;
  Join();
}

}  // namespace rdacore::capture
