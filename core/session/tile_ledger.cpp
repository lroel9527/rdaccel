#include "session/tile_ledger.h"

#include <algorithm>
#include <cstring>

namespace rdacore::session {

void TileLedger::Resize(int32_t width, int32_t height, int32_t cols, int32_t rows) {
  width_ = width;
  height_ = height;
  cols_ = cols;
  tiles_.assign(static_cast<size_t>(cols) * rows, TileState{});
  owed_count_ = 0;
  refining_ = 0;
  cursor = 0;
  px_.assign(static_cast<size_t>(width) * height, kPxVideo);
  cell_cols_ = (width + kCellSize - 1) / kCellSize;
  retakes_.assign(static_cast<size_t>(cell_cols_) * ((height + kCellSize - 1) / kCellSize), 0);
  measured_.assign(retakes_.size(), 0);
  poor_possible_ = false;
}

void TileLedger::StartKeyframe() {
  for (TileState& t : tiles_) {
    t.owed = false;
    t.unflattened = false;
    t.stale = 0;
    t.coarse = false;
  }
  owed_count_ = 0;
  cursor = 0;
}

void TileLedger::ForgetAll() {
  for (TileState& t : tiles_) {
    t.refine = 0;
    t.quality = kRough;
  }
  refining_ = 0;
  std::fill(px_.begin(), px_.end(), kPxVideo);
  poor_possible_ = false;
}

void TileLedger::Owe(size_t idx) {
  if (tiles_[idx].owed) return;
  tiles_[idx].owed = true;
  ++owed_count_;
}

void TileLedger::Settle(size_t idx) {
  if (!tiles_[idx].owed) return;
  tiles_[idx].owed = false;
  --owed_count_;
}

void TileLedger::SetRefine(size_t idx, uint8_t pictures) {
  TileState& t = tiles_[idx];
  refining_ += (pictures > 0) - (t.refine > 0);
  t.refine = pictures;
  if (pictures > 0) t.polish = kUnpolished;
}

bool TileLedger::Refined(size_t idx) {
  TileState& t = tiles_[idx];
  if (t.refine == 0 || --t.refine > 0) return false;
  --refining_;
  return true;
}

void TileLedger::EnterArea(size_t idx, uint64_t frame_no, uint64_t now_us) {
  TileState& t = tiles_[idx];
  t.quality = kRough;
  t.born = frame_no;
  t.born_us = now_us;
  t.stale = 0;  // the area's picture brings it up to date
}

void TileLedger::NewContent(size_t idx) {
  const auto col = static_cast<int32_t>(idx % static_cast<size_t>(cols_));
  const auto row = static_cast<int32_t>(idx / static_cast<size_t>(cols_));
  const int32_t x1 = std::min(width_, (col + 1) * kTileSize), y1 = std::min(height_, (row + 1) * kTileSize);
  for (int32_t y = row * kTileSize; y < y1; y += kCellSize) {
    for (int32_t x = col * kTileSize; x < x1; x += kCellSize) retakes(x, y) = measured(x, y) = 0;
  }
}

void TileLedger::GiveUpPoor() {
  if (!poor_possible_) return;
  std::replace(px_.begin(), px_.end(), kPxPoor, kPxPolished);
  poor_possible_ = false;
}

TileLedger::Mix TileLedger::MixOf(const Rect& r) const {
  Mix m;
  for (int32_t y = r.y; y < r.bottom(); ++y) {
    const uint8_t* p = px_.data() + static_cast<size_t>(y) * width_ + r.x;
    for (int32_t x = 0; x < r.w; ++x) {
      m.exact |= p[x] == kPxExact;
      m.poor |= p[x] == kPxPoor;
    }
  }
  return m;
}

bool TileLedger::HasPoor(const Rect& r) const {
  for (int32_t y = r.y; y < r.bottom(); ++y) {
    if (std::memchr(px_.data() + static_cast<size_t>(y) * width_ + r.x, kPxPoor, static_cast<size_t>(r.w))) return true;
  }
  return false;
}

void TileLedger::Replace(const Rect& r, uint8_t from, uint8_t to) {
  for (int32_t y = r.y; y < r.bottom(); ++y) {
    uint8_t* p = px_.data() + static_cast<size_t>(y) * width_ + r.x;
    for (int32_t x = 0; x < r.w; ++x) p[x] = p[x] == from ? to : p[x];
  }
}

size_t TileLedger::coarse_count() const {
  size_t n = 0;
  for (const TileState& t : tiles_) n += t.coarse;
  return n;
}

void TileLedger::SetPixels(const Rect& r, uint8_t q) {
  for (int32_t y = r.y; y < r.bottom(); ++y) {
    std::memset(px_.data() + static_cast<size_t>(y) * width_ + r.x, q, static_cast<size_t>(r.w));
  }
}

void TileLedger::TakenFromVideo(const Rect& r) {
  for (int32_t y = r.y; y < r.bottom(); ++y) {
    uint8_t* p = px_.data() + static_cast<size_t>(y) * width_ + r.x;
    for (int32_t x = 0; x < r.w; ++x) p[x] = p[x] == kPxExact ? kPxVideo : p[x];
  }
}

uint8_t TileLedger::WorstPixel(const Rect& r) const {
  uint8_t worst = kPxExact;
  for (int32_t y = r.y; y < r.bottom() && worst != kPxVideo; ++y) {
    const uint8_t* p = px_.data() + static_cast<size_t>(y) * width_ + r.x;
    worst = std::max(worst, *std::max_element(p, p + r.w));
  }
  return worst;
}

void TileLedger::ApplyMoves(std::span<const tile::Move> moves) {
  tile::ApplyMovesToPlane(moves, px_.data(), width_, height_, px_scratch_);
}

void TileLedger::SavePixels(const Rect& tile, uint8_t* out) const {
  for (int32_t y = 0; y < tile.h; ++y) {
    std::memcpy(out + static_cast<size_t>(y) * kTileSize, px_.data() + static_cast<size_t>(tile.y + y) * width_ + tile.x,
                static_cast<size_t>(tile.w));
  }
}

void TileLedger::RestorePixels(const Rect& tile, const Rect& r, const uint8_t* saved) {
  for (int32_t y = r.y; y < r.bottom(); ++y) {
    std::memcpy(px_.data() + static_cast<size_t>(y) * width_ + r.x,
                saved + static_cast<size_t>(y - tile.y) * kTileSize + (r.x - tile.x), static_cast<size_t>(r.w));
  }
}

}  // namespace rdacore::session
