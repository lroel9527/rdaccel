#pragma once

// A deterministic synthetic desktop for testing an integration without a
// real screen: a window background, text typed one glyph per frame, and a
// small animated "video" area.

#include <cstdint>
#include <vector>

#include "rdacore/geometry.h"

namespace rdacore {

// Renders frame `index` into `bgra` (resized to w * h * 4, stride w * 4).
// Returns the areas that differ from frame index - 1. If `bgra` is empty or
// has the wrong size, the frame is rendered from scratch.
std::vector<Rect> RenderTestPattern(uint64_t index, int32_t w, int32_t h,
                                    std::vector<uint8_t>& bgra);

}  // namespace rdacore
