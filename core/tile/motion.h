#pragma once

// Scroll and window-move detection.
//
// When many tiles change at once, it is often because content moved: a page
// scrolled, a window was dragged. Re-sending moved pixels costs bits (and
// quality, through the video encoder); telling the viewer to copy what it
// already shows costs a few bytes and is exact.
//
// Detection works on the changed tiles only:
//   1. Each changed tile contributes a probe: its most detailed full-width
//      pixel row, hashed.
//   2. One pass over the previous frame with a rolling hash finds where each
//      probe row was before. Each hit votes for a motion vector.
//   3. For the best-voted vectors, every changed tile is compared with the
//      previous frame shifted by that vector; exact matches become moves.
//   4. Tiles on the edge of the moving area (where new content scrolls in, or
//      a static header meets the scrolling page) are split by pixel rows: rows
//      that moved become moves, unchanged rows need nothing, and only the
//      remaining rows are left to code.

#include <cstdint>
#include <functional>
#include <span>
#include <vector>

#include "tile/tile_diff.h"

namespace rdacore::tile {

struct Move {
  Rect dst;  // area of the current frame
  int32_t dx = 0;  // dst = src shifted by (dx, dy)
  int32_t dy = 0;
  Rect src() const { return {dst.x - dx, dst.y - dy, dst.w, dst.h}; }
  friend bool operator==(const Move&, const Move&) = default;
};

struct MotionOptions {
  size_t min_changed_tiles = 6;  // below this, plain coding is cheap anyway
  int max_vectors = 3;
  // Unchanged tiles inside the moving area (a blank page, a flat window
  // body) are made part of the moves, so that the pieces around them join
  // into a few large ones. Their pixels are the same before and after, but
  // only where the copy's source is what it is here: a receiver that shows
  // the source less exactly than the tile it lands on (video quality next
  // to a lossless tile) would copy that over a tile which was fine, and
  // nothing would ever send it again, as it did not change. With this set,
  // such a tile is only added when the function returns true for the rect
  // it would be copied from. Unset: always (the pixels are exact copies).
  std::function<bool(const Rect& src)> may_copy_unchanged;
};

// Finds moves that explain some of `changes`. `moved[i]` is set for every
// change the moves cover fully or partly; for a partly covered change,
// `residual[i]` lists the parts of its rect that still need coding (for the
// others it is empty). `prev` and `cur` must have the same size.
std::vector<Move> DetectMoves(const ImageView& prev, const ImageView& cur,
                              std::span<const TileChange> changes, std::vector<bool>& moved,
                              std::vector<std::vector<Rect>>& residual,
                              const MotionOptions& opts = {});

// Applies moves to a BGRA buffer the way the viewer does: every source is
// read from the buffer as it was before any of the moves.
void ApplyMoves(std::span<const Move> moves, uint8_t* bgra, int32_t width, int32_t height,
                std::vector<uint8_t>& scratch);

}  // namespace rdacore::tile
