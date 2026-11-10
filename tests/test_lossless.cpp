#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>

#include "codec/lossless.h"

using namespace rdacore;

namespace {

uint32_t Mix(uint32_t x) {
  x ^= x >> 16;
  x *= 0x7feb352dU;
  x ^= x >> 15;
  x *= 0x846ca68bU;
  return x ^ (x >> 16);
}

// A text-like tile: light background, dark strokes, one grey AA level, laid
// out irregularly so deflate cannot just exploit a short period.
std::vector<uint8_t> TextTile(int w, int h, int seed) {
  std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      uint8_t* p = &px[(static_cast<size_t>(y) * w + x) * 4];
      const uint32_t r = Mix(static_cast<uint32_t>((y * w + x) * 31 + seed)) % 16;
      const int v = r == 0 ? 30 : (r == 1 ? 140 : 245);
      p[0] = static_cast<uint8_t>(v);
      p[1] = static_cast<uint8_t>(v);
      p[2] = static_cast<uint8_t>(v + (v == 245 ? 3 : 0));
      p[3] = 255;
    }
  }
  return px;
}

}  // namespace

TEST(Lossless, PaletteRoundTripIsExact) {
  constexpr int W = 64, H = 48;
  auto px = TextTile(W, H, 3);
  px[0] = 7;  // one stray pixel: 4 colours in total
  const ImageView img{px.data(), W, H, W * 4};
  codec::PaletteTile t;
  ASSERT_TRUE(codec::ExtractPalette(img, {0, 0, W, H}, 64, t));
  ASSERT_EQ(t.colors.size(), 4u);
  EXPECT_EQ(t.colors[0], 0x00f8f5f5u);  // background is the most frequent colour

  std::vector<uint8_t> out(px.size(), 0);
  codec::PaintPalette(t, out.data(), W * 4);
  EXPECT_EQ(out, px);
}

TEST(Lossless, TooManyColoursIsRejected) {
  constexpr int W = 64, H = 64;
  std::vector<uint8_t> px(static_cast<size_t>(W) * H * 4);
  for (size_t i = 0; i < px.size(); ++i) px[i] = static_cast<uint8_t>(Mix(static_cast<uint32_t>(i)));
  const ImageView img{px.data(), W, H, W * 4};
  codec::PaletteTile t;
  EXPECT_FALSE(codec::ExtractPalette(img, {0, 0, W, H}, 64, t));
  EXPECT_TRUE(codec::ExtractPalette(img, {0, 0, 2, 2}, 64, t));  // a sub-rect has few
}

TEST(Lossless, StreamAcrossFramesAndReset) {
  constexpr int W = 128, H = 64;
  codec::LosslessEncoder enc;
  codec::LosslessDecoder dec;
  size_t first_size = 0, repeat_size = 0;
  for (int frame = 0; frame < 3; ++frame) {
    auto px = TextTile(W, H, frame == 2 ? 0 : 1);
    const ImageView img{px.data(), W, H, W * 4};
    codec::RegionOps ops;
    ops.tiles.resize(2);
    ASSERT_TRUE(codec::ExtractPalette(img, {0, 0, 64, 64}, 64, ops.tiles[0]));
    ASSERT_TRUE(codec::ExtractPalette(img, {64, 0, 64, 64}, 64, ops.tiles[1]));
    const bool reset = frame == 0 || frame == 2;
    const auto data = enc.Encode(ops, reset);
    if (frame == 0) first_size = data.size();
    if (frame == 1) repeat_size = data.size();

    codec::RegionOps got;
    std::string err;
    ASSERT_TRUE(dec.Decode(data, reset, W, H, 16, got, &err)) << err;
    ASSERT_EQ(got.tiles.size(), 2u);
    std::vector<uint8_t> out(px.size(), 0);
    for (const auto& t : got.tiles) codec::PaintPalette(t, out.data(), W * 4);
    EXPECT_EQ(out, px) << "frame " << frame;
  }
  // The second frame repeats the first, so the shared history makes it tiny.
  EXPECT_LT(repeat_size * 4, first_size);
}

