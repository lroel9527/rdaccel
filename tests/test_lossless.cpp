#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>

#include "codec/context_tiles.h"
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

TEST(Lossless, FaithfulReductionTakesMoreColoursForTextInSeveralColours) {
  // Highlighted code on a dark background: five text colours, each with its
  // own shades towards the background. 16 colours leave visible steps; the
  // tile must not be given up on (it would be video coded) but get more.
  constexpr int W = 64, H = 64;
  std::vector<uint8_t> px(static_cast<size_t>(W) * H * 4);
  const int bg[3] = {40, 33, 30};
  const int fg[5][3] = {{225, 220, 217}, {163, 94, 252}, {240, 217, 102}, {51, 150, 252}, {102, 204, 128}};
  for (int y = 0; y < H; ++y) {
    for (int x = 0; x < W; ++x) {
      uint8_t* p = &px[(static_cast<size_t>(y) * W + x) * 4];
      const uint32_t r = Mix(static_cast<uint32_t>(y * W + x) + 99);
      const int* f = fg[(x / 13 + y / 16) % 5];
      const int level = r % 100 < 72 ? 0 : 1 + static_cast<int>((r >> 8) % 24);  // of 24
      for (int c = 0; c < 3; ++c) p[c] = static_cast<uint8_t>(bg[c] + (f[c] - bg[c]) * level / 24);
      p[3] = 255;
    }
  }
  const ImageView img{px.data(), W, H, W * 4};
  codec::PaletteTile t;
  // The quick way reduces it whatever it looks like; the faithful one takes more.
  ASSERT_TRUE(codec::ExtractReducedPalette(img, {0, 0, W, H}, 16, 256, 4096, t));
  EXPECT_EQ(t.colors.size(), 16u);
  bool reduced = false;
  ASSERT_TRUE(codec::ExtractReducedPalette(img, {0, 0, W, H}, 16, 256, 4096, t, &reduced, true));
  EXPECT_GT(t.colors.size(), 16u);
  EXPECT_LE(t.colors.size(), 64u);
  std::vector<uint8_t> out(px.size(), 0);
  codec::PaintPalette(t, out.data(), W * 4);
  size_t far = 0;
  for (size_t i = 0; i < px.size(); i += 4) {
    int d2 = 0;
    for (int c = 0; c < 3; ++c) d2 += (px[i + c] - out[i + c]) * (px[i + c] - out[i + c]);
    far += d2 > 24 * 24;
  }
  EXPECT_LE(far * 50, static_cast<size_t>(W) * H);
  // A photo is still not text.
  std::vector<uint8_t> photo(px.size());
  for (size_t i = 0; i < photo.size(); ++i) photo[i] = static_cast<uint8_t>(Mix(static_cast<uint32_t>(i)));
  EXPECT_FALSE(codec::ExtractReducedPalette({photo.data(), W, H, W * 4}, {0, 0, W, H}, 16, 256, 4096, t, nullptr, true));
}

// ---- context tiles ---------------------------------------------------------

TEST(Lossless, ContextTilesRoundTripAcrossFramesAndResets) {
  constexpr int W = 256, H = 64;
  codec::LosslessEncoder enc;
  codec::LosslessDecoder dec;
  size_t first_size = 0, later_size = 0;
  for (int frame = 0; frame < 6; ++frame) {
    const bool reset = frame == 0 || frame == 4;
    // Four tiles of anti-aliased text, reduced to 16, 16, 4 and 2 colours,
    // plus an ordinary tile and a sliver.
    std::vector<uint8_t> px(static_cast<size_t>(W) * H * 4);
    const auto a = SmoothText(W, H, 40 + frame, false);
    px = a;
    const ImageView img{px.data(), W, H, W * 4};
    codec::RegionOps ops;
    const int limits[4] = {16, 16, 4, 2};
    for (int k = 0; k < 4; ++k) {
      codec::PaletteTile t;
      const Rect r = k == 3 ? Rect{192, 0, 37, 5} : Rect{k * 64, 0, 64, 64};
      ASSERT_TRUE(codec::ExtractReducedPalette(img, r, limits[k], 256, 4096, t));
      ops.context_tiles.push_back(std::move(t));
    }
    codec::PaletteTile plain;
    ASSERT_TRUE(codec::ExtractPalette(img, {192, 16, 64, 48}, 256, plain));
    ops.tiles.push_back(plain);
    std::vector<uint8_t> context;
    const auto data = enc.Encode(ops, reset, &context);
    ASSERT_FALSE(context.empty());
    (frame == 0 ? first_size : later_size) = context.size();

    codec::RegionOps got;
    std::string err;
    ASSERT_TRUE(dec.Decode(data, reset, W, H, 16, got, &err, context)) << err;
    ASSERT_EQ(got.tiles.size(), 5u);
    // What each tile paints is what the encoder was given.
    std::vector<uint8_t> want(px.size(), 0), have(px.size(), 0);
    codec::PaintPalette(plain, want.data(), W * 4);
    for (const auto& t : ops.context_tiles) codec::PaintPalette(t, want.data(), W * 4);
    for (const auto& t : got.tiles) codec::PaintPalette(t, have.data(), W * 4);
    ASSERT_EQ(want, have) << "frame " << frame;
  }
  // The statistics carry over: the same kind of content costs less later.
  EXPECT_LT(later_size, first_size);
}

