#include <gtest/gtest.h>

#include <random>
#include <vector>

#include "text_cells.h"

using rdaccel::clip::LabelTextCells;
using rdaccel::clip::TextCellCols;
using rdaccel::clip::TextCellRows;

namespace {

// A BGRA screen drawn in code. `stride` may be larger than w * 4.
struct Screen {
  int32_t w, h, stride;
  std::vector<uint8_t> px;
  Screen(int32_t w_, int32_t h_, int32_t stride_ = 0)
      : w(w_), h(h_), stride(stride_ ? stride_ : w_ * 4), px(static_cast<size_t>(stride) * h_, 0) {}
  void set(int32_t x, int32_t y, uint8_t r, uint8_t g, uint8_t b) {
    uint8_t* p = px.data() + static_cast<size_t>(y) * stride + static_cast<size_t>(x) * 4;
    p[0] = b;
    p[1] = g;
    p[2] = r;
    p[3] = 255;
  }
  void grey(int32_t x, int32_t y, uint8_t v) { set(x, y, v, v, v); }
  void fill(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint8_t r, uint8_t g, uint8_t b) {
    for (int32_t y = y0; y < y1; ++y) {
      for (int32_t x = x0; x < x1; ++x) set(x, y, r, g, b);
    }
  }
  void fill_grey(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint8_t v) { fill(x0, y0, x1, y1, v, v, v); }
  std::vector<uint8_t> label() const {
    std::vector<uint8_t> out;
    LabelTextCells(px.data(), w, h, stride, out);
    return out;
  }
};

// A glyph-like shape in the 16 x 16 cell at (ox, oy): a 2 px vertical bar
// and a 2 px horizontal bar, about 15% of the samples (9 of 64).
void Glyph(Screen& s, int32_t ox, int32_t oy, uint8_t r, uint8_t g, uint8_t b) {
  s.fill(ox + 4, oy + 2, ox + 6, oy + 14, r, g, b);
  s.fill(ox + 4, oy + 8, ox + 12, oy + 10, r, g, b);
}

bool OneCell(const Screen& s) {
  const auto out = s.label();
  EXPECT_EQ(out.size(), 1u);
  return !out.empty() && out[0] == 1;
}

}  // namespace

TEST(TextCells, PlainCellIsNotText) {
  Screen s(16, 16);
  s.fill_grey(0, 0, 16, 16, 240);
  EXPECT_FALSE(OneCell(s));
}

TEST(TextCells, DarkStrokesOnLightBackgroundAreText) {
  Screen s(16, 16);
  s.fill_grey(0, 0, 16, 16, 250);
  Glyph(s, 0, 0, 20, 20, 20);
  EXPECT_TRUE(OneCell(s));
}

TEST(TextCells, LightStrokesOnDarkBackgroundAreText) {
  Screen s(16, 16);
  s.fill_grey(0, 0, 16, 16, 30);
  Glyph(s, 0, 0, 220, 220, 220);
  EXPECT_TRUE(OneCell(s));
}

TEST(TextCells, ColouredStrokesAreText) {
  {
    Screen s(16, 16);  // red on white: Y 76 against 255
    s.fill_grey(0, 0, 16, 16, 255);
    Glyph(s, 0, 0, 255, 0, 0);
    EXPECT_TRUE(OneCell(s));
  }
  {
    Screen s(16, 16);  // blue on black: Y 28 against 0
    s.fill_grey(0, 0, 16, 16, 0);
    Glyph(s, 0, 0, 0, 0, 255);
    EXPECT_TRUE(OneCell(s));
  }
  {
    Screen s(16, 16);  // white on a coloured (blue) background
    s.fill(0, 0, 16, 16, 30, 90, 200);
    Glyph(s, 0, 0, 255, 255, 255);
    EXPECT_TRUE(OneCell(s));
  }
}

TEST(TextCells, EdgeBetweenTwoPlainHalvesIsNotText) {
  {
    Screen s(16, 16);
    s.fill_grey(0, 0, 8, 16, 255);
    s.fill_grey(8, 0, 16, 16, 0);
    EXPECT_FALSE(OneCell(s));
  }
  {
    Screen s(16, 16);  // horizontal, coloured
    s.fill(0, 0, 16, 8, 200, 40, 40);
    s.fill(0, 8, 16, 16, 240, 240, 240);
    EXPECT_FALSE(OneCell(s));
  }
}

