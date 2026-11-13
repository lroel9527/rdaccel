#include <cstring>

#include <gtest/gtest.h>

#include "tile/motion.h"

using namespace rdacore;

namespace {

constexpr int W = 512, H = 384;

// Detailed, non-repeating content (every row differs).
std::vector<uint8_t> Noise(int seed) {
  std::vector<uint8_t> px(static_cast<size_t>(W) * H * 4);
  uint32_t s = 0x9e3779b9u * static_cast<uint32_t>(seed + 1);
  for (size_t i = 0; i < px.size(); ++i) {
    s = s * 1664525u + 1013904223u;
    px[i] = (i % 4 == 3) ? 255 : static_cast<uint8_t>(s >> 24);
  }
  return px;
}

ImageView View(const std::vector<uint8_t>& px) { return {px.data(), W, H, W * 4}; }

}  // namespace

TEST(Motion, DetectsScrollAndApplyReproducesTheFrame) {
  const auto prev = Noise(1);
  // Everything below y=64 scrolls up by 7 pixels; new rows at the bottom.
  auto cur = prev;
  const auto fresh = Noise(2);
  for (int y = 64; y < H; ++y) {
    const auto& src = y + 7 < H ? prev : fresh;
    const int sy = y + 7 < H ? y + 7 : y;
    std::memcpy(&cur[static_cast<size_t>(y) * W * 4], &src[static_cast<size_t>(sy) * W * 4], W * 4);
  }

  tile::TileDiffer differ;
  differ.Diff(View(prev), {});
  const auto changes = differ.Diff(View(cur), {});
  std::vector<bool> moved;
  std::vector<std::vector<Rect>> residual;
  const auto moves = tile::DetectMoves(View(prev), View(cur), changes, moved, residual);
  ASSERT_FALSE(moves.empty());
  for (const auto& m : moves) {
    EXPECT_EQ(m.dx, 0);
    EXPECT_EQ(m.dy, -7);
  }
  // Rows 64..319 are full tiles of scrolled content. In the last tile row
  // only the 7 new rows at the bottom are left to code.
  size_t n_full = 0, n_partial = 0;
  for (size_t i = 0; i < changes.size(); ++i) {
    ASSERT_TRUE(moved[i]);
    const Rect& r = changes[i].rect;
    if (residual[i].empty()) {
      ++n_full;
      EXPECT_LT(r.bottom(), H);
    } else {
      ++n_partial;
      ASSERT_EQ(residual[i].size(), 1u);
      EXPECT_EQ(residual[i][0], (Rect{r.x, H - 7, 64, 7}));
    }
  }
  EXPECT_EQ(n_full, 8u * 4u);
  EXPECT_EQ(n_partial, 8u);

  // Applying the moves to the previous frame gives the current one wherever
  // nothing is left to code.
  auto screen = prev;
  std::vector<uint8_t> scratch;
  tile::ApplyMoves(moves, screen.data(), W, H, scratch);
  for (int y = 0; y < H - 7; ++y) {
    const size_t off = static_cast<size_t>(y) * W * 4;
    ASSERT_EQ(std::memcmp(&screen[off], &cur[off], W * 4), 0) << "row " << y;
  }
}

TEST(Motion, APlaneOfOneByteAPixelMovesLikeThePicture) {
  // 8 x 6, numbered; two moves that read each other's destination: both
  // must see the plane as it was.
  std::vector<uint8_t> plane(8 * 6);
  for (size_t i = 0; i < plane.size(); ++i) plane[i] = static_cast<uint8_t>(i);
  const auto was = plane;
  const std::vector<tile::Move> moves = {{{0, 0, 4, 2}, 0, -2}, {{0, 2, 4, 2}, 0, 2}, {{6, 0, 2, 9}, 0, 0}};
  std::vector<uint8_t> scratch;
  tile::ApplyMovesToPlane(moves, plane.data(), 8, 6, scratch);
  for (int y = 0; y < 6; ++y) {
    for (int x = 0; x < 8; ++x) {
      int from = y;
      if (x < 4 && y < 2) from = y + 2;
      else if (x < 4 && y < 4) from = y - 2;
      EXPECT_EQ(plane[static_cast<size_t>(y) * 8 + x], was[static_cast<size_t>(from) * 8 + x]) << x << "," << y;
    }
  }
}

