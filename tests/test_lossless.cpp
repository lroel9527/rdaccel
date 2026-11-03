#include <gtest/gtest.h>

#include "codec/lossless.h"

using namespace rdaccel;

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
