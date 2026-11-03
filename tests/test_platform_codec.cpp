// Encodes synthetic frames with this platform's HEVC encoder and decodes them
// with this platform's decoder. On Linux this exercises FFmpeg (x265 or
// NVENC); on macOS, VideoToolbox.

#include <gtest/gtest.h>

#include <cmath>
#include <cstdio>

#include "capture/synthetic_capturer.h"
#include "platform.h"

using namespace rdacore;

namespace {

double Psnr(const tile::ImageView& a, const std::vector<uint8_t>& b) {
  double se = 0;
  for (int32_t y = 0; y < a.height; ++y) {
    const uint8_t* pa = a.data + static_cast<size_t>(y) * a.stride;
    const uint8_t* pb = b.data() + static_cast<size_t>(y) * a.width * 4;
    for (int32_t x = 0; x < a.width * 4; ++x) {
      if ((x & 3) == 3) continue;
      const double d = static_cast<double>(pa[x]) - pb[x];
      se += d * d;
    }
  }
  const double mse = se / (static_cast<double>(a.width) * a.height * 3);
  return mse == 0 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
}

}  // namespace

TEST(PlatformCodec, EncodeDecodeRoundTrip) {
  constexpr int W = 640, H = 360, kFrames = 30;
  codec::EncoderConfig cfg{W, H, 30, 4'000'000};
  std::string err;
  auto enc = platform::CreateHevcEncoder(cfg, "", &err);
  ASSERT_TRUE(enc) << err;
  auto dec = platform::CreateHevcDecoder(&err);
  ASSERT_TRUE(dec) << err;

  std::vector<uint8_t> px;
  int decoded = 0;
  double min_psnr = 1e9;
  for (int i = 0; i < kFrames; ++i) {
    capture::RenderSyntheticFrame(static_cast<uint64_t>(i), W, H, px);
    auto au = enc->Encode({px.data(), W, H, W * 4}, static_cast<uint64_t>(i) * 33'333, i == 0);
    ASSERT_TRUE(au) << "frame " << i << ": " << enc->last_error();
    if (i == 0) {
      EXPECT_TRUE(au->keyframe);
      EXPECT_EQ(au->parameter_sets.size(), 3u) << "expected VPS, SPS, PPS";
    } else {
      EXPECT_FALSE(au->keyframe) << "unexpected keyframe at " << i;
    }
    ASSERT_TRUE(dec->Decode(au->parameter_sets, au->nal_data, 0,
                            [&](const tile::ImageView& img, uint64_t) {
                              ASSERT_EQ(img.width, W);
                              ASSERT_EQ(img.height, H);
                              min_psnr = std::min(min_psnr, Psnr(img, px));
                              ++decoded;
                            }))
        << dec->last_error();
  }
  EXPECT_EQ(decoded, kFrames) << "low-latency decode should emit one picture per input";
  EXPECT_GT(min_psnr, 30.0);

  // A forced keyframe mid-stream carries parameter sets again.
  capture::RenderSyntheticFrame(kFrames, W, H, px);
  auto key = enc->Encode({px.data(), W, H, W * 4}, kFrames * 33'333ull, true);
  ASSERT_TRUE(key);
  EXPECT_TRUE(key->keyframe);
  EXPECT_EQ(key->parameter_sets.size(), 3u);
}

TEST(PlatformCursor, ReadsTheSystemCursor) {
  std::string err;
  auto src = platform::CreateCursorSource(&err);
  if (!src) GTEST_SKIP() << "no cursor source: " << err;  // e.g. no X display
  capture::CursorSample s;
  bool any_image = false;
  for (int i = 0; i < 8; ++i) {
    ASSERT_TRUE(src->Poll(s));
    EXPECT_GT(s.screen_width, 0);
    EXPECT_GT(s.screen_height, 0);
    if (s.image_changed) {
      any_image = true;
      EXPECT_GT(s.image.width, 0);
      EXPECT_LE(s.image.width, 256);
      EXPECT_EQ(s.image.bgra.size(), static_cast<size_t>(s.image.width) * s.image.height * 4);
      EXPECT_GE(s.image.hotspot_x, 0);
      EXPECT_LT(s.image.hotspot_x, s.image.width);
    }
  }
  std::printf("cursor at %.0f,%.0f on %.0fx%.0f, image %s\n", s.x, s.y, s.screen_width,
              s.screen_height, any_image ? "read" : "not available");
}