TEST(Motion, DetectsWindowDrag) {
  const auto bg = Noise(3);
  const auto win = Noise(4);
  auto place = [&](int wx, int wy) {
    auto img = bg;
    for (int y = 0; y < 192; ++y) {
      std::memcpy(&img[(static_cast<size_t>(wy + y) * W + wx) * 4], &win[static_cast<size_t>(y) * W * 4],
                  192 * 4);
    }
    return img;
  };
  const auto prev = place(40, 30);
  const auto cur = place(40 + 64 + 13, 30 + 64 + 5);  // tiles fully inside the window move
  tile::TileDiffer differ;
  differ.Diff(View(prev), {});
  const auto changes = differ.Diff(View(cur), {});
  std::vector<bool> moved;
  std::vector<std::vector<Rect>> residual;
  const auto moves = tile::DetectMoves(View(prev), View(cur), changes, moved, residual);
  ASSERT_FALSE(moves.empty());
  EXPECT_EQ(moves[0].dx, 77);
  EXPECT_EQ(moves[0].dy, 69);
}

TEST(Motion, NothingDetectedForUnrelatedContent) {
  const auto prev = Noise(5), cur = Noise(6);
  tile::TileDiffer differ;
  differ.Diff(View(prev), {});
  const auto changes = differ.Diff(View(cur), {});
  std::vector<bool> moved;
  std::vector<std::vector<Rect>> residual;
  EXPECT_TRUE(tile::DetectMoves(View(prev), View(cur), changes, moved, residual).empty());
  for (bool m : moved) EXPECT_FALSE(m);
}

TEST(Motion, AnUnchangedTileJoinsTheMoveOnlyWhereItsSourceMayBeCopied) {
  // A blank block that looks the same before and after a scroll by 7: the
  // tile at (2,2) and the 7 rows below it.
  auto page = Noise(3);
  const auto more = Noise(4);
  page.insert(page.end(), more.begin(), more.begin() + static_cast<size_t>(7) * W * 4);
  for (int y = 128; y < 192 + 7; ++y) {
    for (int x = 128; x < 192; ++x) std::memset(&page[(static_cast<size_t>(y) * W + x) * 4], 255, 4);
  }
  const std::vector<uint8_t> prev(page.begin(), page.begin() + static_cast<size_t>(W) * H * 4);
  const std::vector<uint8_t> cur(page.begin() + static_cast<size_t>(7) * W * 4, page.end());
  tile::TileDiffer differ;
  differ.Diff(View(prev), {});
  const auto changes = differ.Diff(View(cur), {});
  for (const auto& c : changes) ASSERT_FALSE(c.col == 2 && c.row == 2);
  const Rect blank{128, 128, 64, 64};
  auto covers_blank = [&](const std::vector<tile::Move>& moves) {
    for (const auto& m : moves) {
      if (m.dst.intersects(blank)) return true;
    }
    return false;
  };
  std::vector<bool> moved;
  std::vector<std::vector<Rect>> residual;
  EXPECT_TRUE(covers_blank(tile::DetectMoves(View(prev), View(cur), changes, moved, residual)));
  tile::MotionOptions opts;
  std::vector<Rect> asked;
  opts.may_copy_unchanged = [&](const Rect& src) {
    asked.push_back(src);
    return false;
  };
  const auto moves = tile::DetectMoves(View(prev), View(cur), changes, moved, residual, opts);
  EXPECT_FALSE(moves.empty());
  EXPECT_FALSE(covers_blank(moves));
  ASSERT_EQ(asked.size(), 1u);
  EXPECT_EQ(asked[0], (Rect{128, 135, 64, 64}));
}
