#include <gtest/gtest.h>

#include "tile/tile_cache.h"

using namespace rdacore;

TEST(TileCache, FindInsertAndEvictLeastRecentlyUsed) {
  tile::TileCacheIndex cache(2);
  EXPECT_FALSE(cache.Find(10));
  const uint32_t a = cache.Insert(10);
  const uint32_t b = cache.Insert(20);
  EXPECT_NE(a, b);
  EXPECT_EQ(cache.Find(10), a);  // 10 is now the most recent
  const uint32_t c = cache.Insert(30);  // evicts 20
  EXPECT_EQ(c, b);
  EXPECT_FALSE(cache.Find(20));
  EXPECT_EQ(cache.Find(10), a);
  EXPECT_EQ(cache.Find(30), c);
  cache.Clear();
  EXPECT_FALSE(cache.Find(10));
  EXPECT_EQ(cache.Insert(40), 0u);
}