namespace {

// Highlighted code on a dark background: five text colours, each with its
// own shades towards the background.
std::vector<uint8_t> ColouredCode(int w, int h, int seed) {
  std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4);
  const int bg[3] = {40, 33, 30};
  const int fg[5][3] = {{225, 220, 217}, {163, 94, 252}, {240, 217, 102}, {51, 150, 252}, {102, 204, 128}};
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      uint8_t* p = &px[(static_cast<size_t>(y) * w + x) * 4];
      const uint32_t r = Mix(static_cast<uint32_t>(y * w + x) + static_cast<uint32_t>(seed) * 7919u);
      const int* f = fg[(x / 13 + y / 16 + seed) % 5];
      // Runs of background, strokes with soft edges.
      const bool stroke = ((x + seed) % 9 < 3) && (y % 16 < 11) && r % 8 != 0;
      const int level = !stroke ? 0 : ((x + seed) % 9 == 1 ? 24 : 6 + static_cast<int>((r >> 8) % 14));
      for (int c = 0; c < 3; ++c) p[c] = static_cast<uint8_t>(bg[c] + (f[c] - bg[c]) * level / 24);
      p[3] = 255;
    }
  }
  return px;
}

}  // namespace

TEST(Lossless, ContextTilesOfUpTo64ColoursRoundTripAndBeatDeflate) {
  constexpr int W = 256, H = 64;
  codec::LosslessEncoder enc, plain_enc;
  codec::LosslessDecoder dec;
  size_t context_total = 0, deflate_total = 0;
  int over16 = 0, over32 = 0;
  for (int frame = 0; frame < 8; ++frame) {
    const auto px = ColouredCode(W, H, frame);
    const ImageView img{px.data(), W, H, W * 4};
    codec::RegionOps ops, plain;
    for (int k = 0; k < 4; ++k) {
      codec::PaletteTile t;
      // Up to 32, 64, 64 colours, and one small odd-sized rect.
      const Rect r = k == 3 ? Rect{192, 3, 41, 7} : Rect{k * 64, 0, 64, 64};
      ASSERT_TRUE(codec::ExtractReducedPalette(img, r, k == 0 ? 32 : 64, 256, 4096, t));
      over16 += t.colors.size() > 16;
      over32 += t.colors.size() > 32;
      plain.tiles.push_back(t);
      ops.context_tiles.push_back(std::move(t));
    }
    std::vector<uint8_t> context;
    const auto data = enc.Encode(ops, frame == 0 || frame == 5, &context);
    context_total += data.size() + context.size();
    deflate_total += plain_enc.Encode(plain, frame == 0 || frame == 5).size();
    codec::RegionOps got;
    std::string err;
    ASSERT_TRUE(dec.Decode(data, frame == 0 || frame == 5, W, H, 16, got, &err, context)) << err;
    ASSERT_EQ(got.tiles.size(), 4u);
    std::vector<uint8_t> want(px.size(), 0), have(px.size(), 0);
    for (const auto& t : ops.context_tiles) codec::PaintPalette(t, want.data(), W * 4);
    for (const auto& t : got.tiles) codec::PaintPalette(t, have.data(), W * 4);
    ASSERT_EQ(want, have) << "frame " << frame;
    // Damage: an error or indices inside the palette, never a crash.
    for (uint32_t seed = 1; seed <= 20; ++seed) {
      auto bad = context;
      bad[Mix(seed + static_cast<uint32_t>(frame) * 100) % bad.size()] ^= static_cast<uint8_t>(1 + Mix(seed) % 255);
      codec::LosslessDecoder d2;
      codec::RegionOps g2;
      if (d2.Decode(data, true, W, H, 16, g2, nullptr, bad)) {
        for (const auto& t : g2.tiles) {
          for (uint8_t i : t.indices) ASSERT_LT(i, t.colors.size());
        }
      }
    }
  }
  EXPECT_GT(over16, 8);  // the test is about tiles beyond 16 colours
  EXPECT_GT(over32, 0);
  EXPECT_LT(context_total * 10, deflate_total * 9) << context_total << " bytes with the context model, " << deflate_total << " with deflate";
}

