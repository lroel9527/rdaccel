#pragma once

#include <cstdint>

namespace rdacore {

// A pixel rectangle in screen coordinates (origin top-left, physical pixels).
struct Rect {
  int32_t x = 0;
  int32_t y = 0;
  int32_t w = 0;
  int32_t h = 0;

  bool empty() const { return w <= 0 || h <= 0; }
  int32_t right() const { return x + w; }
  int32_t bottom() const { return y + h; }

  bool intersects(const Rect& o) const {
    return x < o.right() && o.x < right() && y < o.bottom() && o.y < bottom();
  }

  friend bool operator==(const Rect&, const Rect&) = default;
};

// Side length of a tile, in physical pixels. On a 2x Retina screen this is
// 32x32 points, roughly one line of text.
inline constexpr int32_t kTileSize = 64;

}  // namespace rdacore
