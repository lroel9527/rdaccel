#pragma once

// The host's side of the viewer's tile cache (tile/tile_cache.h): which
// content each slot holds, the host's own copy of its pixels, and how the
// viewer had them when the slot was stored.
//
// The viewer stores what it shows for a tile into a slot and paints a slot
// back when told to; it only obeys, so both sides always agree. A slot holds
// the viewer's rendition (exact for lossless pixels, whatever video it had
// otherwise), so a load restores the qualities that were stored with it:
// text that comes back exact is not owed again, and video that was text in
// motion still is. The host's copy of the pixels lets part of a tile be
// painted from the slot (what a dragged window uncovers).

#include <cstdint>
#include <vector>

#include "proto/messages.h"
#include "rdacore/geometry.h"
#include "session/tile_ledger.h"
#include "tile/tile_cache.h"

namespace rdacore::session {

class SlotBook {
 public:
  tile::TileCacheIndex index{proto::kTileCacheSlots};

  // How the viewer had the tile as a whole (TileState::quality).
  uint8_t quality(uint32_t slot) const { return quality_[slot]; }

  // `slot` now holds `r` (a tile) as the viewer shows it: the host's pixels
  // `screen` (a whole screen, rows of `stride` bytes), the ledger's qualities.
  void Keep(uint32_t slot, const Rect& r, const uint8_t* screen, int32_t stride, const TileLedger& ledger,
            uint8_t tile_quality);
  // The host's copy of the slot's pixels: kTileSize x kTileSize BGRA.
  const uint8_t* pixels(uint32_t slot) const { return pixels_.data() + slot * kSlotBytes; }
  // `r` (inside the tile `tile`) is painted from `slot`: its pixels are as
  // the viewer had them when the slot was stored.
  void Restore(uint32_t slot, const Rect& tile, const Rect& r, TileLedger& ledger) const;

 private:
  static constexpr size_t kSlotPixels = static_cast<size_t>(kTileSize) * kTileSize;
  static constexpr size_t kSlotBytes = kSlotPixels * 4;
  std::vector<uint8_t> quality_ = std::vector<uint8_t>(proto::kTileCacheSlots, kRough);
  std::vector<uint8_t> pixels_;         // kTileCacheSlots tiles of kTileSize^2 BGRA, once used
  std::vector<uint8_t> pixel_quality_;  // kTileCacheSlots tiles of kTileSize^2, once used
};

}  // namespace rdacore::session
