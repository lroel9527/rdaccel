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

  std::string name() const override { return "fake"; }
  const std::string& last_error() const override { return err_; }
  void SetBitrate(int64_t) override {}
  std::optional<codec::HevcAccessUnit> Encode(const tile::ImageView& img, uint64_t,
                                              bool force_keyframe) override {
    err_.clear();
    keyframe_requests.push_back(force_keyframe);
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
    codec::AppendLengthPrefixed(nal, au.nal_data);
    return au;
  }

 private:
  std::string err_;
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
  EXPECT_EQ(regions[0], (Rect{0, 0, 192, 64}));
  EXPECT_EQ(regions[1], (Rect{320, 0, 64, 64}));
  EXPECT_EQ(regions[2], (Rect{0, 64, 64, 64}));
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
  EXPECT_EQ(got.back(), (std::vector<Rect>{{64, 64, 192, 64}, {64, 128, 192, 64}}));
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
