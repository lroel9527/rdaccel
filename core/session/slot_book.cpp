#include "session/slot_book.h"

#include <cstring>

namespace rdacore::session {

void SlotBook::Keep(uint32_t slot, const Rect& r, const uint8_t* screen, int32_t stride, const TileLedger& ledger,
                    uint8_t tile_quality) {
  quality_[slot] = tile_quality;
  if (pixels_.empty()) pixels_.resize(proto::kTileCacheSlots * kSlotBytes);
  for (int32_t y = 0; y < r.h; ++y) {
    std::memcpy(pixels_.data() + slot * kSlotBytes + static_cast<size_t>(y) * kTileSize * 4,
                screen + static_cast<size_t>(r.y + y) * stride + static_cast<size_t>(r.x) * 4, static_cast<size_t>(r.w) * 4);
  }
  if (pixel_quality_.empty()) pixel_quality_.resize(proto::kTileCacheSlots * kSlotPixels, kPxVideo);
  ledger.SavePixels(r, pixel_quality_.data() + slot * kSlotPixels);
}

void SlotBook::Restore(uint32_t slot, const Rect& tile, const Rect& r, TileLedger& ledger) const {
  if (pixel_quality_.size() < (static_cast<size_t>(slot) + 1) * kSlotPixels) {
    ledger.SetPixels(r, quality_[slot] == kExact ? kPxExact : kPxVideo);
    return;
  }
  ledger.RestorePixels(tile, r, pixel_quality_.data() + slot * kSlotPixels);
  // Poor video comes back poor, and is to be taken again where it is now.
  if (ledger.HasPoor(r)) ledger.NotePoor();
}

}  // namespace rdacore::session
