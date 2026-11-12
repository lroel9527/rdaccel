// HostSession -> LocalPipe -> ViewerSession with a fake codec, so the frame
// pipeline (tile skip, regions, reassembly, input path) is tested without any
// real encoder.

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>

#include "capture/synthetic_capturer.h"
#include "session/host_session.h"
#include "session/viewer_session.h"
#include "transport/local_pipe.h"

using namespace rdacore;

namespace {

// "Encodes" a frame as its raw pixels (first row only, to stay small) inside
// a fake slice NAL; the decoder expands it back.
class FakeEncoder final : public codec::IHevcEncoder {
 public:
  bool fail_next = false;
  bool drop_next = false;  // drop the picture, as VideoToolbox may (no error)
  std::vector<bool> keyframe_requests;
  std::vector<int> min_qp;  // per picture: the floor asked for (0 = the encoder's own)
  size_t pad = 0;           // extra bytes per picture (a real one is kilobytes)
  size_t* pictures = nullptr;  // counts the pictures produced, if set
  int64_t* bitrate = nullptr;  // the last bitrate set, if set
  std::vector<uint8_t>* input = nullptr;  // the last picture's input (BGRA, tightly packed), if set

  std::string name() const override { return "fake"; }
  void SetMinQp(int qp) override { min_qp_now_ = qp; }
  const std::string& last_error() const override { return err_; }
  void SetBitrate(int64_t bps) override {
    if (bitrate) *bitrate = bps;
  }
  std::optional<codec::HevcAccessUnit> Encode(const tile::ImageView& img, uint64_t,
                                              bool force_keyframe) override {
    err_.clear();
    if (input) {
      input->resize(static_cast<size_t>(img.width) * img.height * 4);
      for (int y = 0; y < img.height; ++y) {
        std::memcpy(input->data() + static_cast<size_t>(y) * img.width * 4,
                    img.data + static_cast<size_t>(y) * img.stride, static_cast<size_t>(img.width) * 4);
      }
    }
    keyframe_requests.push_back(force_keyframe);
    min_qp.push_back(min_qp_now_);
    if (fail_next) {
      fail_next = false;
      err_ = "simulated encoder failure";
      return std::nullopt;
    }
    if (drop_next) {
      drop_next = false;
      return std::nullopt;
    }
    codec::HevcAccessUnit au;
    au.keyframe = force_keyframe;
    if (force_keyframe) au.parameter_sets = {{codec::kNalVps << 1, 1}};
    std::vector<uint8_t> nal = {static_cast<uint8_t>((force_keyframe ? 19 : 1) << 1), 1};
    nal.push_back(static_cast<uint8_t>(img.width & 0xff));
    nal.push_back(static_cast<uint8_t>(img.height & 0xff));
    nal.resize(nal.size() + pad, 0x55);
    if (pictures) ++*pictures;
    codec::AppendLengthPrefixed(nal, au.nal_data);
    return au;
  }

 private:
  std::string err_;
  int min_qp_now_ = 0;
};

class FakeDecoder final : public codec::IHevcDecoder {
 public:
  bool fail_next = false;
  std::string name() const override { return "fake"; }
  const std::string& last_error() const override { return err_; }
  bool Decode(const std::vector<std::vector<uint8_t>>& ps, const std::vector<uint8_t>& nal_data,
              uint64_t pts, const codec::DecodedFrameFn& on_frame) override {
    if (fail_next) {
      fail_next = false;
      return false;
    }
    if (!ps.empty()) have_ps_ = true;
    if (!have_ps_) return false;  // like a real decoder: no picture before a keyframe
    std::vector<std::span<const uint8_t>> nals;
    if (!codec::SplitLengthPrefixed(nal_data, nals) || nals.empty()) return false;
    const int w = nals[0][2], h = nals[0][3];
    px_.assign(static_cast<size_t>(w) * h * 4, 0);
    on_frame({px_.data(), w, h, w * 4}, pts);
    return true;
  }

 private:
  bool have_ps_ = false;
  std::vector<uint8_t> px_;
  std::string err_;
};

// Stops both pipe workers before the test's locals they call into go away.
struct PipeStopper {
  std::unique_ptr<transport::LocalPipeEndpoint>& a;
  std::unique_ptr<transport::LocalPipeEndpoint>& b;
  ~PipeStopper() {
    a.reset();
    b.reset();
  }
};

}  // namespace

TEST(Session, FramesFlowAndStaticFramesAreSkipped) {
  auto [host_end, viewer_end] = transport::MakeLocalPipe();
  constexpr int W = 200, H = 120;
  session::HostSession host(host_end.get(), std::make_unique<FakeEncoder>(), W, H,
                            {.lossless = false, .refine_frames = 0});
  // Attached only once the handlers are set: the Hello is already waiting
  // in the pipe and would otherwise race the handler.
  session::ViewerSession viewer(nullptr, std::make_unique<FakeDecoder>());

  std::mutex mu;
  std::condition_variable cv;
  proto::Hello hello;
  std::vector<std::pair<uint64_t, size_t>> got;  // frame id, region count
  viewer.SetHelloHandler([&](const proto::Hello& h) {
    std::lock_guard<std::mutex> lock(mu);
    hello = h;
  });
  viewer.SetFrameHandler([&](const tile::ImageView& img, const std::vector<Rect>& regions,
                             uint64_t id, uint64_t) {
    EXPECT_EQ(img.width, W);
    EXPECT_EQ(img.height, H);
    std::lock_guard<std::mutex> lock(mu);
    got.emplace_back(id, regions.size());
    cv.notify_all();
  });

  viewer.Attach(viewer_end.get());

  std::vector<uint8_t> px(static_cast<size_t>(W) * H * 4, 50);
  capture::CapturedFrame f{{px.data(), W, H, W * 4}, {}, 0};
  host.OnFrame(f);  // keyframe
  host.OnFrame(f);  // identical -> skipped
  px[(70 * W + 150) * 4] = 99;  // one pixel in tile (2, 1)
  host.OnFrame(f);

  PipeStopper stop{host_end, viewer_end};
  std::unique_lock<std::mutex> lock(mu);
  ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(5), [&] { return got.size() == 2; }));
  EXPECT_EQ(hello.width, static_cast<uint32_t>(W));
  EXPECT_EQ(got[0], (std::pair<uint64_t, size_t>{1, 0}));  // keyframe: whole screen
  EXPECT_EQ(got[1], (std::pair<uint64_t, size_t>{2, 1}));  // one changed region

  const auto hs = host.stats();
  EXPECT_EQ(hs.frames_captured, 3u);
  EXPECT_EQ(hs.frames_skipped, 1u);
  EXPECT_EQ(hs.frames_encoded, 2u);
  EXPECT_EQ(hs.keyframes, 1u);
}

TEST(Session, ADroppedPictureIsCarriedByTheNextOneWithoutAKeyframe) {
  auto [host_end, viewer_end] = transport::MakeLocalPipe();
  auto enc = std::make_unique<FakeEncoder>();
  FakeEncoder* raw = enc.get();
  constexpr int W = 200, H = 120;
  session::HostSession host(host_end.get(), std::move(enc), W, H, {.lossless = false, .refine_frames = 0});
  session::ViewerSession viewer(nullptr, std::make_unique<FakeDecoder>());
  std::mutex mu;
  std::condition_variable cv;
  std::vector<std::vector<Rect>> got;
  viewer.SetFrameHandler([&](const tile::ImageView&, const std::vector<Rect>& regions, uint64_t, uint64_t) {
    std::lock_guard<std::mutex> lock(mu);
    got.push_back(regions);
    cv.notify_all();
  });
  viewer.Attach(viewer_end.get());

  std::vector<uint8_t> px(static_cast<size_t>(W) * H * 4, 50);
  capture::CapturedFrame f{{px.data(), W, H, W * 4}, {}, 0};
  std::string err;
  ASSERT_TRUE(host.OnFrame(f, &err));  // keyframe
  px[(10 * W + 10) * 4] = 99;          // tile (0, 0)
  raw->drop_next = true;
  ASSERT_TRUE(host.OnFrame(f, &err)) << err;  // dropped: not an error
  px[(70 * W + 150) * 4] = 99;                // tile (2, 1)
  ASSERT_TRUE(host.OnFrame(f, &err));
  EXPECT_EQ(raw->keyframe_requests, (std::vector<bool>{true, false, false}));
  EXPECT_EQ(host.stats().keyframes, 1u);
  EXPECT_EQ(host.stats().encoder_drops, 1u);

  PipeStopper stop{host_end, viewer_end};
  std::unique_lock<std::mutex> lock(mu);
  ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(5), [&] { return got.size() == 3; }));
  // The last picture shows both changes: the dropped frame's tile too.
  ASSERT_EQ(got[2].size(), 2u);
  EXPECT_EQ(got[2][0], (Rect{0, 0, 64, 64}));
  EXPECT_EQ(got[2][1], (Rect{128, 64, 64, 56}));
}

