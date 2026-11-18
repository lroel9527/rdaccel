// Tests of rdacore::tile::LooksLikeText (core/tile/text_cell.h), the
// library's own check for "does this rect look like text". It looks at every
// pixel of the rect; the clip tool's LabelTextCells (tools/clip/text_cells.h,
// tested in test_text_cells.cpp) samples every other pixel instead.

#include <gtest/gtest.h>

#include <random>
#include <vector>

#include "tile/text_cell.h"

using rdacore::ImageView;
using rdacore::Rect;
using rdacore::tile::kTextContrast;
using rdacore::tile::LooksLikeText;

namespace {

// A BGRA image drawn in code. `stride` may be larger than w * 4; the padding
// at the end of each row is filled with `pad` so that reading it by mistake
// shows up.
struct Img {
  int32_t w, h, stride;
  std::vector<uint8_t> px;
  Img(int32_t w_, int32_t h_, int32_t stride_ = 0, uint8_t pad = 0)
      : w(w_), h(h_), stride(stride_ ? stride_ : w_ * 4), px(static_cast<size_t>(stride) * h_, pad) {
    fill_grey(0, 0, w, h, 0);
  }
  ImageView view() const { return {px.data(), w, h, stride}; }
  Rect all() const { return {0, 0, w, h}; }
  void set(int32_t x, int32_t y, uint8_t r, uint8_t g, uint8_t b) {
    uint8_t* p = px.data() + static_cast<size_t>(y) * stride + static_cast<size_t>(x) * 4;
    p[0] = b;
    p[1] = g;
    p[2] = r;
    p[3] = 255;
  }
  // Grey v has luminance exactly v: (77 + 150 + 29) * v >> 8 == v.
  void grey(int32_t x, int32_t y, uint8_t v) { set(x, y, v, v, v); }
  void fill(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint8_t r, uint8_t g, uint8_t b) {
    for (int32_t y = y0; y < y1; ++y) {
      for (int32_t x = x0; x < x1; ++x) set(x, y, r, g, b);
    }
  }
  void fill_grey(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint8_t v) { fill(x0, y0, x1, y1, v, v, v); }
  bool text() const { return LooksLikeText(view(), all()); }
  bool text(const Rect& r) const { return LooksLikeText(view(), r); }
};

// A glyph-like shape in the 16 x 16 square at (ox, oy): a 2 px vertical bar
// and a 2 px horizontal bar, 36 of the 256 pixels.
void Glyph(Img& s, int32_t ox, int32_t oy, uint8_t r, uint8_t g, uint8_t b) {
  s.fill(ox + 4, oy + 2, ox + 6, oy + 14, r, g, b);
  s.fill(ox + 4, oy + 8, ox + 12, oy + 10, r, g, b);
}

// Fills `count` pixels with grey v, row by row starting at pixel index
// `*next`; advances `*next`.
void FillRun(Img& s, int32_t* next, int32_t count, uint8_t v) {
  for (int32_t k = 0; k < count; ++k, ++*next) s.grey(*next % s.w, *next / s.w, v);
}

}  // namespace

TEST(TextCell, PlainIsNotText) {
  Img s(16, 16);
  s.fill_grey(0, 0, 16, 16, 240);
  EXPECT_FALSE(s.text());
  s.fill_grey(0, 0, 16, 16, 0);
  EXPECT_FALSE(s.text());
}

TEST(TextCell, DarkStrokesOnLightAreText) {
  Img s(16, 16);
  s.fill_grey(0, 0, 16, 16, 250);
  Glyph(s, 0, 0, 20, 20, 20);
  EXPECT_TRUE(s.text());
}

TEST(TextCell, LightStrokesOnDarkAreText) {
  Img s(16, 16);
  s.fill_grey(0, 0, 16, 16, 30);
  Glyph(s, 0, 0, 220, 220, 220);
  EXPECT_TRUE(s.text());
}

TEST(TextCell, ColouredStrokesAreText) {
  {
    Img s(16, 16);  // red on white: Y 76 against 255
    s.fill_grey(0, 0, 16, 16, 255);
    Glyph(s, 0, 0, 255, 0, 0);
    EXPECT_TRUE(s.text());
  }
  {
    Img s(16, 16);  // blue on black: Y 28 against 0
    s.fill_grey(0, 0, 16, 16, 0);
    Glyph(s, 0, 0, 0, 0, 255);
    EXPECT_TRUE(s.text());
  }
  {
    Img s(16, 16);  // white on a green background: Y 255 against 149
    s.fill(0, 0, 16, 16, 0, 255, 0);
    Glyph(s, 0, 0, 255, 255, 255);
    EXPECT_TRUE(s.text());
  }
}

