#pragma once

// CPU reference implementation of tile change detection.
//
// The frame is cut into kTileSize x kTileSize tiles. For every tile touched by
// a dirty rect we hash its pixels and compare with the previous frame's hash.
// Platforms may replace this with a GPU version (Metal compute on macOS); the
// CPU version stays as the correctness reference for tests.

#include <cstdint>
#include <span>
#include <vector>

#include "rdaccel/geometry.h"

namespace rdaccel::tile {

// A read-only view of a 32-bit-per-pixel image (BGRA on all platforms).
struct ImageView {
  const uint8_t* data = nullptr;
  int32_t width = 0;
  int32_t height = 0;
  int32_t stride = 0;  // bytes per row
};

struct TileChange {
  int32_t col = 0;
  int32_t row = 0;
  uint64_t hash = 0;
  uint64_t prev_hash = 0;
  Rect rect;  // clipped to the frame
};

// 64-bit hash of a rectangle of pixels. Stable across platforms and runs, so
// both ends of a connection can use it as a cache key.
uint64_t HashRect(const ImageView& img, const Rect& r);

class TileDiffer {
 public:
  TileDiffer() = default;

  // Returns the tiles whose content changed since the previous call. Only
  // tiles that intersect `dirty` are examined; pass an empty span to examine
  // the whole frame. A change of frame size resets all state, so every tile
  // is reported as changed.
  std::vector<TileChange> Diff(const ImageView& img, std::span<const Rect> dirty);

  int32_t cols() const { return cols_; }
  int32_t rows() const { return rows_; }

  // Forget all hashes; the next Diff() reports every examined tile.
  void Reset();

 private:
  int32_t width_ = 0;
  int32_t height_ = 0;
  int32_t cols_ = 0;
  int32_t rows_ = 0;
  std::vector<uint64_t> hashes_;  // row-major, 0 = unknown
};

}  // namespace rdaccel::tile