TEST(Lossless, CorruptOrOutOfBoundsInputIsRejected) {
  codec::LosslessEncoder enc;
  auto px = TextTile(64, 64, 0);
  codec::RegionOps ops;
  ops.tiles.resize(1);
  ASSERT_TRUE(codec::ExtractPalette({px.data(), 64, 64, 256}, {0, 0, 64, 64}, 64, ops.tiles[0]));
  const auto data = enc.Encode(ops, true);

  codec::RegionOps got;
  codec::LosslessDecoder small;
  EXPECT_FALSE(small.Decode(data, true, 32, 32, 16, got));  // tile does not fit the screen

  codec::LosslessDecoder dec;
  std::vector<uint8_t> junk = data;
  for (size_t i = 2; i < junk.size(); i += 3) junk[i] ^= 0x5a;
  EXPECT_FALSE(dec.Decode(junk, true, 64, 64, 16, got));
  // After an error only a reset frame is accepted again.
  EXPECT_FALSE(dec.Decode(data, false, 64, 64, 16, got));
  EXPECT_TRUE(dec.Decode(data, true, 64, 64, 16, got));
}

TEST(Lossless, RegionCommandsRoundTripAndAreChecked) {
  codec::RegionOps ops;
  ops.stores = {{3, {64, 0, 64, 64}}};
  ops.moves = {{{0, 10, 200, 50}, 0, -3}, {{10, 70, 30, 30}, 5, 2}};
  ops.loads = {{3, {70, 0, 4, 20}, 6, 0}};
  codec::LosslessEncoder enc;
  const auto data = enc.Encode(ops, true);
  codec::LosslessDecoder dec;
  codec::RegionOps got;
  std::string err;
  ASSERT_TRUE(dec.Decode(data, true, 256, 128, 16, got, &err)) << err;
  EXPECT_EQ(got.stores, ops.stores);
  EXPECT_EQ(got.moves, ops.moves);
  EXPECT_EQ(got.loads, ops.loads);

  // A slot beyond the cache, or a move reading outside the screen.
  codec::LosslessDecoder d2;
  EXPECT_FALSE(d2.Decode(data, true, 256, 128, 2, got));
  codec::RegionOps bad;
  bad.moves = {{{0, 0, 10, 10}, 1, 0}};
  codec::LosslessEncoder e3;
  codec::LosslessDecoder d3;
  EXPECT_FALSE(d3.Decode(e3.Encode(bad, true), true, 256, 128, 16, got));
}

// ---- reduced palettes (text mode) -----------------------------------------

namespace {

// Dark text on a light background with `shades` levels of anti-aliasing
// between them; with `gradient`, the background drifts across the tile.
std::vector<uint8_t> SmoothText(int w, int h, int shades, bool gradient) {
  std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      uint8_t* p = &px[(static_cast<size_t>(y) * w + x) * 4];
      const uint32_t r = Mix(static_cast<uint32_t>(y * w + x)) % 100;
      int bg[3] = {244, 244, 247};
      if (gradient) {
        bg[0] = 228 + x / 4;  // 16 levels each way, as under a translucent panel
        bg[1] = 228 + y / 4;
      }
      // 70% background, 10% text, the rest somewhere in between.
      const int level = r < 70 ? 0 : r < 80 ? shades : 1 + static_cast<int>(Mix(r * 977 + x) % (shades - 1));
      for (int c = 0; c < 3; ++c) p[c] = static_cast<uint8_t>(bg[c] + (30 - bg[c]) * level / shades);
      p[3] = 255;
    }
  }
  return px;
}

}  // namespace