TEST(Session, EncoderFailureIsReportedAndForcesKeyframe) {
  auto [host_end, viewer_end] = transport::MakeLocalPipe();
  auto enc = std::make_unique<FakeEncoder>();
  FakeEncoder* raw = enc.get();
  constexpr int W = 128, H = 64;
  session::HostSession host(host_end.get(), std::move(enc), W, H, {.lossless = false, .refine_frames = 0});
  std::vector<uint8_t> px(static_cast<size_t>(W) * H * 4, 50);
  capture::CapturedFrame f{{px.data(), W, H, W * 4}, {}, 0};
  std::string err;
  ASSERT_TRUE(host.OnFrame(f, &err));  // keyframe
  px[0] = 1;
  raw->fail_next = true;
  EXPECT_FALSE(host.OnFrame(f, &err));
  EXPECT_EQ(err, "simulated encoder failure");
  EXPECT_EQ(host.stats().encode_errors, 1u);
  // The failed frame never reached the viewer, so the next one (even with
  // unchanged pixels) must be a full refresh.
  ASSERT_TRUE(host.OnFrame(f, &err));
  EXPECT_EQ(raw->keyframe_requests, (std::vector<bool>{true, false, true}));
  EXPECT_EQ(host.stats().frames_encoded, 2u);
}

TEST(Session, LosslessStaysWithinItsBudgetOnASlowLink) {
  // At 0.4 Mbps, many-colour (but palette-able) content appearing in every
  // frame must not go out losslessly faster than the budget refills: once a
  // frame overdrew, later frames send such tiles as video. (Per frame, it
  // was up to four times the bitrate, in 10-15 kB bursts.)
  auto [host_end, viewer_end] = transport::MakeLocalPipe();
  constexpr int W = 640, H = 384, kFps = 30;
  constexpr int64_t kBitrate = 400'000;
  session::HostSession host(host_end.get(), std::make_unique<FakeEncoder>(), W, H,
                            {.refine_frames = 0, .progressive = false, .fps = kFps,
                             .bitrate_bps = kBitrate});
  std::vector<uint8_t> px(static_cast<size_t>(W) * H * 4, 50);
  uint32_t seed = 1;
  auto next = [&] { return (seed = seed * 1103515245u + 12345u) >> 16; };
  uint64_t t = 1'000'000;
  host.OnFrame({{px.data(), W, H, W * 4}, {}, t});  // keyframe
  const uint64_t after_key = host.stats().lossless_bytes;
  constexpr int kFrames = 60;
  for (int f = 0; f < kFrames; ++f) {
    t += 1'000'000 / kFps;
    // A third of the tiles get new 200-colour content each frame (so none
    // is hot: each changes every third frame).
    for (int ty = 0; ty < H / 64; ++ty) {
      for (int tx = 0; tx < W / 64; ++tx) {
        if ((tx + ty + f) % 3 != 0) continue;
        for (int y = 0; y < 64; ++y) {
          for (int x = 0; x < 64; ++x) {
            uint8_t* p = &px[(static_cast<size_t>(ty * 64 + y) * W + tx * 64 + x) * 4];
            const uint32_t c = next() % 200;
            p[0] = static_cast<uint8_t>(c);
            p[1] = static_cast<uint8_t>(c * 7);
            p[2] = static_cast<uint8_t>(c * 13);
          }
        }
      }
    }
    host.OnFrame({{px.data(), W, H, W * 4}, {}, t});
  }
  const double seconds = static_cast<double>(kFrames) / kFps;
  const double sent = static_cast<double>(host.stats().lossless_bytes - after_key);
  // Half the bitrate, plus what the bucket holds (three frame intervals).
  const double allowed = kBitrate / 8.0 * (0.5 * seconds + 3.0 / kFps);
  EXPECT_LE(sent, allowed * 1.1) << sent << " bytes lossless in " << seconds << " s";
  PipeStopper stop{host_end, viewer_end};
}

TEST(Session, TilesTheBudgetSentAsVideoTurnExactOnceStill) {
  // Content beyond the lossless budget goes as video; once the screen is
  // still it must be sent again losslessly (refresh is off by default, and
  // nothing else would). The fake decoder shows black, so any tile left at
  // video quality differs from the screen.
  auto [host_end, viewer_end] = transport::MakeLocalPipe();
  constexpr int W = 640, H = 384, kFps = 30;
  constexpr int64_t kBitrate = 400'000;
  session::HostSession host(host_end.get(), std::make_unique<FakeEncoder>(), W, H,
                            {.refine_frames = 0, .progressive = false, .fps = kFps,
                             .bitrate_bps = kBitrate});
  session::ViewerSession viewer(nullptr, std::make_unique<FakeDecoder>());
  std::mutex mu;
  std::condition_variable cv;
  std::vector<uint8_t> shown;
  uint64_t last_id = 0;
  viewer.SetFrameHandler([&](const tile::ImageView& img, const std::vector<Rect>&, uint64_t id,
                             uint64_t) {
    std::lock_guard<std::mutex> lock(mu);
    shown.resize(static_cast<size_t>(img.width) * img.height * 4);
    for (int y = 0; y < img.height; ++y) {
      std::memcpy(shown.data() + static_cast<size_t>(y) * img.width * 4,
                  img.data + static_cast<size_t>(y) * img.stride, static_cast<size_t>(img.width) * 4);
    }
    last_id = id;
    cv.notify_all();
  });
  viewer.Attach(viewer_end.get());

  std::vector<uint8_t> px(static_cast<size_t>(W) * H * 4, 50);
  for (size_t i = 3; i < px.size(); i += 4) px[i] = 255;
  uint32_t seed = 1;
  auto next = [&] { return (seed = seed * 1103515245u + 12345u) >> 16; };
  uint64_t t = 1'000'000;
  host.OnFrame({{px.data(), W, H, W * 4}, {}, t});  // keyframe
  for (int f = 0; f < 30; ++f) {
    t += 1'000'000 / kFps;
    for (int ty = 0; ty < H / 64; ++ty) {
      for (int tx = 0; tx < W / 64; ++tx) {
        if ((tx + ty + f) % 3 != 0) continue;
        for (int y = 0; y < 64; ++y) {
          for (int x = 0; x < 64; ++x) {
            uint8_t* p = &px[(static_cast<size_t>(ty * 64 + y) * W + tx * 64 + x) * 4];
            const uint32_t c = next() % 200;
            p[0] = static_cast<uint8_t>(c);
            p[1] = static_cast<uint8_t>(c * 7);
            p[2] = static_cast<uint8_t>(c * 13);
          }
        }
      }
    }
    host.OnFrame({{px.data(), W, H, W * 4}, {}, t});
  }
  // Still for 15 s: the ~60 tiles owed, of noise at ~4.5 kB each, take about
  // 9 s at 80% of 0.4 Mbps.
  for (int f = 0; f < 15 * kFps; ++f) {
    t += 1'000'000 / kFps;
    host.OnFrame({{px.data(), W, H, W * 4}, {}, t});
  }
  const uint64_t frames = host.stats().frames_encoded;
  PipeStopper stop{host_end, viewer_end};
  std::unique_lock<std::mutex> lock(mu);
  ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(5), [&] { return last_id >= frames; }));
  size_t wrong = 0;
  for (size_t i = 0; i < px.size(); i += 4) {
    if (std::memcmp(&px[i], &shown[i], 3) != 0) ++wrong;
  }
  std::string bad;
  for (int ty = 0; ty < H / 64; ++ty) {
    for (int tx = 0; tx < W / 64; ++tx) {
      size_t n = 0;
      for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 64; ++x) {
          const size_t i = (static_cast<size_t>(ty * 64 + y) * W + tx * 64 + x) * 4;
          if (std::memcmp(&px[i], &shown[i], 3) != 0) ++n;
        }
      }
      if (n) bad += " (" + std::to_string(tx) + "," + std::to_string(ty) + "):" + std::to_string(n);
    }
  }
  EXPECT_EQ(wrong, 0u) << "pixels still at video quality in tiles" << bad;
  EXPECT_GT(host.stats().lossless_bytes, 0u);
}

