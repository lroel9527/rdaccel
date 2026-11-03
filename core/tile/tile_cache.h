#pragma once

// Host-side index of the viewer's tile cache.
//
// The viewer keeps `capacity` slots of tile pixels. The host decides
// everything: it tells the viewer to store what it currently shows for a tile
// into a slot (CacheStore), and later to paint a slot into a tile
// (CacheLoad). The viewer only obeys, so both sides always agree.
//
// Slots are keyed by the tile content hash from TileDiffer. What a slot holds
// is the viewer's rendition of that content (exact for lossless tiles, the
// refined video picture otherwise), which is what it showed last time.

#include <cstdint>
#include <list>
#include <optional>
#include <unordered_map>

namespace rdaccel::tile {

class TileCacheIndex {
 public:
  explicit TileCacheIndex(uint32_t capacity) : capacity_(capacity) {}

  uint32_t capacity() const { return capacity_; }

  // The slot holding `hash`, if any; marks it most recently used.
  std::optional<uint32_t> Find(uint64_t hash);

  // True if `slot` still holds `hash`. Does not change the LRU order.
  bool Holds(uint32_t slot, uint64_t hash) const {
    auto it = map_.find(hash);
    return it != map_.end() && it->second->slot == slot;
  }

  // Assigns a slot to `hash` (which must not be present), evicting the least
  // recently used entry when full.
  uint32_t Insert(uint64_t hash);

  void Clear();

 private:
  struct Entry {
    uint64_t hash;
    uint32_t slot;
  };
  uint32_t capacity_;
  std::list<Entry> lru_;  // front = most recent
  std::unordered_map<uint64_t, std::list<Entry>::iterator> map_;
};

}  // namespace rdaccel::tile
