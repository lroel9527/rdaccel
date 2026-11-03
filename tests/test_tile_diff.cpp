#include <gtest/gtest.h>

#include <vector>

#include "tile/tile_diff.h"

using namespace rdaccel;
using namespace rdaccel::tile;

namespace {

struct Image {
  int32_t w, h;
  std::vector<uint8_t> px;
  Image(int32_t w_, int32_t h_) : w(w_), h(h_), px(static_cast<size_t>(w_) * h_ * 4, 0) {}
  ImageView view() const { return {px.data(), w, h, w * 4}; }
  void set(int32_t x, int32_t y, uint8_t v) { px[(static_cast<size_t>(y) * w + x) * 4] = v; }
};

}  // namespace

TEST(TileDiff, FirstFrameReportsAllTiles) {
  Image img(200, 100);  // 4 x 2 tiles, last column and row partial
  TileDiffer d;
  auto changes = d.Diff(img.view(), {});
  EXPECT_EQ(d.cols(), 4);
  EXPECT_EQ(d.rows(), 2);
  EXPECT_EQ(changes.size(), 8u);
  // Edge tiles are clipped to the frame.
  EXPECT_EQ(changes.back().rect, (Rect{192, 64, 8, 36}));
}

TEST(TileDiff, UnchangedFrameReportsNothing) {
  Image img(256, 256);
  TileDiffer d;
  d.Diff(img.view(), {});
  EXPECT_TRUE(d.Diff(img.view(), {}).empty());
}

TEST(TileDiff, SinglePixelChangeReportsOneTile) {
  Image img(256, 256);
  TileDiffer d;
  d.Diff(img.view(), {});
  img.set(130, 70, 9);
  auto changes = d.Diff(img.view(), {});
  ASSERT_EQ(changes.size(), 1u);
  EXPECT_EQ(changes[0].col, 2);
  EXPECT_EQ(changes[0].row, 1);
}

TEST(TileDiff, DirtyRectThatRepaintedSamePixelsReportsNothing) {
  Image img(256, 256);
  TileDiffer d;
  d.Diff(img.view(), {});
  const Rect dirty[] = {{0, 0, 256, 256}};
  EXPECT_TRUE(d.Diff(img.view(), dirty).empty());
}

TEST(TileDiff, ChangesOutsideDirtyRectsAreIgnored) {
  Image img(256, 256);
  TileDiffer d;
  d.Diff(img.view(), {});
  img.set(10, 10, 1);    // tile (0, 0)
  img.set(200, 200, 1);  // tile (3, 3)
  const Rect dirty[] = {{190, 190, 20, 20}};
  auto changes = d.Diff(img.view(), dirty);
  ASSERT_EQ(changes.size(), 1u);
  EXPECT_EQ(changes[0].col, 3);
  EXPECT_EQ(changes[0].row, 3);
}

TEST(TileDiff, DirtyRectSpanningTileBoundaryChecksBothTiles) {
  Image img(256, 64);
  TileDiffer d;
  d.Diff(img.view(), {});
  img.set(63, 0, 5);
  img.set(64, 0, 5);
  const Rect dirty[] = {{60, 0, 8, 1}};
  EXPECT_EQ(d.Diff(img.view(), dirty).size(), 2u);
}

TEST(TileDiff, ResizeResetsState) {
  Image a(128, 128), b(192, 128);
  TileDiffer d;
  d.Diff(a.view(), {});
  EXPECT_EQ(d.Diff(b.view(), {}).size(), 6u);
}

TEST(TileDiff, HashDependsOnContentAndStride) {
  Image img(64, 64);
  const Rect r{0, 0, 64, 64};
  const uint64_t h0 = HashRect(img.view(), r);
  EXPECT_NE(h0, 0u);
  img.set(5, 5, 1);
  EXPECT_NE(HashRect(img.view(), r), h0);

  // The same pixels in a buffer with row padding hash the same.
  Image padded(80, 64);
  for (int y = 0; y < 64; ++y)
    for (int x = 0; x < 64; ++x)
      for (int c = 0; c < 4; ++c)
        padded.px[(static_cast<size_t>(y) * 80 + x) * 4 + c] =
            img.px[(static_cast<size_t>(y) * 64 + x) * 4 + c];
  EXPECT_EQ(HashRect(padded.view(), r), HashRect(img.view(), r));
}