TEST(Session, FewColourTilesGoLosslessAndSkipTheEncoder) {
  auto [host_end, viewer_end] = transport::MakeLocalPipe();
  constexpr int W = 200, H = 120;
  auto enc = std::make_unique<FakeEncoder>();
  FakeEncoder* raw = enc.get();
  session::HostSession host(host_end.get(), std::move(enc), W, H,
                            {.refine_frames = 0, .progressive = false});
  session::ViewerSession viewer(viewer_end.get(), std::make_unique<FakeDecoder>());

  std::mutex mu;
  std::condition_variable cv;
  std::vector<std::vector<Rect>> got;
  std::vector<uint8_t> shown;
  viewer.SetFrameHandler([&](const tile::ImageView& img, const std::vector<Rect>& regions,
                             uint64_t, uint64_t) {
    std::lock_guard<std::mutex> lock(mu);
    got.push_back(regions);
    shown.assign(img.data, img.data + static_cast<size_t>(img.stride) * img.height);
    cv.notify_all();
  });

  std::vector<uint8_t> px(static_cast<size_t>(W) * H * 4, 50);
  for (size_t i = 3; i < px.size(); i += 4) px[i] = 255;  // the viewer shows opaque pixels
  capture::CapturedFrame f{{px.data(), W, H, W * 4}, {}, 0};
  host.OnFrame(f);              // keyframe: video + every tile lossless
  px[(70 * W + 150) * 4] = 99;  // one pixel in tile (2, 1): two colours
  host.OnFrame(f);

  PipeStopper stop{host_end, viewer_end};
  std::unique_lock<std::mutex> lock(mu);
  ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(5), [&] { return got.size() == 2; }));
  EXPECT_TRUE(got[0].empty());
  ASSERT_EQ(got[1].size(), 1u);
  EXPECT_EQ(got[1][0], (Rect{128, 64, 64, 56}));
  // The fake decoder outputs zeros, so every pixel shown came from the
  // lossless layer, which is exact.
  EXPECT_EQ(shown, px);
  EXPECT_EQ(raw->keyframe_requests, (std::vector<bool>{true}));  // no second encode

  const auto hs = host.stats();
  EXPECT_EQ(hs.frames_encoded, 2u);
  EXPECT_EQ(hs.video_frames, 1u);
  EXPECT_EQ(hs.lossless_tiles, 8u + 1u);  // 4x2 tiles, then the changed one
}

TEST(Session, InputReachesHost) {
  auto [host_end, viewer_end] = transport::MakeLocalPipe();
  session::HostSession host(host_end.get(), std::make_unique<FakeEncoder>(), 64, 64);
  session::ViewerSession viewer(viewer_end.get(), std::make_unique<FakeDecoder>());

  std::mutex mu;
  std::condition_variable cv;
  std::vector<proto::InputEvent> events;
  host.SetInputHandler([&](const proto::InputEvent& e) {
    std::lock_guard<std::mutex> lock(mu);
    events.push_back(e);
    cv.notify_all();
  });
  viewer.SendInput({proto::InputKind::kMouseDown, 10, 20, 0, 0, 1, 0});
  viewer.SendInput({proto::InputKind::kKeyDown, 0, 0, 0, 0, 36, 0});

  PipeStopper stop{host_end, viewer_end};
  std::unique_lock<std::mutex> lock(mu);
  ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(5), [&] { return events.size() == 2; }));
  EXPECT_EQ(events[0].kind, proto::InputKind::kMouseDown);
  EXPECT_EQ(events[0].x, 10);
  EXPECT_EQ(events[1].code, 36u);
}

TEST(Session, TilesToRegionsMergesHorizontalRuns) {
  std::vector<tile::TileChange> changes;
  for (int col : {0, 1, 2, 5}) changes.push_back({col, 0, 1, 0, {col * 64, 0, 64, 64}});
  changes.push_back({0, 1, 1, 0, {0, 64, 64, 64}});
  auto regions = session::TilesToRegions(changes);
  ASSERT_EQ(regions.size(), 3u);
  EXPECT_EQ(regions[0], (Rect{0, 64, 64, 64}));
  EXPECT_EQ(regions[1], (Rect{0, 0, 192, 64}));
  EXPECT_EQ(regions[2], (Rect{320, 0, 64, 64}));
}

TEST(Session, TilesToRegionsMergesRowsOfATileAndColumnsOfTiles) {
  // A tile split into pixel rows (what a dragged window uncovered), and the
  // same columns in the tile below: one rect.
  std::vector<tile::TileChange> changes;
  for (int y = 0; y < 64; ++y) changes.push_back({1, 0, 1, 0, {70, y, 30, 1}});
  changes.push_back({1, 1, 1, 0, {70, 64, 30, 64}});
  auto regions = session::TilesToRegions(changes);
  ASSERT_EQ(regions.size(), 1u);
  EXPECT_EQ(regions[0], (Rect{70, 0, 30, 128}));
}

TEST(Synthetic, IncrementalRenderMatchesFullRender) {
  constexpr int W = 640, H = 360;
  std::vector<uint8_t> inc;
  for (uint64_t i = 0; i < 120; ++i) capture::RenderSyntheticFrame(i, W, H, inc);
  std::vector<uint8_t> full;
  capture::RenderSyntheticFrame(119, W, H, full);
  EXPECT_EQ(inc, full);
}

TEST(Synthetic, AnySizeStaysInsideTheFrame) {
  // Small frames once wrote past the buffer (the page margins and the video
  // area did not fit). Under ASan this checks every pixel written.
  for (auto [w, h] : {std::pair{2, 2}, {16, 16}, {64, 64}, {100, 50}, {50, 300}, {130, 72}}) {
    std::vector<uint8_t> inc;
    for (uint64_t i = 0; i < 40; ++i) {
      for (const Rect& r : capture::RenderSyntheticFrame(i, w, h, inc)) {
        EXPECT_TRUE(r.w > 0 && r.h > 0 && r.x >= 0 && r.y >= 0 && r.right() <= w && r.bottom() <= h)
            << w << "x" << h << " frame " << i;
      }
    }
    std::vector<uint8_t> full;
    capture::RenderSyntheticFrame(39, w, h, full);
    EXPECT_EQ(inc, full) << w << "x" << h;
  }
}

TEST(Session, BusyAreaGetsItsOwnStreamAndASharpPictureWhenItStops) {
  auto [host_end, viewer_end] = transport::MakeLocalPipe();
  constexpr int W = 320, H = 256;  // 5x4 tiles
  session::HostOptions opts;
  opts.refine_frames = 0;
  std::vector<std::pair<int, int>> region_sizes;
  opts.region_encoder_factory = [&](const codec::EncoderConfig& c, std::string*) {
    region_sizes.emplace_back(c.width, c.height);
    return std::make_unique<FakeEncoder>();
  };
  opts.region_params.window = 4;
  opts.region_params.min_busy = 2;
  opts.region_params.confirm = 1;
  opts.region_params.linger = 3;
  session::HostSession host(host_end.get(), std::make_unique<FakeEncoder>(), W, H, opts);
  session::ViewerSession viewer(viewer_end.get(), std::make_unique<FakeDecoder>());
  viewer.SetDecoderFactory([] { return std::make_unique<FakeDecoder>(); });

  std::mutex mu;
  std::condition_variable cv;
  std::vector<std::vector<Rect>> got;
  viewer.SetFrameHandler([&](const tile::ImageView&, const std::vector<Rect>& regions, uint64_t,
                             uint64_t) {
    std::lock_guard<std::mutex> lock(mu);
    got.push_back(regions);
    cv.notify_all();
  });

  std::vector<uint8_t> px(static_cast<size_t>(W) * H * 4, 50);
  capture::CapturedFrame f{{px.data(), W, H, W * 4}, {}, 0};
  ASSERT_TRUE(host.OnFrame(f));  // keyframe
  // Noise in tiles (1..3, 1..2): too many colours for the lossless layer.
  uint32_t seed = 1;
  auto play = [&] {
    for (int y = 64; y < 192; ++y) {
      for (int x = 64; x < 256; ++x) {
        seed = seed * 1664525u + 1013904223u;
        std::memcpy(&px[(static_cast<size_t>(y) * W + x) * 4], &seed, 3);
      }
    }
  };
  int frames = 1;
  while (host.stats().regions_opened == 0 && frames < 10) {
    play();
    ASSERT_TRUE(host.OnFrame(f));
    ++frames;
  }
  // The area is found at the end of the second busy frame; the frame that
  // counts it as opened already sends its first picture.
  for (int i = 0; i < 3; ++i, ++frames) {
    play();
    ASSERT_TRUE(host.OnFrame(f));
  }
  auto st = host.stats();
  EXPECT_EQ(st.regions_opened, 1u);
  EXPECT_EQ(st.region_pictures, 4u);
  ASSERT_EQ(region_sizes.size(), 1u);
  EXPECT_EQ(region_sizes[0], (std::pair<int, int>{192, 128}));
  const uint64_t main_frames = st.video_frames;
  // Still: the area closes after `linger` quiet frames, and its tiles are
  // then sent once more through the full-screen encoder.
  for (int i = 0; i < 4; ++i, ++frames) ASSERT_TRUE(host.OnFrame(f));
  st = host.stats();
  EXPECT_EQ(st.regions_closed, 1u);
  EXPECT_EQ(st.video_frames, main_frames + 1);

  PipeStopper stop{host_end, viewer_end};
  std::unique_lock<std::mutex> lock(mu);
  const size_t sent = static_cast<size_t>(st.frames_encoded);
  ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(5), [&] { return got.size() == sent; }));
  EXPECT_EQ(viewer.stats().decode_errors, 0u);
  // An area picture covers the whole area.
  bool area_seen = false;
  for (const auto& g : got) {
    for (const Rect& r : g) area_seen |= r == Rect{64, 64, 192, 128};
  }
  EXPECT_TRUE(area_seen);
  // The final, sharp, picture covers the area's six tiles (merged per row).
  EXPECT_EQ(got.back(), (std::vector<Rect>{{64, 64, 192, 128}}));
}