TEST(TextCell, EdgeBetweenTwoPlainHalvesIsNotText) {
  Img s(16, 16);
  s.fill_grey(0, 0, 8, 16, 30);
  s.fill_grey(8, 0, 16, 16, 220);
  EXPECT_FALSE(s.text());  // two values hold a third each
  Img t(16, 16);
  t.fill_grey(0, 0, 16, 8, 220);
  t.fill_grey(0, 8, 16, 16, 30);
  EXPECT_FALSE(t.text());
}

TEST(TextCell, NoiseIsNotText) {
  Img s(16, 16);
  std::mt19937 rng(7);
  std::uniform_int_distribution<int> d(0, 255);
  for (int32_t y = 0; y < 16; ++y) {
    for (int32_t x = 0; x < 16; ++x) {
      s.set(x, y, static_cast<uint8_t>(d(rng)), static_cast<uint8_t>(d(rng)), static_cast<uint8_t>(d(rng)));
    }
  }
  EXPECT_FALSE(s.text());
}

TEST(TextCell, SlowGradientIsNotText) {
  {
    // A ramp, one level per column: no value holds a third.
    Img s(16, 16);
    for (int32_t x = 0; x < 16; ++x) s.fill_grey(x, 0, x + 1, 16, static_cast<uint8_t>(100 + x));
    EXPECT_FALSE(s.text());
  }
  {
    // A flat part that dominates, then a ramp that never gets 16 levels away.
    Img s(16, 16);
    s.fill_grey(0, 0, 6, 16, 100);  // 96 pixels
    for (int32_t x = 6; x < 16; ++x) s.fill_grey(x, 0, x + 1, 16, static_cast<uint8_t>(100 + (x - 5)));  // 101..110
    EXPECT_FALSE(s.text());
  }
}

TEST(TextCell, ContrastBoundary) {
  ASSERT_EQ(kTextContrast, 16);
  for (int sign : {1, -1}) {
    Img s(16, 16);
    s.fill_grey(0, 0, 16, 16, 100);
    s.grey(3, 4, static_cast<uint8_t>(100 + sign * 15));
    s.grey(9, 11, static_cast<uint8_t>(100 + sign * 15));
    EXPECT_FALSE(s.text()) << "15 levels, sign " << sign;
    s.grey(3, 4, static_cast<uint8_t>(100 + sign * 16));
    s.grey(9, 11, static_cast<uint8_t>(100 + sign * 16));
    EXPECT_TRUE(s.text()) << "16 levels, sign " << sign;
  }
}

TEST(TextCell, OneStrayPixelIsNotTextTwoAre) {
  Img s(16, 16);
  s.fill_grey(0, 0, 16, 16, 240);
  s.grey(5, 5, 0);
  EXPECT_FALSE(s.text());
  s.grey(12, 2, 0);
  EXPECT_TRUE(s.text());
}

// Every pixel is looked at, so a one-pixel line on an odd row or column is
// seen. The clip tool's sampled LabelTextCells looks only at even
// coordinates and misses it; this is where the two differ.
TEST(TextCell, OnePixelLineOnOddRowOrColumnIsSeen) {
  {
    Img s(16, 16);
    s.fill_grey(0, 0, 16, 16, 240);
    s.fill_grey(0, 7, 16, 8, 20);  // row 7
    EXPECT_TRUE(s.text());
  }
  {
    Img s(16, 16);
    s.fill_grey(0, 0, 16, 16, 240);
    s.fill_grey(7, 0, 8, 16, 20);  // column 7
    EXPECT_TRUE(s.text());
  }
  {
    Img s(16, 16);  // two pixels, both on odd coordinates
    s.fill_grey(0, 0, 16, 16, 240);
    s.grey(3, 5, 20);
    s.grey(11, 13, 20);
    EXPECT_TRUE(s.text());
  }
}

