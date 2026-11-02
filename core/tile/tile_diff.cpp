#include "tile/tile_diff.h"

#include <algorithm>
#include <cstring>

namespace rdaccel::tile {
namespace {

// 64-bit FNV-1a over 8-byte words followed by a final avalanche (from
// splitmix64). Good enough for change detection; will be swapped for XXH3 when
// the tile cache lands (M1), which needs a stronger hash.
constexpr uint64_t kFnvOffset = 0xcbf29ce484222325ull;
constexpr uint64_t kFnvPrime = 0x100000001b3ull;

inline uint64_t Mix(uint64_t h, uint64_t v) { return (h ^ v) * kFnvPrime; }

inline uint64_t Finalize(uint64_t z) {
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
  return z ^ (z >> 31);
}

}  // namespace

uint64_t HashRect(const ImageView& img, const Rect& r) {
  uint64_t h = Mix(kFnvOffset, (static_cast<uint64_t>(r.w) << 32) | static_cast<uint32_t>(r.h));
  const size_t row_bytes = static_cast<size_t>(r.w) * 4;
  for (int32_t y = r.y; y < r.bottom(); ++y) {
    const uint8_t* p = img.data + static_cast<size_t>(y) * img.stride + static_cast<size_t>(r.x) * 4;
    size_t i = 0;
    for (; i + 8 <= row_bytes; i += 8) {
      uint64_t v;
      std::memcpy(&v, p + i, 8);
      h = Mix(h, v);
    }
    for (; i < row_bytes; ++i) h = Mix(h, p[i]);
  }
  const uint64_t out = Finalize(h);
  return out == 0 ? 1 : out;  // 0 is reserved for "unknown"
}

void TileDiffer::Reset() { std::fill(hashes_.begin(), hashes_.end(), 0); }

std::vector<TileChange> TileDiffer::Diff(const ImageView& img, std::span<const Rect> dirty) {
  if (img.width != width_ || img.height != height_) {
    width_ = img.width;
    height_ = img.height;
    cols_ = (width_ + kTileSize - 1) / kTileSize;
    rows_ = (height_ + kTileSize - 1) / kTileSize;
    hashes_.assign(static_cast<size_t>(cols_) * rows_, 0);
  }

  // Mark the tiles touched by any dirty rect.
  std::vector<uint8_t> touched(hashes_.size(), dirty.empty() ? 1 : 0);
  for (const Rect& d : dirty) {
    const int32_t x0 = std::max(0, d.x) / kTileSize;
    const int32_t y0 = std::max(0, d.y) / kTileSize;
    const int32_t x1 = std::min(width_, d.right());
    const int32_t y1 = std::min(height_, d.bottom());
    if (x1 <= 0 || y1 <= 0 || d.empty()) continue;
    for (int32_t row = y0; row <= (y1 - 1) / kTileSize; ++row) {
      for (int32_t col = x0; col <= (x1 - 1) / kTileSize; ++col) {
        touched[static_cast<size_t>(row) * cols_ + col] = 1;
      }
    }
  }

  std::vector<TileChange> changes;
  for (int32_t row = 0; row < rows_; ++row) {
    for (int32_t col = 0; col < cols_; ++col) {
      const size_t idx = static_cast<size_t>(row) * cols_ + col;
      if (!touched[idx]) continue;
      Rect r{col * kTileSize, row * kTileSize, kTileSize, kTileSize};
      r.w = std::min(r.w, width_ - r.x);
      r.h = std::min(r.h, height_ - r.y);
      const uint64_t h = HashRect(img, r);
      if (h != hashes_[idx]) {
        changes.push_back({col, row, h, hashes_[idx], r});
        hashes_[idx] = h;
      }
    }
  }
  return changes;
}

}  // namespace rdaccel::tile