TEST(Session, ADroppedAreaPictureDoesNotForceAnAreaKeyframe) {
  auto [host_end, viewer_end] = transport::MakeLocalPipe();
  constexpr int W = 320, H = 256;
  session::HostOptions opts;
  opts.refine_frames = 0;
  FakeEncoder* area = nullptr;
  opts.region_encoder_factory = [&](const codec::EncoderConfig&, std::string*) {
    auto e = std::make_unique<FakeEncoder>();
    area = e.get();
    return e;
  };
  opts.region_params.window = 4;
  opts.region_params.min_busy = 2;
  opts.region_params.confirm = 1;
  opts.region_params.linger = 3;
  session::HostSession host(host_end.get(), std::make_unique<FakeEncoder>(), W, H, opts);
  std::vector<uint8_t> px(static_cast<size_t>(W) * H * 4, 50);
  capture::CapturedFrame f{{px.data(), W, H, W * 4}, {}, 0};
  ASSERT_TRUE(host.OnFrame(f));
  uint32_t seed = 1;
  auto play = [&] {
    for (int y = 64; y < 192; ++y) {
      for (int x = 64; x < 256; ++x) {
        seed = seed * 1664525u + 1013904223u;
        std::memcpy(&px[(static_cast<size_t>(y) * W + x) * 4], &seed, 3);
      }
    }
  };
  for (int i = 0; i < 10 && !area; ++i) {
    play();
    ASSERT_TRUE(host.OnFrame(f));
  }
  ASSERT_NE(area, nullptr);
  play();
  ASSERT_TRUE(host.OnFrame(f));
  area->drop_next = true;  // VideoToolbox may drop at low rates
  play();
  ASSERT_TRUE(host.OnFrame(f));
  play();
  ASSERT_TRUE(host.OnFrame(f));
  // Only the area's first picture is a keyframe: a keyframe after each drop
  // made VideoToolbox drop more.
  ASSERT_GE(area->keyframe_requests.size(), 3u);
  EXPECT_TRUE(area->keyframe_requests.front());
  for (size_t i = 1; i < area->keyframe_requests.size(); ++i) EXPECT_FALSE(area->keyframe_requests[i]) << i;
  EXPECT_EQ(host.stats().encoder_drops, 1u);
  PipeStopper stop{host_end, viewer_end};
}

TEST(Session, ViewerAsksForAKeyframeAfterADecodeError) {
  auto [host_end, viewer_end] = transport::MakeLocalPipe();
  constexpr int W = 128, H = 64;
  auto enc = std::make_unique<FakeEncoder>();
  FakeEncoder* encoder = enc.get();
  auto dec = std::make_unique<FakeDecoder>();
  FakeDecoder* decoder = dec.get();
  session::HostSession host(host_end.get(), std::move(enc), W, H, {.lossless = false, .refine_frames = 0});
  session::ViewerSession viewer(viewer_end.get(), std::move(dec));
  std::mutex mu;
  std::condition_variable cv;
  int shown = 0;
  viewer.SetFrameHandler([&](const tile::ImageView&, const std::vector<Rect>&, uint64_t, uint64_t) {
    std::lock_guard<std::mutex> lock(mu);
    ++shown;
    cv.notify_all();
  });
  PipeStopper stop{host_end, viewer_end};
  std::vector<uint8_t> px(static_cast<size_t>(W) * H * 4, 50);
  capture::CapturedFrame f{{px.data(), W, H, W * 4}, {}, 0};
  host.OnFrame(f);
  {
    std::unique_lock<std::mutex> lock(mu);
    ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(5), [&] { return shown == 1; }));
  }
  decoder->fail_next = true;
  px[0] = 1;
  host.OnFrame(f);
  // The request travels back on the control channel.
  for (int i = 0; i < 500 && host.stats().keyframe_requests == 0; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_EQ(host.stats().keyframe_requests, 1u);
  EXPECT_EQ(viewer.stats().keyframe_requests, 1u);
  px[0] = 2;
  host.OnFrame(f);
  ASSERT_EQ(encoder->keyframe_requests.size(), 3u);
  EXPECT_FALSE(encoder->keyframe_requests[1]);
  EXPECT_TRUE(encoder->keyframe_requests[2]);
  std::unique_lock<std::mutex> lock(mu);
  ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(5), [&] { return shown == 2; }));
}

// A still screen: what stays video is taken from more pictures, coded with
// the lower QP floor, and then nothing more is sent. A change starts over,
// at the ordinary floor while it moves.
TEST(Session, StillScreenPolishesVideoTilesAtAFinerQuality) {
  auto [host_end, viewer_end] = transport::MakeLocalPipe();
  constexpr int W = 200, H = 120;  // 4 x 2 tiles
  auto enc = std::make_unique<FakeEncoder>();
  FakeEncoder* raw = enc.get();
  session::HostSession host(host_end.get(), std::move(enc), W, H,
                            {.lossless = false, .refine_frames = 2, .still_refine_frames = 5, .still_min_qp = 12,
                             .progressive = false});
  session::ViewerSession viewer(viewer_end.get(), std::make_unique<FakeDecoder>());
  std::mutex mu;
  std::vector<size_t> regions_per_frame;
  viewer.SetFrameHandler([&](const tile::ImageView&, const std::vector<Rect>& regions, uint64_t, uint64_t) {
    std::lock_guard<std::mutex> lock(mu);
    regions_per_frame.push_back(regions.size());
  });
  std::vector<uint8_t> px(static_cast<size_t>(W) * H * 4, 50);
  capture::CapturedFrame f{{px.data(), W, H, W * 4}, {}, 0};
  auto run_until_skipped = [&] {
    const uint64_t skipped = host.stats().frames_skipped;
    for (int n = 0; host.stats().frames_skipped == skipped && n < 100; ++n) host.OnFrame(f);
  };
  // The keyframe, its 2 refinements, then (after the wait that makes sure
  // the tiles are static) 5 pictures at the finer floor.
  run_until_skipped();
  EXPECT_FALSE(host.work_pending());
  ASSERT_EQ(raw->min_qp.size(), 1u + 2u + 5u);
  EXPECT_EQ(raw->min_qp.front(), 0);  // the keyframe: the encoder's own floor
  EXPECT_EQ(std::count(raw->min_qp.begin(), raw->min_qp.end(), 12), 5 + 2);  // refinements of a still screen too
  for (size_t i = 3; i < raw->min_qp.size(); ++i) EXPECT_EQ(raw->min_qp[i], 12) << i;
  host.OnFrame(f);
  EXPECT_EQ(raw->min_qp.size(), 8u);  // nothing more

  // One tile changes: it is refined and polished again, the others are not.
  px[(70 * W + 150) * 4] = 99;  // tile (2, 1)
  run_until_skipped();
  EXPECT_EQ(raw->min_qp.size(), 8u + 1u + 2u + 5u);
  EXPECT_FALSE(host.work_pending());

  PipeStopper stop{host_end, viewer_end};
  for (int i = 0; i < 500; ++i) {
    {
      std::lock_guard<std::mutex> lock(mu);
      if (regions_per_frame.size() >= 16) break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  std::lock_guard<std::mutex> lock(mu);
  ASSERT_GE(regions_per_frame.size(), 16u);
  // The last pictures (the second polish) carry only the tile that changed.
  for (size_t i = regions_per_frame.size() - 5; i < regions_per_frame.size(); ++i) {
    EXPECT_EQ(regions_per_frame[i], 1u) << i;
  }
}

TEST(Session, ProgressiveKeyframeSendsVideoFirstAndLosslessAfter) {
  auto [host_end, viewer_end] = transport::MakeLocalPipe();
  constexpr int W = 200, H = 120;  // 4 x 2 tiles
  auto enc = std::make_unique<FakeEncoder>();
  FakeEncoder* raw = enc.get();
  // A budget of about 3 lossless tiles per frame.
  session::HostSession host(host_end.get(), std::move(enc), W, H,
                            {.refine_frames = 0, .fps = 10, .bitrate_bps = 10 * 8 * 1500 * 3 * 2});
  session::ViewerSession viewer(viewer_end.get(), std::make_unique<FakeDecoder>());
  std::mutex mu;
  std::condition_variable cv;
  std::vector<std::vector<uint8_t>> shown;
  viewer.SetFrameHandler([&](const tile::ImageView& img, const std::vector<Rect>&, uint64_t, uint64_t) {
    std::lock_guard<std::mutex> lock(mu);
    shown.emplace_back(img.data, img.data + static_cast<size_t>(img.stride) * img.height);
    cv.notify_all();
  });
  // Two-colour stripes (text-like, lossless), except the first tile: noise
  // with too many colours, which stays video.
  std::vector<uint8_t> px(static_cast<size_t>(W) * H * 4);
  uint32_t seed = 1;
  for (int y = 0; y < H; ++y) {
    for (int x = 0; x < W; ++x) {
      uint8_t* p = px.data() + (static_cast<size_t>(y) * W + x) * 4;
      seed = seed * 1664525u + 1013904223u;
      const uint8_t v = x < 64 && y < 64 ? static_cast<uint8_t>(seed >> 24) : (y % 4 < 2 ? 30 : 220);
      p[0] = p[1] = v;
      p[2] = static_cast<uint8_t>(x < 64 && y < 64 ? seed >> 16 : v);
      p[3] = 255;
    }
  }
  capture::CapturedFrame f{{px.data(), W, H, W * 4}, {}, 0};
  // The screen never changes; frames keep coming until all tiles are exact.
  int frames = 0;
  while (frames < 10) {
    host.OnFrame(f);
    ++frames;
    if (host.stats().progressive_tiles == 7) break;
  }
  EXPECT_EQ(host.stats().progressive_tiles, 7u);
  EXPECT_GE(frames, 3);  // spread over frames, not all at once
  // Only the keyframe went through the encoder so far ...
  ASSERT_EQ(raw->keyframe_requests.size(), 1u);
  EXPECT_TRUE(raw->keyframe_requests[0]);
  // ... then the video tile gets a fresh intra picture of its own ...
  host.OnFrame(f);
  ++frames;
  ASSERT_EQ(raw->keyframe_requests.size(), 2u);
  EXPECT_TRUE(raw->keyframe_requests[1]);
  host.OnFrame(f);  // ... and nothing is owed any more: skipped
  EXPECT_EQ(host.stats().frames_skipped, 1u);

  PipeStopper stop{host_end, viewer_end};
  std::unique_lock<std::mutex> lock(mu);
  ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(5), [&] { return static_cast<int>(shown.size()) == frames; }));
  // The keyframe is video only (the fake decoder shows zeros) ...
  EXPECT_NE(shown.front(), px);
  // ... and the lossless tiles make the text exact.
  const auto& last = shown.back();
  for (int y = 0; y < H; ++y) {
    for (int x = 0; x < W; ++x) {
      if (x < 64 && y < 64) continue;
      const size_t o = (static_cast<size_t>(y) * W + x) * 4;
      ASSERT_EQ(std::memcmp(&last[o], &px[o], 3), 0) << x << "," << y;
    }
  }
}

