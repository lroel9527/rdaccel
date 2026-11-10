#pragma once

// Dirty rectangles from a capture API whose unit is not what the picture is
// in. ScreenCaptureKit documents neither: on a 2x display its content rect
// is in points while its dirty rects are in pixels of the picture (measured
// on macOS 27), and nothing promises that every version agrees. Tiles the
// dirty rects miss are never compared, so a wrong guess leaves parts of the
// screen stale; a rect too many only costs a comparison. So when points and
// pixels differ, a rect is taken both ways.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "rdacore/geometry.h"

namespace rdacore::capture {

// Adds to `out` the part of the w x h picture that the rect (x, y, rw, rh)
// may mean: as it stands (pixels of the picture), and scaled by `scale`
// (points of a content area `scale` times smaller, starting at ox, oy in
// its own unit). Empty results are left out.
inline void AppendDirtyRect(double x, double y, double rw, double rh, double scale, double ox, double oy, int32_t w,
                            int32_t h, std::vector<Rect>& out) {
  auto add = [&](double x0, double y0, double x1, double y1) {
    const double l = std::clamp(std::floor(x0), 0.0, static_cast<double>(w));
    const double t = std::clamp(std::floor(y0), 0.0, static_cast<double>(h));
    const double r = std::clamp(std::ceil(x1), 0.0, static_cast<double>(w));
    const double b = std::clamp(std::ceil(y1), 0.0, static_cast<double>(h));
    if (r <= l || b <= t) return;
    const Rect rect{static_cast<int32_t>(l), static_cast<int32_t>(t), static_cast<int32_t>(r - l),
                    static_cast<int32_t>(b - t)};
    if (!out.empty() && out.back().x == rect.x && out.back().y == rect.y && out.back().w == rect.w &&
        out.back().h == rect.h) {
      return;  // both readings are the same rect (scale 1)
    }
    out.push_back(rect);
  };
  if (!(rw > 0) || !(rh > 0) || !std::isfinite(x + y + rw + rh)) return;
  add(x, y, x + rw, y + rh);
  if (std::isfinite(scale) && scale > 0) {
    add((x - ox) * scale, (y - oy) * scale, (x + rw - ox) * scale, (y + rh - oy) * scale);
  }
}

}  // namespace rdacore::capture