TEST(Lossless, ContextTilesBeatDeflateOnAntiAliasedText) {
  constexpr int W = 64, H = 64;
  size_t deflate = 0, context = 0;
  codec::LosslessEncoder a, b;
  for (int k = 0; k < 24; ++k) {
    const auto px = SmoothText(W, H, 30 + k, false);
    codec::RegionOps plain, ctx;
    codec::PaletteTile t;
    ASSERT_TRUE(codec::ExtractReducedPalette({px.data(), W, H, W * 4}, {0, 0, W, H}, 16, 256, 4096, t));
    plain.tiles.push_back(t);
    ctx.context_tiles.push_back(t);
    deflate += a.Encode(plain, k == 0).size();
    std::vector<uint8_t> block;
    context += b.Encode(ctx, k == 0, &block).size() + block.size();
  }
  EXPECT_LT(context, deflate);
}

TEST(Lossless, ContextTileStreamsWithoutThemAreUnchanged) {
  // No context tiles: byte for byte what an older encoder wrote, so older
  // viewers and recordings are unaffected.
  const auto px = TextTile(64, 64, 5);
  codec::RegionOps ops;
  ops.tiles.resize(1);
  ASSERT_TRUE(codec::ExtractPalette({px.data(), 64, 64, 256}, {0, 0, 64, 64}, 64, ops.tiles[0]));
  codec::LosslessEncoder a, b;
  std::vector<uint8_t> context;
  EXPECT_EQ(a.Encode(ops, true), b.Encode(ops, true, &context));
  EXPECT_TRUE(context.empty());
}

TEST(Lossless, CorruptContextTileDataIsRejectedOrHarmless) {
  constexpr int W = 128, H = 64;
  const auto px = SmoothText(W, H, 50, false);
  codec::RegionOps ops;
  for (int k = 0; k < 2; ++k) {
    codec::PaletteTile t;
    ASSERT_TRUE(codec::ExtractReducedPalette({px.data(), W, H, W * 4}, {k * 64, 0, 64, 64}, 16, 256, 4096, t));
    ops.context_tiles.push_back(std::move(t));
  }
  codec::LosslessEncoder enc;
  std::vector<uint8_t> context;
  const auto data = enc.Encode(ops, true, &context);
  {
    // The block missing or cut short: an error, not a read past the end.
    codec::LosslessDecoder dec;
    codec::RegionOps got;
    EXPECT_FALSE(dec.Decode(data, true, W, H, 16, got));
    codec::LosslessDecoder dec2;
    EXPECT_FALSE(dec2.Decode(data, true, W, H, 16, got, nullptr,
                             std::span<const uint8_t>(context).first(context.size() / 3)));
  }
  // Any damage: indices stay inside the palette whatever comes out.
  for (uint32_t seed = 1; seed <= 200; ++seed) {
    auto bad = context;
    bad[Mix(seed) % bad.size()] ^= static_cast<uint8_t>(1 + Mix(seed * 31) % 255);
    codec::LosslessDecoder dec;
    codec::RegionOps got;
    if (dec.Decode(data, true, W, H, 16, got, nullptr, bad)) {
      for (const auto& t : got.tiles) {
        for (uint8_t i : t.indices) ASSERT_LT(i, t.colors.size());
      }
    }
  }
  // A screen smaller than the tiles' rects is refused before any decoding.
  codec::LosslessDecoder dec;
  codec::RegionOps got;
  EXPECT_FALSE(dec.Decode(data, true, 64, 64, 16, got, nullptr, context));
}
