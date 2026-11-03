#include "tile/tile_cache.h"

namespace rdacore::tile {

std::optional<uint32_t> TileCacheIndex::Find(uint64_t hash) {
  auto it = map_.find(hash);
  if (it == map_.end()) return std::nullopt;
  lru_.splice(lru_.begin(), lru_, it->second);
  return it->second->slot;
}

uint32_t TileCacheIndex::Insert(uint64_t hash) {
  uint32_t slot;
  if (lru_.size() < capacity_) {
    slot = static_cast<uint32_t>(lru_.size());
  } else {
    slot = lru_.back().slot;
    map_.erase(lru_.back().hash);
    lru_.pop_back();
  }
  lru_.push_front({hash, slot});
  map_[hash] = lru_.begin();
  return slot;
}

void TileCacheIndex::Clear() {
  lru_.clear();
  map_.clear();
}

}  // namespace rdacore::tile