TEST(TextCells, NoiseIsNotText) {
  std::mt19937 rng(12345);
  Screen s(16, 16);
  for (int32_t y = 0; y < 16; ++y) {
    for (int32_t x = 0; x < 16; ++x) {
      s.set(x, y, static_cast<uint8_t>(rng()), static_cast<uint8_t>(rng()), static_cast<uint8_t>(rng()));
    }
  }
  EXPECT_FALSE(OneCell(s));
}

TEST(TextCells, PhotoLikeCellIsNotText) {
  // A smooth colour field with a little grain: many luminance values, none
  // dominating.
  std::mt19937 rng(7);
  Screen s(16, 16);
  for (int32_t y = 0; y < 16; ++y) {
    for (int32_t x = 0; x < 16; ++x) {
      const int g = static_cast<int>(rng() % 7) - 3;
      s.set(x, y, static_cast<uint8_t>(60 + 6 * x + g), static_cast<uint8_t>(90 + 4 * y + g),
            static_cast<uint8_t>(40 + 3 * (x + y) + g));
    }
  }
  EXPECT_FALSE(OneCell(s));
}

TEST(TextCells, SlowGradientIsNotText) {
  {
    Screen s(16, 16);  // one level more every pixel across
    for (int32_t y = 0; y < 16; ++y) {
      for (int32_t x = 0; x < 16; ++x) s.grey(x, y, static_cast<uint8_t>(100 + x));
    }
    EXPECT_FALSE(OneCell(s));
  }
  {
    // A dominating shade (half the cell) with neighbouring shades 1-2 levels
    // apart around it: one dominating value, but nothing 16 levels away.
    Screen s(16, 16);
    for (int32_t y = 0; y < 16; ++y) {
      for (int32_t x = 0; x < 16; ++x) s.grey(x, y, static_cast<uint8_t>(y < 8 ? 120 : 120 + (y - 7) * 2));
    }
    EXPECT_FALSE(OneCell(s));
  }
}

TEST(TextCells, ContrastBoundaryIsSixteenLevels) {
  // Grey strokes on grey: luminance equals the grey value.
  for (const int d : {8, 15, -8, -15}) {
    Screen s(16, 16);
    s.fill_grey(0, 0, 16, 16, 128);
    Glyph(s, 0, 0, static_cast<uint8_t>(128 + d), static_cast<uint8_t>(128 + d), static_cast<uint8_t>(128 + d));
    EXPECT_FALSE(OneCell(s)) << "strokes " << d << " levels from the background";
  }
  for (const int d : {16, 17, -16, -17}) {
    Screen s(16, 16);
    s.fill_grey(0, 0, 16, 16, 128);
    Glyph(s, 0, 0, static_cast<uint8_t>(128 + d), static_cast<uint8_t>(128 + d), static_cast<uint8_t>(128 + d));
    EXPECT_TRUE(OneCell(s)) << "strokes " << d << " levels from the background";
  }
}

TEST(TextCells, OneStraySampleIsNotTextTwoAre) {
  // Only every second pixel in both directions is sampled: (even, even).
  {
    Screen s(16, 16);
    s.fill_grey(0, 0, 16, 16, 255);
    s.grey(6, 6, 0);
    EXPECT_FALSE(OneCell(s));
  }
  {
    Screen s(16, 16);
    s.fill_grey(0, 0, 16, 16, 255);
    s.grey(6, 6, 0);
    s.grey(10, 12, 0);
    EXPECT_TRUE(OneCell(s));
  }
  {
    // The two need not share a value: each only has to be 16+ levels away.
    Screen s(16, 16);
    s.fill_grey(0, 0, 16, 16, 128);
    s.grey(2, 2, 112);  // 16 below
    s.grey(4, 4, 144);  // 16 above
    EXPECT_TRUE(OneCell(s));
  }
  {
    // One sample 16 away and one 15 away: only one counts.
    Screen s(16, 16);
    s.fill_grey(0, 0, 16, 16, 128);
    s.grey(2, 2, 112);
    s.grey(4, 4, 143);
    EXPECT_FALSE(OneCell(s));
  }
}

