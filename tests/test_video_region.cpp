#include <gtest/gtest.h>

#include "tile/video_region.h"

using namespace rdacore;

namespace {

// A 10x6 tile screen (640x384).
constexpr int32_t kCols = 10, kRows = 6;

std::vector<uint8_t> Mask(const Rect& tiles) {
  std::vector<uint8_t> m(kCols * kRows, 0);
  for (int32_t r = tiles.y; r < tiles.bottom(); ++r) {
    for (int32_t c = tiles.x; c < tiles.right(); ++c) m[r * kCols + c] = 1;
  }
  return m;
}

}  // namespace

TEST(VideoRegion, BusyBlockBecomesAnAreaAndClosesWhenQuiet) {
  tile::VideoRegionParams p;
  p.window = 10;
  p.min_busy = 6;
  p.confirm = 2;
  p.linger = 5;
  tile::VideoRegionTracker t(p);
  t.Reset(kCols * 64, kRows * 64);
  const auto busy = Mask({2, 1, 3, 2});
  int frames = 0;
  while (t.regions().empty() && frames < 20) {
    EXPECT_TRUE(t.Update(busy, busy).empty());
    ++frames;
  }
  // 6 busy frames to be active, then 2 to confirm.
  EXPECT_EQ(frames, 7);
  ASSERT_EQ(t.regions().size(), 1u);
  EXPECT_EQ(t.regions()[0].rect, (Rect{128, 64, 192, 128}));
  EXPECT_EQ(t.RegionOf(2, 1), 0);
  EXPECT_EQ(t.RegionOf(4, 2), 0);
  EXPECT_EQ(t.RegionOf(5, 2), -1);

  const std::vector<uint8_t> none(kCols * kRows, 0);
  std::vector<tile::VideoRegion> closed;
  for (int i = 0; i < 5 && closed.empty(); ++i) closed = t.Update(none, none);
  ASSERT_EQ(closed.size(), 1u);
  EXPECT_EQ(closed[0].rect, (Rect{128, 64, 192, 128}));
  EXPECT_TRUE(t.regions().empty());
}

TEST(VideoRegion, ThinStripsAndSparseTilesAreIgnored) {
  tile::VideoRegionParams p;
  p.window = 4;
  p.min_busy = 2;
  p.confirm = 1;
  tile::VideoRegionTracker t(p);
  t.Reset(kCols * 64, kRows * 64);
  // A one-tile-high strip (the rows a scroll uncovers) and a diagonal.
  auto busy = Mask({0, 5, 10, 1});
  for (int i = 0; i < 3; ++i) busy[i * kCols + i] = 1;
  for (int i = 0; i < 10; ++i) t.Update(busy, busy);
  EXPECT_TRUE(t.regions().empty());
}

TEST(VideoRegion, AreaGrowsIntoANewStream) {
  tile::VideoRegionParams p;
  p.window = 4;
  p.min_busy = 2;
  p.confirm = 1;
  p.min_resize_gap = 3;
  tile::VideoRegionTracker t(p);
  t.Reset(kCols * 64, kRows * 64);
  auto small = Mask({1, 1, 2, 2});
  for (int i = 0; i < 3; ++i) t.Update(small, small);
  ASSERT_EQ(t.regions().size(), 1u);
  const uint8_t first = t.regions()[0].stream;
  auto big = Mask({1, 1, 4, 3});
  for (int i = 0; i < 8; ++i) t.Update(big, big);
  ASSERT_EQ(t.regions().size(), 1u);
  EXPECT_EQ(t.regions()[0].rect, (Rect{64, 64, 256, 192}));
  EXPECT_NE(t.regions()[0].stream, first);
}

TEST(VideoRegion, OddScreenEdgeIsLeftOut) {
  tile::VideoRegionParams p;
  p.window = 4;
  p.min_busy = 2;
  p.confirm = 1;
  tile::VideoRegionTracker t(p);
  // 4 columns, the last one 31 pixels wide.
  t.Reset(3 * 64 + 31, 2 * 64);
  std::vector<uint8_t> busy(8, 1);
  for (int i = 0; i < 4; ++i) t.Update(busy, busy);
  ASSERT_EQ(t.regions().size(), 1u);
  EXPECT_EQ(t.regions()[0].rect, (Rect{0, 0, 192, 128}));
  EXPECT_EQ(t.RegionOf(3, 0), -1);
}