// What a change leaves owed (tiles over the lossless budget, a picture the
// encoder dropped) shows up in work_pending(), and re-submitting the last
// frame pays it off: the Host does that while screen capture sends no
// frames for a static screen.
TEST(Session, WorkLeftOwedIsPaidOffByResubmittingTheLastFrame) {
  auto [host_end, viewer_end] = transport::MakeLocalPipe();
  constexpr int W = 320, H = 192;
  auto enc = std::make_unique<FakeEncoder>();
  FakeEncoder* encp = enc.get();
  session::HostOptions o;
  o.bitrate_bps = 200'000;
  session::HostSession host(host_end.get(), std::move(enc), W, H, o);
  PipeStopper stop{host_end, viewer_end};

  std::vector<uint8_t> px(static_cast<size_t>(W) * H * 4, 0xe0);
  uint64_t t = 1'000'000;
  capture::CapturedFrame f{{px.data(), W, H, W * 4}, {}, t};
  host.OnFrame(f);  // keyframe
  for (int i = 0; i < 100 && host.work_pending(); ++i) {
    f.capture_time_us = t += 100'000;
    host.OnFrame(f);
  }
  ASSERT_FALSE(host.work_pending());

  // Two-colour detail over the whole screen: far over one frame's lossless
  // budget at 200 kbps.
  uint32_t seed = 7;
  for (size_t i = 0; i < px.size(); i += 4) {
    seed = seed * 1103515245u + 12345u;
    if ((seed >> 16) % 3 == 0) px[i] = px[i + 1] = px[i + 2] = 0x20;
  }
  f.capture_time_us = t += 33'000;
  host.OnFrame(f);
  EXPECT_TRUE(host.work_pending());
  int resubmits = 0;
  while (host.work_pending() && resubmits < 200) {
    std::vector<uint8_t> last = host.last_frame();
    capture::CapturedFrame r{{last.data(), W, H, W * 4}, {}, t += 100'000};
    host.OnFrame(r);
    ++resubmits;
  }
  EXPECT_FALSE(host.work_pending()) << resubmits << " re-submits";
  EXPECT_GT(resubmits, 0);

  // A dropped picture's video tiles are owed too.
  px[(10 * W + 10) * 4] ^= 0x55;
  px[(100 * W + 200) * 4] ^= 0x55;
  encp->drop_next = true;
  f.capture_time_us = t += 33'000;
  host.OnFrame(f);
  if (host.stats().encoder_drops > 0) {
    EXPECT_TRUE(host.work_pending());
    std::vector<uint8_t> last = host.last_frame();
    capture::CapturedFrame r{{last.data(), W, H, W * 4}, {}, t += 100'000};
    for (int i = 0; i < 100 && host.work_pending(); ++i) {
      r.capture_time_us = t += 100'000;
      host.OnFrame(r);
    }
    EXPECT_FALSE(host.work_pending());
  }
}

// ---- text mode ------------------------------------------------------------

namespace {

// What the viewer shows, copied on every frame.
struct Shown {
  std::mutex mu;
  std::condition_variable cv;
  std::vector<uint8_t> px;
  uint64_t last_id = 0;
  // Tiles that showed content and then turned into one plain colour again
  // (every tile of the test's screen has text, so that is a placeholder
  // coming back), counted over all frames.
  bool watch_placeholders = false;
  size_t placeholders_back = 0;
  std::string where;
  std::vector<uint8_t> had_content;
  void Attach(session::ViewerSession& viewer) {
    viewer.SetFrameHandler([this](const tile::ImageView& img, const std::vector<Rect>&, uint64_t id, uint64_t) {
      std::lock_guard<std::mutex> lock(mu);
      px.resize(static_cast<size_t>(img.width) * img.height * 4);
      for (int y = 0; y < img.height; ++y) {
        std::memcpy(px.data() + static_cast<size_t>(y) * img.width * 4,
                    img.data + static_cast<size_t>(y) * img.stride, static_cast<size_t>(img.width) * 4);
      }
      if (watch_placeholders) {
        const int cols = (img.width + 63) / 64, rows = (img.height + 63) / 64;
        had_content.resize(static_cast<size_t>(cols) * rows, 0);
        for (int ty = 0; ty < rows; ++ty) {
          for (int tx = 0; tx < cols; ++tx) {
            const uint8_t* first = &px[(static_cast<size_t>(ty) * 64 * img.width + tx * 64) * 4];
            bool plain = true;
            for (int y = ty * 64; y < std::min(img.height, ty * 64 + 64) && plain; ++y) {
              for (int x = tx * 64; x < std::min(img.width, tx * 64 + 64) && plain; ++x) {
                plain = std::memcmp(&px[(static_cast<size_t>(y) * img.width + x) * 4], first, 3) == 0;
              }
            }
            uint8_t& had = had_content[static_cast<size_t>(ty) * cols + tx];
            // (All black is the fake decoder's video picture: content on
            // its way, not a placeholder.)
            if (plain && had && (first[0] | first[1] | first[2]) != 0) {
              ++placeholders_back;
              where += " frame " + std::to_string(id) + " tile (" + std::to_string(tx) + "," + std::to_string(ty) + ")";
            }
            had = !plain;
          }
        }
      }
      last_id = id;
      cv.notify_all();
    });
  }
  // Tiles whose pixels differ from `want` once frame `id` has been shown.
  std::string Differences(const std::vector<uint8_t>& want, int w, int h, uint64_t id) {
    std::unique_lock<std::mutex> lock(mu);
    if (!cv.wait_for(lock, std::chrono::seconds(10), [&] { return last_id >= id; })) return " (frames missing)";
    std::string bad;
    for (int ty = 0; ty * 64 < h; ++ty) {
      for (int tx = 0; tx * 64 < w; ++tx) {
        size_t n = 0;
        for (int y = ty * 64; y < std::min(h, ty * 64 + 64); ++y) {
          for (int x = tx * 64; x < std::min(w, tx * 64 + 64); ++x) {
            const size_t i = (static_cast<size_t>(y) * w + x) * 4;
            if (std::memcmp(&want[i], &px[i], 3) != 0) ++n;
          }
        }
        if (n) bad += " (" + std::to_string(tx) + "," + std::to_string(ty) + "):" + std::to_string(n);
      }
    }
    return bad;
  }
};

// "Text": pixels of `colors` colours of one hue family, irregular enough
// that deflate finds little.
void FillText(std::vector<uint8_t>& px, int w, const Rect& r, uint32_t& seed, int colors) {
  for (int y = r.y; y < r.bottom(); ++y) {
    for (int x = r.x; x < r.right(); ++x) {
      seed = seed * 1103515245u + 12345u;
      const uint32_t c = (seed >> 16) % static_cast<uint32_t>(colors);
      uint8_t* p = &px[(static_cast<size_t>(y) * w + x) * 4];
      p[0] = static_cast<uint8_t>(250 - c * 17);
      p[1] = static_cast<uint8_t>(250 - c * 16);
      p[2] = static_cast<uint8_t>(245 - c * 15);
      p[3] = 255;
    }
  }
}

}  // namespace

TEST(Session, TextModeKeepsTextWithinTheBitrateAndEndsUpExact) {
  // Text beyond the budget goes as video first, as without text mode (a
  // tile left as it was among new ones looks worse than a soft one), and
  // follows losslessly, within the bitrate. Twelve colours a tile, so the
  // reduced palette is exact.
  auto [host_end, viewer_end] = transport::MakeLocalPipe();
  constexpr int W = 640, H = 384, kFps = 30;
  constexpr int64_t kBitrate = 400'000;
  size_t pictures = 0;
  auto enc = std::make_unique<FakeEncoder>();
  enc->pictures = &pictures;
  session::HostSession host(host_end.get(), std::move(enc), W, H,
                            {.text_mode = true, .fps = kFps, .bitrate_bps = kBitrate});
  session::ViewerSession viewer(nullptr, std::make_unique<FakeDecoder>());
  Shown shown;
  shown.Attach(viewer);
  viewer.Attach(viewer_end.get());

  std::vector<uint8_t> px(static_cast<size_t>(W) * H * 4, 250);
  uint32_t seed = 1;
  uint64_t t = 1'000'000;
  host.OnFrame({{px.data(), W, H, W * 4}, {}, t});  // keyframe: a plain screen
  const uint64_t after_key = host.stats().lossless_bytes;
  constexpr int kFrames = 60;
  for (int f = 0; f < kFrames; ++f) {
    t += 1'000'000 / kFps;
    for (int ty = 0; ty < H / 64; ++ty) {
      for (int tx = 0; tx < W / 64; ++tx) {
        if ((tx + ty + f) % 3 == 0) FillText(px, W, {tx * 64, ty * 64, 64, 64}, seed, 12);
      }
    }
    host.OnFrame({{px.data(), W, H, W * 4}, {}, t});
  }
  const double seconds = static_cast<double>(kFrames) / kFps;
  const double sent = static_cast<double>(host.stats().lossless_bytes - after_key);
  // The bitrate, what the bucket holds (300 ms), and one tile a frame.
  EXPECT_LE(sent, kBitrate / 8.0 * (seconds + 0.3) * 1.25) << sent << " bytes in " << seconds << " s";
  EXPECT_TRUE(host.work_pending());
  // Still: the 60 tiles (2 kB each, twice where they had a quick pass first)
  // take a few seconds.
  for (int f = 0; f < 20 * kFps && host.work_pending(); ++f) {
    t += 1'000'000 / kFps;
    host.OnFrame({{px.data(), W, H, W * 4}, {}, t});
  }
  EXPECT_FALSE(host.work_pending());
  const uint64_t frames = host.stats().frames_encoded;
  PipeStopper stop{host_end, viewer_end};
  EXPECT_EQ(shown.Differences(px, W, H, frames), "");
}

TEST(Session, TextModeContextTilesArriveExactAndSmaller) {
  // The same screenful of "text" with and without context tiles (the
  // viewer decodes both): what arrives is the same, in fewer bytes.
  constexpr int W = 512, H = 384, kFps = 30;
  uint64_t bytes[2] = {0, 0};
  for (int with = 0; with < 2; ++with) {
    auto [host_end, viewer_end] = transport::MakeLocalPipe();
    session::HostSession host(host_end.get(), std::make_unique<FakeEncoder>(), W, H,
                              {.text_mode = true, .context_tiles = with == 1, .fps = kFps, .bitrate_bps = 2'000'000});
    session::ViewerSession viewer(nullptr, std::make_unique<FakeDecoder>());
    Shown shown;
    shown.Attach(viewer);
    viewer.Attach(viewer_end.get());
    std::vector<uint8_t> px(static_cast<size_t>(W) * H * 4, 250);
    for (size_t i = 3; i < px.size(); i += 4) px[i] = 255;
    // Strokes on a background, with softer pixels along their edges.
    uint32_t seed = 9;
    auto rnd = [&](uint32_t n) { return ((seed = seed * 1103515245u + 12345u) >> 16) % n; };
    for (int k = 0; k < 2500; ++k) {
      const int x = 2 + static_cast<int>(rnd(W - 12)), y = 2 + static_cast<int>(rnd(H - 12));
      const int len = 2 + static_cast<int>(rnd(8));
      const bool across = rnd(2) == 0;
      for (int i = -1; i <= len; ++i) {
        for (int side = -1; side <= 1; ++side) {
          const int px_x = across ? x + i : x + side, px_y = across ? y + side : y + i;
          const bool edge = side != 0 || i < 0 || i == len;
          uint8_t* p = &px[(static_cast<size_t>(px_y) * W + px_x) * 4];
          const uint8_t v = edge ? static_cast<uint8_t>(std::min<int>(p[0], 120 + 20 * static_cast<int>(rnd(5)))) : 30;
          p[0] = p[1] = p[2] = v;
        }
      }
    }
    uint64_t t = 1'000'000;
    host.OnFrame({{px.data(), W, H, W * 4}, {}, t});
    for (int f = 0; f < 30 * kFps && host.work_pending(); ++f) {
      t += 1'000'000 / kFps;
      host.OnFrame({{px.data(), W, H, W * 4}, {}, t});
    }
    EXPECT_FALSE(host.work_pending());
    bytes[with] = host.stats().lossless_bytes;
    const uint64_t frames = host.stats().frames_encoded;
    PipeStopper stop{host_end, viewer_end};
    EXPECT_EQ(shown.Differences(px, W, H, frames), "") << (with ? "context tiles" : "deflate");
  }
  EXPECT_LT(bytes[1] * 10, bytes[0] * 8) << bytes[1] << " bytes with context tiles, " << bytes[0] << " without";
}

TEST(Session, TextModeEndsUpExactAfterScrollingAndEditing) {
  // Scrolls (copies of what the viewer shows), cached tiles and partial
  // updates all assume the viewer has the previous frame, which in text
  // mode it may not (stale tiles). Whatever the order of events, a still
  // screen must end up exactly right.
  constexpr int W = 512, H = 448, kFps = 30;
  for (uint32_t run = 1; run <= 6; ++run) {
    auto [host_end, viewer_end] = transport::MakeLocalPipe();
    // Odd runs with context tiles, even ones with deflate.
    session::HostSession host(host_end.get(), std::make_unique<FakeEncoder>(), W, H,
                              {.text_mode = true, .context_tiles = run % 2 == 1, .fps = kFps,
                               .bitrate_bps = 60'000 * run});
    session::ViewerSession viewer(nullptr, std::make_unique<FakeDecoder>());
    Shown shown;
    shown.Attach(viewer);
    viewer.Attach(viewer_end.get());
    std::vector<uint8_t> px(static_cast<size_t>(W) * H * 4, 250);
    uint32_t seed = run * 7919;
    auto rnd = [&](uint32_t n) { return ((seed = seed * 1103515245u + 12345u) >> 16) % n; };
    // Lines of "text" 16 pixels high with gaps, as a page has.
    auto line = [&](int y) {
      uint32_t s = seed;
      FillText(px, W, {32, y, W - 64 - static_cast<int>(rnd(200)), 10}, s, 6);
      seed = s;
    };
    for (int y = 16; y + 16 <= H; y += 16) line(y);
    uint64_t t = 1'000'000;
    host.OnFrame({{px.data(), W, H, W * 4}, {}, t});
    for (int f = 0; f < 150; ++f) {
      t += 1'000'000 / kFps;
      const uint32_t what = rnd(10);
      if (what < 5) {
        // Scroll up by one or more lines; new lines come in at the bottom.
        const int dy = 16 * (1 + static_cast<int>(rnd(4)));
        std::memmove(px.data(), px.data() + static_cast<size_t>(dy) * W * 4, static_cast<size_t>(H - dy) * W * 4);
        std::fill(px.begin() + static_cast<ptrdiff_t>(static_cast<size_t>(H - dy) * W * 4), px.end(), 250);
        for (int y = H - dy; y + 16 <= H; y += 16) line(y);
      } else if (what < 8) {
        // Typing: a few characters somewhere.
        uint32_t s = seed;
        FillText(px, W, {static_cast<int>(rnd(W - 40)), 16 * static_cast<int>(rnd(H / 16)), 24, 10}, s, 6);
        seed = s;
      } else if (what == 8) {
        // A window appears: a block of other text.
        const Rect r{64 * static_cast<int>(rnd(4)), 64 * static_cast<int>(rnd(3)), 200, 180};
        uint32_t s = seed;
        FillText(px, W, r, s, 9);
        seed = s;
      }  // else: nothing changes
      host.OnFrame({{px.data(), W, H, W * 4}, {}, t});
    }
    for (int f = 0; f < 120 * kFps && host.work_pending(); ++f) {
      t += 1'000'000 / kFps;
      host.OnFrame({{px.data(), W, H, W * 4}, {}, t});
    }
    EXPECT_FALSE(host.work_pending()) << "run " << run;
    const uint64_t frames = host.stats().frames_encoded;
    PipeStopper stop{host_end, viewer_end};
    EXPECT_EQ(shown.Differences(px, W, H, frames), "") << "run " << run;
  }
}

TEST(Session, TextModeLeavesNoPlaceholderBehindWhenTilesChangeBeforeTheirTurn) {
  // After a keyframe the viewer shows text tiles as their background colour
  // until each one's turn comes. On a slow link that takes a while, and
  // meanwhile carets blink, a clock ticks, rows get highlighted: tiles
  // change in part, come back to what they were, scroll. None of that may
  // treat the placeholder as content the viewer has (blocks of plain colour
  // stayed on screen, marked as done).
  constexpr int W = 640, H = 512, kFps = 30;
  for (int variant = 0; variant < 4; ++variant) {
    auto [host_end, viewer_end] = transport::MakeLocalPipe();
    session::HostSession host(host_end.get(), std::make_unique<FakeEncoder>(), W, H,
                              {.text_mode = true, .context_tiles = variant % 2 == 1, .fps = kFps,
                               .bitrate_bps = 40'000});
    session::ViewerSession viewer(nullptr, std::make_unique<FakeDecoder>());
    Shown shown;
    shown.watch_placeholders = true;
    shown.Attach(viewer);
    viewer.Attach(viewer_end.get());
    std::vector<uint8_t> px(static_cast<size_t>(W) * H * 4, 250);
    uint32_t seed = 77 + static_cast<uint32_t>(variant);
    for (int y = 8; y + 12 <= H; y += 16) FillText(px, W, {8, y, W - 16, 10}, seed, 6);
    const std::vector<uint8_t> page = px;
    auto set = [&](const Rect& r, bool on) {
      for (int y = r.y; y < r.bottom(); ++y) {
        for (int x = r.x; x < r.right(); ++x) {
          uint8_t* p = &px[(static_cast<size_t>(y) * W + x) * 4];
          if (on) {
            p[0] = p[1] = p[2] = 20;
          } else {
            std::memcpy(p, &page[(static_cast<size_t>(y) * W + x) * 4], 4);
          }
        }
      }
    };
    uint64_t t = 1'000'000;
    host.OnFrame({{px.data(), W, H, W * 4}, {}, t});
    // At 40 kbps the page takes many seconds; all of this happens meanwhile.
    for (int f = 0; f < 240; ++f) {
      t += 1'000'000 / kFps;
      // Carets in tiles all over the screen, on for 15 frames, off for 15.
      if (f % 15 == 0) {
        const bool on = (f / 15) % 2 == 1;  // first at frame 15: the tiles have been still a while
        for (int k = 0; k < 6; ++k) set({40 + k * 97, 30 + k * 80, 2, 14}, on);
        // A highlighted row that moves down (variants 2 and 3: with a scroll).
        if (variant >= 2 && f % 60 == 0) {
          std::memmove(px.data(), px.data() + static_cast<size_t>(32) * W * 4, static_cast<size_t>(H - 32) * W * 4);
        }
      }
      host.OnFrame({{px.data(), W, H, W * 4}, {}, t});
    }
    for (int f = 0; f < 300 * kFps && host.work_pending(); ++f) {
      t += 1'000'000 / kFps;
      host.OnFrame({{px.data(), W, H, W * 4}, {}, t});
    }
    EXPECT_FALSE(host.work_pending()) << "variant " << variant;
    const uint64_t frames = host.stats().frames_encoded;
    PipeStopper stop{host_end, viewer_end};
    EXPECT_EQ(shown.Differences(px, W, H, frames), "") << "variant " << variant;
    EXPECT_EQ(shown.placeholders_back, 0u) << "variant " << variant << ":" << shown.where;
  }
}

TEST(Session, TextModeSendsTheWholeScreenAtOnceWhenThePathTakesIt) {
  // Saving traffic is not sending slowly: at a bitrate that covers a
  // screenful of text in half a second, the keyframe carries all of it. No
  // placeholders, nothing owed afterwards, and a window dragged across the
  // screen right away is exact in every frame it is shown.
  constexpr int W = 640, H = 512, kFps = 30;
  auto [host_end, viewer_end] = transport::MakeLocalPipe();
  size_t pictures = 0;
  auto enc = std::make_unique<FakeEncoder>();
  enc->pictures = &pictures;
  session::HostSession host(host_end.get(), std::move(enc), W, H,
                            {.text_mode = true, .context_tiles = true, .fps = kFps, .bitrate_bps = 4'000'000});
  session::ViewerSession viewer(nullptr, std::make_unique<FakeDecoder>());
  Shown shown;
  shown.watch_placeholders = true;
  shown.Attach(viewer);
  viewer.Attach(viewer_end.get());
  std::vector<uint8_t> px(static_cast<size_t>(W) * H * 4, 250);
  uint32_t seed = 3;
  for (int y = 8; y + 12 <= H; y += 16) FillText(px, W, {8, y, W - 16, 10}, seed, 6);
  uint64_t t = 1'000'000;
  host.OnFrame({{px.data(), W, H, W * 4}, {}, t});
  EXPECT_FALSE(host.work_pending()) << "text was left for later";
  EXPECT_EQ(shown.Differences(px, W, H, 1), "");
  const std::vector<uint8_t> page = px;
  for (int f = 1; f <= 40; ++f) {
    t += 1'000'000 / kFps;
    px = page;
    // A block of the page ("a window") moving 5 pixels a frame.
    for (int y = 0; y < 160; ++y) {
      std::memcpy(&px[(static_cast<size_t>(100 + f * 3 + y) * W + 40 + f * 5) * 4],
                  &page[(static_cast<size_t>(200 + y) * W + 300) * 4], 200 * 4);
    }
    host.OnFrame({{px.data(), W, H, W * 4}, {}, t});
    EXPECT_FALSE(host.work_pending()) << "frame " << f;
  }
  EXPECT_EQ(pictures, 1u);
  const uint64_t frames = host.stats().frames_encoded;
  PipeStopper stop{host_end, viewer_end};
  EXPECT_EQ(shown.Differences(px, W, H, frames), "");
  EXPECT_EQ(shown.placeholders_back, 0u) << shown.where;
}

TEST(Session, TextModeKeepsUpWithTextThatChangesEverywhereAndEndsUpExact) {
  // Smooth scrolling re-renders the text at every offset: no tile matches a
  // copy, every tile changes in every frame. Sent losslessly that is far
  // too much, and sent twice a second rows of older text stood on screen.
  // While the video bucket holds its burst the tiles go through the video
  // encoder, a picture a frame; once the page stops, the text is exact.
  constexpr int W = 640, H = 384, kFps = 30;
  auto [host_end, viewer_end] = transport::MakeLocalPipe();
  size_t pictures = 0;
  auto enc = std::make_unique<FakeEncoder>();
  enc->pad = 3000;
  enc->pictures = &pictures;
  session::HostSession host(host_end.get(), std::move(enc), W, H,
                            {.text_mode = true, .context_tiles = true, .fps = kFps, .bitrate_bps = 4'000'000});
  session::ViewerSession viewer(nullptr, std::make_unique<FakeDecoder>());
  Shown shown;
  shown.Attach(viewer);
  viewer.Attach(viewer_end.get());
  std::vector<uint8_t> px(static_cast<size_t>(W) * H * 4, 250);
  for (size_t i = 3; i < px.size(); i += 4) px[i] = 255;
  uint32_t seed = 11;
  uint64_t t = 1'000'000;
  host.OnFrame({{px.data(), W, H, W * 4}, {}, t});
  constexpr int kMoving = 75;  // 2.5 s
  for (int f = 0; f < kMoving; ++f) {
    t += 1'000'000 / kFps;
    FillText(px, W, {0, 0, W, H}, seed, 6);  // all of it, new every frame
    host.OnFrame({{px.data(), W, H, W * 4}, {}, t});
  }
  // After the first half second (until then it is not known to be an
  // animation), a picture for nearly every frame.
  EXPECT_GE(pictures, static_cast<size_t>(kMoving - kFps / 2 - 10)) << pictures << " pictures in " << kMoving << " frames";
  for (int f = 0; f < 60 * kFps && host.work_pending(); ++f) {
    t += 1'000'000 / kFps;
    host.OnFrame({{px.data(), W, H, W * 4}, {}, t});
  }
  EXPECT_FALSE(host.work_pending());
  const uint64_t frames = host.stats().frames_encoded;
  PipeStopper stop{host_end, viewer_end};
  EXPECT_EQ(shown.Differences(px, W, H, frames), "");
}

TEST(Session, TextModeCodesMotionAsWithoutIt) {
  // A photo-like area that changes every frame, 20 kB pictures on a 20 Mbps
  // path: a picture every frame, at the whole bitrate, as without text mode
  // (a fixed video allowance made a full-screen transition a second of
  // blocks), and polished once it stops.
  auto [host_end, viewer_end] = transport::MakeLocalPipe();
  constexpr int W = 320, H = 192, kFps = 30;
  size_t pictures = 0;
  int64_t bitrate = 0;
  auto enc = std::make_unique<FakeEncoder>();
  enc->pad = 20'000;
  enc->pictures = &pictures;
  enc->bitrate = &bitrate;
  session::HostSession host(host_end.get(), std::move(enc), W, H,
                            {.text_mode = true, .fps = kFps, .bitrate_bps = 20'000'000});
  host.SetBitrate(20'000'000);
  EXPECT_EQ(bitrate, 20'000'000);
  std::vector<uint8_t> px(static_cast<size_t>(W) * H * 4, 250);
  uint64_t t = 1'000'000;
  host.OnFrame({{px.data(), W, H, W * 4}, {}, t});
  uint32_t seed = 5;
  constexpr int kFrames = 3 * kFps;
  const size_t first = pictures;
  for (int f = 0; f < kFrames; ++f) {
    t += 1'000'000 / kFps;
    for (int y = 0; y < 128; ++y) {
      for (int x = 0; x < 128; ++x) {
        seed = seed * 1103515245u + 12345u;
        std::memcpy(&px[(static_cast<size_t>(y) * W + x) * 4], &seed, 3);  // noise: no palette
      }
    }
    host.OnFrame({{px.data(), W, H, W * 4}, {}, t});
  }
  EXPECT_EQ(pictures - first, static_cast<size_t>(kFrames));
  const size_t before = pictures;
  EXPECT_TRUE(host.work_pending());  // refinement and the still-screen polish
  for (int f = 0; f < 60 * kFps && host.work_pending(); ++f) {
    t += 1'000'000 / kFps;
    host.OnFrame({{px.data(), W, H, W * 4}, {}, t});
  }
  EXPECT_FALSE(host.work_pending());
  EXPECT_GT(pictures, before);
  PipeStopper stop{host_end, viewer_end};
}

TEST(Session, LeavingTextModeRefreshesTheWholePicture) {
  auto [host_end, viewer_end] = transport::MakeLocalPipe();
  constexpr int W = 128, H = 128;
  session::HostSession host(host_end.get(), std::make_unique<FakeEncoder>(), W, H, {.text_mode = true});
  std::vector<uint8_t> px(static_cast<size_t>(W) * H * 4, 250);
  host.OnFrame({{px.data(), W, H, W * 4}, {}, 1'000'000});
  EXPECT_FALSE(host.keyframe_pending());
  host.SetTextMode(true);  // no change
  EXPECT_FALSE(host.keyframe_pending());
  host.SetTextMode(false);
  EXPECT_FALSE(host.text_mode());
  EXPECT_TRUE(host.keyframe_pending());
  host.OnFrame({{px.data(), W, H, W * 4}, {}, 1'033'000});
  host.SetTextMode(true);  // reduced colours need no refresh
  EXPECT_FALSE(host.keyframe_pending());
  PipeStopper stop{host_end, viewer_end};
}

// ---- what a copy and the encoder's picture may bring into a tile -----------

namespace {

void FillNoise(std::vector<uint8_t>& px, int w, const Rect& r, uint32_t& seed) {
  for (int y = r.y; y < r.bottom(); ++y) {
    for (int x = r.x; x < r.right(); ++x) {
      seed = seed * 1103515245u + 12345u;
      uint8_t* p = &px[(static_cast<size_t>(y) * w + x) * 4];
      std::memcpy(p, &seed, 3);  // no palette: video
      p[3] = 255;
    }
  }
}

}  // namespace

TEST(Session, AScrollDoesNotCopyVideoQualityIntoATileThatDidNotChange) {
  // A page of photo-like content (video quality at the viewer; the fake
  // decoder shows it black) with a blank block in it, scrolled by 7 pixels.
  // The blank tile looks the same before and after, and matches the scroll
  // too: made part of the move, its last 7 rows were copied from the tile
  // below, as the viewer has it, and stayed like that, since the tile had
  // not changed and counted as exact.
  for (const bool text : {false, true}) {
    constexpr int W = 512, H = 384, kFps = 30;
    auto [host_end, viewer_end] = transport::MakeLocalPipe();
    session::HostSession host(host_end.get(), std::make_unique<FakeEncoder>(), W, H,
                              {.text_mode = text, .fps = kFps, .bitrate_bps = 20'000'000});
    session::ViewerSession viewer(nullptr, std::make_unique<FakeDecoder>());
    Shown shown;
    shown.Attach(viewer);
    viewer.Attach(viewer_end.get());
    std::vector<uint8_t> page(static_cast<size_t>(W) * (H + 7) * 4);
    uint32_t seed = 3;
    FillNoise(page, W, {0, 0, W, H + 7}, seed);
    // Blank: the tile at (2,2) and the 7 rows that scroll into it.
    for (int y = 128; y < 192 + 7; ++y) {
      for (int x = 128; x < 192; ++x) {
        const uint8_t white[4] = {255, 255, 255, 255};
        std::memcpy(&page[(static_cast<size_t>(y) * W + x) * 4], white, 4);
      }
    }
    uint64_t t = 1'000'000;
    auto settle = [&](const uint8_t* px) {
      for (int f = 0; f < 60 * kFps && (f < 10 || host.work_pending()); ++f) {
        t += 1'000'000 / kFps;
        host.OnFrame({{px, W, H, W * 4}, {}, t});
      }
    };
    host.OnFrame({{page.data(), W, H, W * 4}, {}, t});
    settle(page.data());
    const uint8_t* scrolled = page.data() + static_cast<size_t>(7) * W * 4;
    t += 1'000'000 / kFps;
    host.OnFrame({{scrolled, W, H, W * 4}, {}, t});
    EXPECT_GT(host.stats().moved_tiles, 0u);
    settle(scrolled);
    const uint64_t frames = host.stats().frames_encoded;
    PipeStopper stop{host_end, viewer_end};
    const std::vector<uint8_t> want(scrolled, scrolled + static_cast<size_t>(W) * H * 4);
    const std::string bad = shown.Differences(want, W, H, frames);
    EXPECT_EQ(bad.find(" (2,2):"), std::string::npos) << "text mode " << text << ":" << bad;
  }
}

TEST(Session, TheEncoderSeesTheScreenJustOutsideWhatTheViewerTakesFromAPicture) {
  // Text on the left (lossless: one plain colour in the encoder's picture),
  // a photo on the right. When the photo changes, the two columns of text
  // pixels next to it are the screen's in the encoder's input, so that the
  // photo's outermost pixels get their colour from what is really beside
  // them; further away the text stays flattened.
  constexpr int W = 256, H = 128, kFps = 30;
  auto [host_end, viewer_end] = transport::MakeLocalPipe();
  std::vector<uint8_t> input;
  auto enc = std::make_unique<FakeEncoder>();
  enc->input = &input;
  session::HostSession host(host_end.get(), std::move(enc), W, H,
                            {.text_mode = true, .fps = kFps, .bitrate_bps = 20'000'000});
  std::vector<uint8_t> px(static_cast<size_t>(W) * H * 4, 255);
  uint32_t seed = 9;
  FillText(px, W, {0, 0, 128, H}, seed, 3);
  FillNoise(px, W, {128, 0, 128, H}, seed);
  uint64_t t = 1'000'000;
  host.OnFrame({{px.data(), W, H, W * 4}, {}, t});
  for (int f = 0; f < 60 * kFps && host.work_pending(); ++f) {
    t += 1'000'000 / kFps;
    host.OnFrame({{px.data(), W, H, W * 4}, {}, t});
  }
  FillNoise(px, W, {128, 0, 64, 64}, seed);  // the photo's top left tile
  t += 1'000'000 / kFps;
  input.clear();
  host.OnFrame({{px.data(), W, H, W * 4}, {}, t});
  ASSERT_EQ(input.size(), px.size());
  size_t beside = 0, further = 0;
  for (int y = 0; y < 64; ++y) {
    for (int x = 120; x < 128; ++x) {
      const size_t i = (static_cast<size_t>(y) * W + x) * 4;
      (x >= 126 ? beside : further) += std::memcmp(&input[i], &px[i], 3) != 0;
    }
  }
  EXPECT_EQ(beside, 0u);
  EXPECT_GT(further, 64u);  // still one colour there
  PipeStopper stop{host_end, viewer_end};
}