TEST(TextCells, PixelsBetweenSamplesAreNotSeen) {
  // Pixels at odd x or odd y are not sampled: two dark pixels there leave
  // the cell plain.
  Screen s(16, 16);
  s.fill_grey(0, 0, 16, 16, 255);
  s.grey(5, 6, 0);
  s.grey(6, 7, 0);
  EXPECT_FALSE(OneCell(s));
}

TEST(TextCells, DominantValueMustHoldAThirdOfTheSamples) {
  // 64 samples: a third is 22. Background on 21 samples, the rest a noise of
  // distinct far values: no dominating value.
  {
    Screen s(16, 16);
    int k = 0;
    for (int32_t y = 0; y < 16; y += 2) {
      for (int32_t x = 0; x < 16; x += 2, ++k) s.grey(x, y, static_cast<uint8_t>(k < 21 ? 200 : k));
    }
    EXPECT_FALSE(OneCell(s));
  }
  {
    Screen s(16, 16);
    int k = 0;
    for (int32_t y = 0; y < 16; y += 2) {
      for (int32_t x = 0; x < 16; x += 2, ++k) s.grey(x, y, static_cast<uint8_t>(k < 22 ? 200 : k));
    }
    EXPECT_TRUE(OneCell(s));
  }
}

TEST(TextCells, PartialCellsAtTheRightAndBottomEdges) {
  // 40 x 24: 3 x 2 cells, the last column 8 px wide and the last row 8 px
  // high.
  Screen s(40, 24);
  s.fill_grey(0, 0, 40, 24, 250);
  EXPECT_EQ(TextCellCols(40), 3);
  EXPECT_EQ(TextCellRows(24), 2);
  {
    const auto out = s.label();
    ASSERT_EQ(out.size(), 6u);
    for (const uint8_t c : out) EXPECT_EQ(c, 0);
  }
  // Text in the corner cell (32..39, 16..23): 16 samples, a third is 6.
  s.fill_grey(34, 16, 36, 24, 0);
  // Text in the bottom-left cell (0..15, 16..23).
  s.fill_grey(4, 18, 10, 20, 0);
  // Text in the right-edge cell of the top row (32..39, 0..15).
  s.fill_grey(32, 6, 40, 8, 0);
  const auto out = s.label();
  ASSERT_EQ(out.size(), 6u);
  EXPECT_EQ(out[0], 0);
  EXPECT_EQ(out[1], 0);
  EXPECT_EQ(out[2], 1);
  EXPECT_EQ(out[3], 1);
  EXPECT_EQ(out[4], 0);
  EXPECT_EQ(out[5], 1);
}

TEST(TextCells, SizesThatAreNotMultiplesOfSixteen) {
  for (const auto& [w, h] : {std::pair{1, 1}, std::pair{17, 15}, std::pair{33, 49}, std::pair{1920, 1080}}) {
    Screen s(w, h);
    const auto out = s.label();
    EXPECT_EQ(out.size(), static_cast<size_t>(TextCellCols(w)) * TextCellRows(h)) << w << " x " << h;
  }
}

TEST(TextCells, StrideIsHonoured) {
  // 32 x 32 with 16 pixels of padding per row. Text only in the top-right
  // cell; the padding holds noise that must not be read.
  const int32_t w = 32, h = 32, stride = (w + 16) * 4;
  Screen s(w, h, stride);
  std::mt19937 rng(99);
  for (auto& v : s.px) v = static_cast<uint8_t>(rng());
  s.fill_grey(0, 0, w, h, 250);
  Glyph(s, 16, 0, 0, 0, 0);
  const auto out = s.label();
  ASSERT_EQ(out.size(), 4u);
  EXPECT_EQ(out[0], 0);
  EXPECT_EQ(out[1], 1);
  EXPECT_EQ(out[2], 0);
  EXPECT_EQ(out[3], 0);
}
