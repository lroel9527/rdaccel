#pragma once

// Which parts of a screen are text (or UI detail of the same kind), for
// measuring: rdaccel_clip reports how the viewer shows these parts apart
// from the rest of the screen. Not used by the host.
//
// The screen is cut into 16 x 16 cells. A cell is text when one luminance
// value dominates it without filling it: a background with strokes on it.
// Sampled at every second pixel in both directions, a cell is text when
// exactly one luminance value holds at least a third of the samples and at
// least two samples are 16 levels or more away from it. Two such values (an
// edge between two plain areas, a thick stroke), none (a photo, a texture),
// one that fills the cell (a plain area) or one with only nearby shades
// around it (a slow gradient) are not text. The rule looks at which value
// dominates, not at which is brighter, so dark and light themes and
// coloured text count alike. Text on a gradient or on a translucent panel
// has no dominating value and is missed.

#include <algorithm>
#include <cstdint>
#include <vector>

namespace rdaccel::clip {

inline constexpr int32_t kTextCell = 16;
inline constexpr int32_t kTextContrast = 16;  // luminance levels between a stroke and its background, at least

inline int32_t TextCellCols(int32_t width) { return (width + kTextCell - 1) / kTextCell; }
inline int32_t TextCellRows(int32_t height) { return (height + kTextCell - 1) / kTextCell; }

// `out` gets one byte per cell, row by row: 1 for text.
inline void LabelTextCells(const uint8_t* bgra, int32_t width, int32_t height, int32_t stride,
                           std::vector<uint8_t>& out) {
  const int32_t cols = TextCellCols(width), rows = TextCellRows(height);
  out.assign(static_cast<size_t>(cols) * rows, 0);
  uint16_t hist[256];
  for (int32_t cy = 0; cy < rows; ++cy) {
    for (int32_t cx = 0; cx < cols; ++cx) {
      for (auto& h : hist) h = 0;
      int32_t n = 0;
      const int32_t x1 = std::min(width, (cx + 1) * kTextCell), y1 = std::min(height, (cy + 1) * kTextCell);
      for (int32_t y = cy * kTextCell; y < y1; y += 2) {
        const uint8_t* p = bgra + static_cast<size_t>(y) * stride + static_cast<size_t>(cx) * kTextCell * 4;
        for (int32_t x = cx * kTextCell; x < x1; x += 2, p += 8) {
          ++hist[(77 * p[2] + 150 * p[1] + 29 * p[0]) >> 8];
          ++n;
        }
      }
      const int32_t need = (n + 2) / 3;
      int32_t dominant = 0, value = 0;
      for (int32_t v = 0; v < 256; ++v) {
        if (hist[v] >= need) {
          ++dominant;
          value = v;
        }
      }
      // Strokes stand out from their background; the neighbouring shades of
      // a slow gradient do not.
      int32_t apart = 0;
      for (int32_t v = 0; v < 256; ++v) {
        if (v <= value - kTextContrast || v >= value + kTextContrast) apart += hist[v];
      }
      out[static_cast<size_t>(cy) * cols + cx] = dominant == 1 && apart >= 2 ? 1 : 0;
    }
  }
}

}  // namespace rdaccel::clip
