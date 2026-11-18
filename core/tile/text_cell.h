#pragma once

// Does a small rect of the screen look like text (or UI detail of the same
// kind: thin lines, glyph-like marks)? One luminance value dominates it
// without filling it, and something stands out from that value: a
// background with strokes on it. A photo, a texture or a gradient has no
// dominating value; a plain area has nothing standing out; an edge between
// two plain areas has two dominating values.
//
// Every pixel is looked at (a one-pixel line counts). Meant for rects of
// about 16 x 16 pixels.

#include <cstdint>

#include "tile/tile_diff.h"

namespace rdacore::tile {

inline constexpr int32_t kTextContrast = 16;  // luminance levels between a stroke and its background, at least

inline bool LooksLikeText(const ImageView& img, const Rect& r) {
  uint32_t hist[256] = {};
  for (int32_t y = r.y; y < r.bottom(); ++y) {
    const uint8_t* p = img.data + static_cast<size_t>(y) * img.stride + static_cast<size_t>(r.x) * 4;
    for (int32_t x = 0; x < r.w; ++x, p += 4) ++hist[(77 * p[2] + 150 * p[1] + 29 * p[0]) >> 8];
  }
  const int32_t n = r.w * r.h, need = (n + 2) / 3;
  int32_t dominant = 0, value = 0;
  for (int32_t v = 0; v < 256; ++v) {
    if (static_cast<int32_t>(hist[v]) >= need) {
      ++dominant;
      value = v;
    }
  }
  if (dominant != 1) return false;
  int32_t apart = 0;
  for (int32_t v = 0; v < 256; ++v) {
    if (v <= value - kTextContrast || v >= value + kTextContrast) apart += static_cast<int32_t>(hist[v]);
  }
  return apart >= 2;
}

}  // namespace rdacore::tile
