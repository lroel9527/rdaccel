#include "codec/codec_choice.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>

namespace rdacore::codec {

namespace {

std::string Fps(double fps) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.0f fps", fps);
  return buf;
}

std::string Size(int32_t w, int32_t h) { return std::to_string(w) + "x" + std::to_string(h); }

}  // namespace

CodecChoice ChooseVideoCodec(CodecPreference pref, bool viewer_decodes_h264, int32_t w, int32_t h,
                             const HardwareFpsFn& measure) {
  CodecChoice c;
  if (pref == CodecPreference::kHevc) {
    c.reason = "HEVC as configured";
    return c;
  }
  if (!viewer_decodes_h264) {
    if (pref == CodecPreference::kH264) {
      c.warn = true;
      c.reason = "H.264 configured, but the viewer does not decode it: HEVC";
    } else {
      c.reason = "HEVC (the viewer does not decode H.264)";
    }
    return c;
  }
  if (pref == CodecPreference::kH264) {
    c.codec = VideoCodec::kH264;
    c.reason = "H.264 as configured";
    return c;
  }
  c.measured = true;
  c.hevc_fps = measure ? measure(VideoCodec::kHevc, w, h) : 0;
  if (c.hevc_fps >= kMinUsableFps) {
    c.reason = "HEVC (hardware encodes " + Size(w, h) + " at " + Fps(c.hevc_fps) + ")";
    return c;
  }
  c.h264_fps = measure ? measure(VideoCodec::kH264, w, h) : 0;
  const std::string hevc = c.hevc_fps > 0 ? "hardware HEVC encodes " + Size(w, h) + " at only " + Fps(c.hevc_fps)
                                          : "no hardware HEVC encoder for " + Size(w, h);
  if (c.h264_fps > c.hevc_fps) {
    c.codec = VideoCodec::kH264;
    c.warn = true;
    c.reason = "H.264: " + hevc + ", hardware H.264 at " + Fps(c.h264_fps);
  } else {
    c.reason = "HEVC: " + hevc + ", and hardware H.264 is " +
               (c.h264_fps > 0 ? "no faster (" + Fps(c.h264_fps) + ")" : std::string("not available"));
  }
  return c;
}

double MeasureEncodeFps(IHevcEncoder& enc, int32_t w, int32_t h, int frames, double stop_below_fps) {
  if (w <= 0 || h <= 0 || frames <= 0) return 0;
  // A light background with a block of detail (a third of each side) that
  // moves and changes every frame, so each picture costs the encoder real work (a still picture
  // would be nearly free and flatter the encoder).
  const int32_t stride = w * 4;
  std::vector<uint8_t> px(static_cast<size_t>(stride) * h, 0xe0);
  const int32_t bw = std::max(16, w / 3), bh = std::max(16, h / 3);
  auto paint = [&](int i) {
    const int32_t x0 = (i * 37) % std::max(1, w - bw);
    const int32_t y0 = (i * 23) % std::max(1, h - bh);
    for (int32_t y = y0; y < y0 + bh && y < h; ++y) {
      uint8_t* row = px.data() + static_cast<size_t>(y) * stride;
      for (int32_t x = x0; x < x0 + bw && x < w; ++x) {
        const uint32_t v = static_cast<uint32_t>((x + i * 7) * 31 ^ (y - i * 5) * 17);
        row[x * 4 + 0] = static_cast<uint8_t>(v);
        row[x * 4 + 1] = static_cast<uint8_t>(v >> 3);
        row[x * 4 + 2] = static_cast<uint8_t>(v >> 6);
      }
    }
  };
  using Clock = std::chrono::steady_clock;
  const tile::ImageView view{px.data(), w, h, stride};
  paint(0);
  // Timestamps from 1 s: the encoder may have been tried with a picture at 0
  // already (VideoToolbox wants them increasing).
  constexpr uint64_t kStartUs = 1'000'000;
  if (!enc.Encode(view, kStartUs, true)) return 0;  // warm-up keyframe, not timed
  const double budget_s = stop_below_fps > 0 ? frames / stop_below_fps : 1e9;
  double spent_s = 0;
  int done = 0;
  for (int i = 1; i <= frames; ++i) {
    paint(i);  // not timed
    const auto t0 = Clock::now();
    auto au = enc.Encode(view, kStartUs + static_cast<uint64_t>(i) * 16'667, false);
    spent_s += std::chrono::duration<double>(Clock::now() - t0).count();
    if (!au && !enc.last_error().empty()) return 0;
    ++done;
    if (spent_s > budget_s) break;  // cannot reach stop_below_fps any more
  }
  return spent_s > 0 ? done / spent_s : 1e6;
}

}  // namespace rdacore::codec