TEST(Lossless, ReducedPaletteKeepsBackgroundTextAndEdges) {
  constexpr int W = 64, H = 64;
  const auto px = SmoothText(W, H, 60, false);
  const ImageView img{px.data(), W, H, W * 4};
  codec::PaletteTile exact, t;
  ASSERT_TRUE(codec::ExtractPalette(img, {0, 0, W, H}, 256, exact));
  ASSERT_GT(exact.colors.size(), 40u);
  bool reduced = false;
  ASSERT_TRUE(codec::ExtractReducedPalette(img, {0, 0, W, H}, 16, 256, 4096, t, &reduced));
  EXPECT_TRUE(reduced);
  ASSERT_EQ(t.colors.size(), 16u);
  EXPECT_EQ(t.colors[0], exact.colors[0]);  // the background as it is
  std::vector<uint8_t> out(px.size(), 0);
  codec::PaintPalette(t, out.data(), W * 4);
  int worst = 0;
  for (size_t i = 0; i < px.size(); i += 4) {
    const bool plain = px[i] == 244 || px[i] == 30;  // background or full text colour
    for (int c = 0; c < 3; ++c) {
      const int d = std::abs(static_cast<int>(px[i + c]) - static_cast<int>(out[i + c]));
      worst = std::max(worst, d);
      if (plain) ASSERT_EQ(d, 0) << "background or text colour changed at " << i / 4;
    }
  }
  // 214 levels between text and background over 16 colours: steps of ~14.
  EXPECT_LE(worst, 12);
  // Every colour is one of the tile's own.
  for (uint32_t c : t.colors) {
    EXPECT_NE(std::find(exact.colors.begin(), exact.colors.end(), c), exact.colors.end());
  }
}

TEST(Lossless, ReducedPaletteLeavesFewColoursExact) {
  constexpr int W = 64, H = 48;
  const auto px = TextTile(W, H, 3);
  const ImageView img{px.data(), W, H, W * 4};
  codec::PaletteTile t;
  bool reduced = true;
  ASSERT_TRUE(codec::ExtractReducedPalette(img, {0, 0, W, H}, 16, 256, 4096, t, &reduced));
  EXPECT_FALSE(reduced);
  std::vector<uint8_t> out(px.size(), 0);
  codec::PaintPalette(t, out.data(), W * 4);
  EXPECT_EQ(out, px);
}

TEST(Lossless, ReducedPaletteTakesTextOnAGradientButNotAPhoto) {
  constexpr int W = 64, H = 64;
  // Text on a soft gradient: far more than 256 colours, video coded (and
  // blurred) without the reduction.
  const auto px = SmoothText(W, H, 8, true);
  const ImageView img{px.data(), W, H, W * 4};
  codec::PaletteTile t;
  EXPECT_FALSE(codec::ExtractPalette(img, {0, 0, W, H}, 256, t));
  bool reduced = false;
  ASSERT_TRUE(codec::ExtractReducedPalette(img, {0, 0, W, H}, 16, 256, 4096, t, &reduced));
  EXPECT_TRUE(reduced);
  EXPECT_LE(t.colors.size(), 16u);
  // ... unless told not to look beyond the exact limit.
  EXPECT_FALSE(codec::ExtractReducedPalette(img, {0, 0, W, H}, 16, 256, 256, t));

  // A photo (noise here) is left to the video encoder.
  std::vector<uint8_t> photo(static_cast<size_t>(W) * H * 4);
  for (size_t i = 0; i < photo.size(); ++i) photo[i] = static_cast<uint8_t>(Mix(static_cast<uint32_t>(i)));
  EXPECT_FALSE(codec::ExtractReducedPalette({photo.data(), W, H, W * 4}, {0, 0, W, H}, 16, 256, 4096, t));
  // So is a smooth picture with too much range for 16 colours.
  for (int y = 0; y < H; ++y) {
    for (int x = 0; x < W; ++x) {
      uint8_t* p = &photo[(static_cast<size_t>(y) * W + x) * 4];
      p[0] = static_cast<uint8_t>(x * 4);
      p[1] = static_cast<uint8_t>(y * 4);
      p[2] = static_cast<uint8_t>((x + y) * 2);
    }
  }
  EXPECT_FALSE(codec::ExtractReducedPalette({photo.data(), W, H, W * 4}, {0, 0, W, H}, 16, 256, 4096, t));
}