// "At least a third" of 256 pixels: 256 / 3 = 85.3, so 86 are needed; the
// header computes (256 + 2) / 3 = 86.
TEST(TextCell, AThirdOfA16x16Rect) {
  {
    Img s(16, 16);
    int32_t next = 0;
    FillRun(s, &next, 85, 100);
    FillRun(s, &next, 85, 150);
    FillRun(s, &next, 85, 200);
    FillRun(s, &next, 1, 250);
    ASSERT_EQ(next, 256);
    EXPECT_FALSE(s.text());  // no value holds 86
  }
  {
    Img s(16, 16);
    int32_t next = 0;
    FillRun(s, &next, 86, 100);
    FillRun(s, &next, 85, 150);
    FillRun(s, &next, 85, 200);
    ASSERT_EQ(next, 256);
    EXPECT_TRUE(s.text());  // 100 holds 86, the rest is far from it
  }
  {
    Img s(16, 16);
    int32_t next = 0;
    FillRun(s, &next, 86, 100);
    FillRun(s, &next, 86, 150);
    FillRun(s, &next, 84, 200);
    ASSERT_EQ(next, 256);
    EXPECT_FALSE(s.text());  // two values hold 86
  }
}

TEST(TextCell, StripOf16x5) {
  Img s(16, 5);
  s.fill_grey(0, 0, 16, 5, 240);
  EXPECT_FALSE(s.text());
  s.fill_grey(0, 3, 16, 4, 20);  // a one-pixel line on row 3
  EXPECT_TRUE(s.text());
  // 80 pixels: a third is 26.7, so 27 are needed ((80 + 2) / 3).
  Img b(16, 5);
  int32_t next = 0;
  FillRun(b, &next, 26, 100);
  FillRun(b, &next, 26, 150);
  FillRun(b, &next, 26, 200);
  FillRun(b, &next, 2, 250);
  EXPECT_FALSE(b.text());
  next = 0;
  FillRun(b, &next, 27, 100);
  FillRun(b, &next, 26, 150);
  FillRun(b, &next, 26, 200);
  FillRun(b, &next, 1, 250);
  EXPECT_TRUE(b.text());
}

TEST(TextCell, StripOf7x16) {
  Img s(7, 16);
  s.fill_grey(0, 0, 7, 16, 30);
  EXPECT_FALSE(s.text());
  s.fill_grey(3, 0, 4, 16, 220);  // a one-pixel line on column 3
  EXPECT_TRUE(s.text());
  Img e(7, 16);  // an edge
  e.fill_grey(0, 0, 3, 16, 30);
  e.fill_grey(3, 0, 7, 16, 220);
  EXPECT_FALSE(e.text());
}

TEST(TextCell, Rect64x64) {
  Img s(64, 64);
  s.fill_grey(0, 0, 64, 64, 250);
  EXPECT_FALSE(s.text());
  for (int32_t y = 0; y < 64; y += 16) {
    for (int32_t x = 0; x < 64; x += 16) Glyph(s, x, y, 20, 20, 20);
  }
  EXPECT_TRUE(s.text());
  Img e(64, 64);
  e.fill_grey(0, 0, 32, 64, 30);
  e.fill_grey(32, 0, 64, 64, 220);
  EXPECT_FALSE(e.text());
}

TEST(TextCell, RectInsideALargerImageWithPaddedRows) {
  // 64 x 48 with 40 bytes of padding per row, filled with 0; the rect is
  // light grey. Around the rect: dark lines right next to each of its sides,
  // which would make it text if any pixel outside it were counted.
  Img s(64, 48, 64 * 4 + 40, 0);
  s.fill_grey(0, 0, 64, 48, 240);
  const Rect r{20, 13, 16, 16};
  s.fill_grey(r.x - 1, 0, r.x, 48, 0);
  s.fill_grey(r.right(), 0, r.right() + 1, 48, 0);
  s.fill_grey(0, r.y - 1, 64, r.y, 0);
  s.fill_grey(0, r.bottom(), 64, r.bottom() + 1, 0);
  EXPECT_FALSE(s.text(r));
  Glyph(s, r.x, r.y, 20, 20, 20);
  EXPECT_TRUE(s.text(r));
  // The same rect at the right edge, next to the padding.
  Img t(64, 48, 64 * 4 + 40, 0);
  t.fill_grey(0, 0, 64, 48, 240);
  EXPECT_FALSE(t.text({48, 32, 16, 16}));
  Glyph(t, 48, 32, 20, 20, 20);
  EXPECT_TRUE(t.text({48, 32, 16, 16}));
}
