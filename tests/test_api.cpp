// Tests of the public API, written the way an application would use it:
// only <rdaccel/rdaccel.hpp>, real platform codecs.

#include <rdaccel/rdaccel.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr int32_t kW = 640, kH = 360;

double Psnr(const rdaccel::ImageView& a, const std::vector<uint8_t>& b) {
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

// Collects what a Viewer delivers.
struct Received {
  std::mutex mu;
  std::condition_variable cv;
  int32_t width = 0, height = 0;
  int frames = 0;
  double min_psnr = 1e9;
  std::vector<uint8_t> ref;

  rdaccel::ViewerHandlers Handlers() {
    rdaccel::ViewerHandlers h;
    h.on_stream_info = [this](int32_t w, int32_t ht) {
      std::lock_guard<std::mutex> lock(mu);
      width = w;
      height = ht;
    };
    h.on_frame = [this](const rdaccel::ImageView& img, std::span<const rdaccel::Rect>,
                        const rdaccel::FrameInfo& info) {
      std::lock_guard<std::mutex> lock(mu);
      ref.clear();
      rdaccel::RenderTestPattern(info.frame_id - 1, img.width, img.height, ref);
      min_psnr = std::min(min_psnr, Psnr(img, ref));
      ++frames;
      cv.notify_all();
    };
    return h;
  }

  bool WaitFor(int n) {
    std::unique_lock<std::mutex> lock(mu);
    return cv.wait_for(lock, std::chrono::seconds(30), [&] { return frames >= n; });
  }
};

}  // namespace

TEST(PublicApi, VersionAndPlatform) {
  EXPECT_STREQ(rdaccel::Version(), "0.2.0");
  EXPECT_NE(std::string(rdaccel::PlatformName()), "");
  const auto a = rdaccel::NowMicros();
  EXPECT_GE(rdaccel::NowMicros(), a);
}

TEST(PublicApi, HostToViewerOverLocalPipe) {
  auto [host_end, viewer_end] = rdaccel::CreateLocalPipe();
  Received got;
  std::string err;
  auto viewer = rdaccel::Viewer::Create(std::move(viewer_end), got.Handlers(), &err);
  ASSERT_TRUE(viewer) << err;

  rdaccel::HostConfig cfg;
  cfg.fps = 30;
  cfg.bitrate_bps = 4'000'000;
  rdaccel::Host host(std::move(host_end), cfg);
  constexpr int kFrames = 20;
  std::vector<uint8_t> px;
  for (int i = 0; i < kFrames; ++i) {
    const auto dirty = rdaccel::RenderTestPattern(i, kW, kH, px);
    ASSERT_TRUE(host.SubmitFrame({px.data(), kW, kH, kW * 4}, rdaccel::NowMicros(), dirty, &err))
        << err;
  }
  ASSERT_TRUE(got.WaitFor(kFrames));
  EXPECT_FALSE(host.encoder_name().empty());
  EXPECT_FALSE(viewer->decoder_name().empty());
  std::lock_guard<std::mutex> lock(got.mu);
  EXPECT_EQ(got.width, kW);
  EXPECT_EQ(got.height, kH);
  EXPECT_GT(got.min_psnr, 30.0);
  EXPECT_EQ(viewer->stats().decode_errors, 0u);
  EXPECT_EQ(host.stats().frames_encoded, static_cast<uint64_t>(kFrames));
}

TEST(PublicApi, StaticScreenIsSkipped) {
  auto [host_end, viewer_end] = rdaccel::CreateLocalPipe();
  Received got;
  auto viewer = rdaccel::Viewer::Create(std::move(viewer_end), got.Handlers());
  ASSERT_TRUE(viewer);
  rdaccel::HostConfig cfg;
  rdaccel::Host host(std::move(host_end), cfg);
  std::vector<uint8_t> px;
  rdaccel::RenderTestPattern(0, kW, kH, px);
  // After the first frame come the progressive tiles, one sharper picture of
  // what stays video, and `refine_frames` refinements of it; after that,
  // nothing more is sent.
  const int n = cfg.refine_frames + 40;
  for (int i = 0; i < n; ++i) {
    ASSERT_TRUE(host.SubmitFrame({px.data(), kW, kH, kW * 4}, rdaccel::NowMicros()));
  }
  ASSERT_TRUE(got.WaitFor(1));
  const auto st = host.stats();
  EXPECT_LE(st.frames_encoded, 2u + static_cast<uint64_t>(cfg.refine_frames) + st.progressive_tiles);
  EXPECT_EQ(st.frames_encoded + st.frames_skipped, static_cast<uint64_t>(n));
  const uint64_t sent = st.bytes_sent;
  for (int i = 0; i < 10; ++i) {
    ASSERT_TRUE(host.SubmitFrame({px.data(), kW, kH, kW * 4}, rdaccel::NowMicros()));
  }
  EXPECT_EQ(host.stats().bytes_sent, sent);
}

TEST(PublicApi, RejectsSizeChangeAndBadImages) {
  auto [host_end, viewer_end] = rdaccel::CreateLocalPipe();
  rdaccel::Host host(std::move(host_end));
  std::vector<uint8_t> px;
  rdaccel::RenderTestPattern(0, kW, kH, px);
  std::string err;
  EXPECT_FALSE(host.SubmitFrame({nullptr, kW, kH, kW * 4}, 0, {}, &err));
  EXPECT_FALSE(host.SubmitFrame({px.data(), kW, kH, kW}, 0, {}, &err));  // stride too small
  ASSERT_TRUE(host.SubmitFrame({px.data(), kW, kH, kW * 4}, 0, {}, &err)) << err;
  int errors = 0;
  host.SetErrorHandler([&](const std::string&) { ++errors; });
  EXPECT_FALSE(host.SubmitFrame({px.data(), kW / 2, kH / 2, kW * 4}, 1, {}, &err));
  EXPECT_NE(err.find("size changed"), std::string::npos);
  EXPECT_FALSE(host.SubmitFrame({px.data(), kW / 2, kH / 2, kW * 4}, 2, {}, &err));
  EXPECT_EQ(errors, 1);  // reported once, not per frame
}

TEST(PublicApi, RecordingRoundTrip) {
  const std::string path = ::testing::TempDir() + "rdaccel_api_test.rdstream";
  std::string err;
  constexpr int kFrames = 15;
  {
    auto file = rdaccel::CreateRecordingTransport(path, &err);
    ASSERT_TRUE(file) << err;
    rdaccel::Host host(std::move(file));
    std::vector<uint8_t> px;
    for (int i = 0; i < kFrames; ++i) {
      const auto dirty = rdaccel::RenderTestPattern(i, kW, kH, px);
      ASSERT_TRUE(host.SubmitFrame({px.data(), kW, kH, kW * 4}, i * 33'333ull, dirty, &err))
          << err;
    }
  }  // destroying the host closes the file

  std::vector<uint8_t> bytes;
  ASSERT_TRUE(rdaccel::ReadRecording(path, bytes, &err)) << err;
  Received got;
  auto viewer = rdaccel::Viewer::Create(nullptr, got.Handlers(), &err);
  ASSERT_TRUE(viewer) << err;
  viewer->FeedRecording(bytes);
  EXPECT_EQ(got.frames, kFrames);
  EXPECT_EQ(got.width, kW);
  EXPECT_GT(got.min_psnr, 30.0);
  std::remove(path.c_str());

  EXPECT_FALSE(rdaccel::ReadRecording(path, bytes, &err));
}

TEST(PublicApi, CustomTransportInterface) {
  // An application-provided transport that just counts what it is given.
  struct Counting final : rdaccel::Transport {
    size_t bytes = 0;
    bool Send(rdaccel::Channel, std::span<const uint8_t> b) override {
      bytes += b.size();
      return true;
    }
    void SetReceiver(ReceiveFn) override {}
    void Close() override {}
  };
  auto t = std::make_unique<Counting>();
  Counting* raw = t.get();
  rdaccel::Host host(std::move(t));
  std::vector<uint8_t> px;
  rdaccel::RenderTestPattern(0, kW, kH, px);
  ASSERT_TRUE(host.SubmitFrame({px.data(), kW, kH, kW * 4}, 0));
  EXPECT_EQ(raw->bytes, host.stats().bytes_sent);
  EXPECT_GT(raw->bytes, 0u);
}

// A "video" playing in part of the screen gets its own stream (on macOS a
// second VideoToolbox session), and the viewer composes both.
TEST(PublicApi, VideoAreaGetsItsOwnStream) {
  constexpr int32_t W = 640, H = 384;
  const rdaccel::Rect area{128, 64, 256, 192};
  std::vector<uint8_t> px(static_cast<size_t>(W) * H * 4, 200);
  auto render = [&](int i) {
    // Smooth, moving gradients: too many colours for the lossless layer, and
    // something an encoder can actually code well.
    for (int y = area.y; y < area.bottom(); ++y) {
      for (int x = area.x; x < area.right(); ++x) {
        uint8_t* p = &px[(static_cast<size_t>(y) * W + x) * 4];
        p[0] = static_cast<uint8_t>(x * 2 + i * 3);
        p[1] = static_cast<uint8_t>(y * 3 - i * 2);
        p[2] = static_cast<uint8_t>((x + y) + i * 5);
        p[3] = 255;
      }
    }
  };

  auto [host_end, viewer_end] = rdaccel::CreateLocalPipe();
  std::mutex mu;
  std::condition_variable cv;
  int frames = 0;
  double last_psnr = 0;
  std::vector<uint8_t> expected;
  rdaccel::ViewerHandlers h;
  h.on_frame = [&](const rdaccel::ImageView& img, std::span<const rdaccel::Rect>,
                   const rdaccel::FrameInfo&) {
    std::lock_guard<std::mutex> lock(mu);
    last_psnr = Psnr(img, expected);
    ++frames;
    cv.notify_all();
  };
  std::string err;
  auto viewer = rdaccel::Viewer::Create(std::move(viewer_end), std::move(h), &err);
  ASSERT_TRUE(viewer) << err;
  rdaccel::HostConfig cfg;
  cfg.fps = 30;
  cfg.bitrate_bps = 4'000'000;
  rdaccel::Host host(std::move(host_end), cfg);
  for (int i = 0; i < 60; ++i) {
    render(i);
    {
      std::lock_guard<std::mutex> lock(mu);
      expected = px;
    }
    ASSERT_TRUE(host.SubmitFrame({px.data(), W, H, W * 4}, i * 33'333ull, {}, &err)) << err;
    std::unique_lock<std::mutex> lock(mu);
    // One frame at a time, so `expected` matches what is being shown.
    ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(30), [&] { return frames == i + 1; }));
    if (i > 40) {
      EXPECT_GT(last_psnr, 30.0) << "frame " << i;
    }
  }
  const auto st = host.stats();
  EXPECT_EQ(st.regions_opened, 1u);
  EXPECT_GT(st.region_pictures, 20u);
  EXPECT_EQ(viewer->stats().decode_errors, 0u);
}

// Host and Viewer over a slow simulated network with the UDP transport's
// protocol: the host learns the path and keeps latency bounded.
TEST(PublicApi, AdaptsToASlowNetwork) {
  rdaccel::NetworkConditions down;
  down.bandwidth_bps = 1'000'000;
  down.delay_ms = 15;
  down.loss = 0.01;
  auto [host_end, viewer_end] = rdaccel::CreateSimulatedNetwork(down, {.delay_ms = 15});
  std::mutex mu;
  int frames = 0;
  uint64_t worst_late_us = 0;
  rdaccel::ViewerHandlers h;
  h.on_frame = [&](const rdaccel::ImageView&, std::span<const rdaccel::Rect>,
                   const rdaccel::FrameInfo& info) {
    std::lock_guard<std::mutex> lock(mu);
    ++frames;
    // Ignore the first second (the first, full picture takes a while).
    if (frames > 10) worst_late_us = std::max(worst_late_us, rdaccel::NowMicros() - info.capture_time_us);
  };
  std::string err;
  auto viewer = rdaccel::Viewer::Create(std::move(viewer_end), std::move(h), &err);
  ASSERT_TRUE(viewer) << err;
  rdaccel::HostConfig cfg;
  cfg.fps = 30;
  cfg.bitrate_bps = 8'000'000;
  rdaccel::Host host(std::move(host_end), cfg);
  std::vector<uint8_t> px;
  const uint64_t t0 = rdaccel::NowMicros();
  for (int i = 0; i < 90; ++i) {
    while (rdaccel::NowMicros() < t0 + i * 33'333ull) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    rdaccel::RenderTestPattern(i, 640, 360, px);
    ASSERT_TRUE(host.SubmitFrame({px.data(), 640, 360, 640 * 4}, rdaccel::NowMicros(), {}, &err)) << err;
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  const auto st = host.stats();
  EXPECT_GT(st.bandwidth_estimate_bps, 0);  // a lower bound while little is sent
  EXPECT_LT(st.bandwidth_estimate_bps, 1'500'000);
  EXPECT_GT(st.rtt_us, 25'000u);
  EXPECT_GT(st.target_bitrate_bps, 0);
  std::lock_guard<std::mutex> lock(mu);
  EXPECT_GT(frames, 30);
  EXPECT_LT(worst_late_us, 1'000'000u);
}

TEST(PublicApi, CursorTravelsOnItsOwnChannel) {
  auto [host_end, viewer_end] = rdaccel::CreateLocalPipe();
  std::mutex mu;
  std::condition_variable cv;
  std::vector<rdaccel::CursorState> states;
  std::vector<rdaccel::CursorImage> images;
  rdaccel::ViewerHandlers h;
  h.on_cursor = [&](const rdaccel::CursorState& s) {
    std::lock_guard<std::mutex> lock(mu);
    states.push_back(s);
    images.push_back(s.image ? *s.image : rdaccel::CursorImage{});
    cv.notify_all();
  };
  std::string err;
  auto viewer = rdaccel::Viewer::Create(std::move(viewer_end), std::move(h), &err);
  ASSERT_TRUE(viewer) << err;
  rdaccel::Host host(std::move(host_end));

  rdaccel::CursorImage img;
  img.width = img.height = 16;
  img.hotspot_x = 3;
  img.bgra.assign(16 * 16 * 4, 0);
  for (size_t i = 0; i < 16 * 4; ++i) img.bgra[i] = 255;  // top row opaque white
  // No frame needed: the cursor works before the first picture.
  host.SetCursorImage(img);
  host.SetCursor(100, 50);
  host.SetCursor(100, 50);  // unchanged
  host.SetCursor(101, 52, true, 777);
  {
    std::unique_lock<std::mutex> lock(mu);
    ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(10), [&] { return states.size() >= 2; }));
    EXPECT_EQ(states[0].x, 100);
    EXPECT_TRUE(states[0].image_changed);
    EXPECT_EQ(images[0], img);
    EXPECT_EQ(states[1].x, 101);
    EXPECT_EQ(states[1].y, 52);
    EXPECT_EQ(states[1].time_us, 777u);
    EXPECT_FALSE(states[1].image_changed);
    EXPECT_EQ(states[1].image_id, states[0].image_id);
  }
  const auto st = host.stats();
  EXPECT_EQ(st.cursor_updates, 2u);
  EXPECT_EQ(st.cursor_images, 1u);
  EXPECT_GT(st.cursor_bytes, 0u);
  EXPECT_LT(st.cursor_bytes, 200u);

  // Drawing it on a frame.
  std::vector<uint8_t> px(kW * kH * 4, 0);
  rdaccel::DrawCursor(img, 101, 52, px.data(), kW, kH, kW * 4);
  EXPECT_EQ(px[(52 * kW + 98) * 4], 255);  // hotspot (3, 0) lands on (101, 52)
  EXPECT_EQ(px[(52 * kW + 97) * 4], 0);
}

TEST(PublicApi, RecordingKeepsTheCursor) {
  const std::string path = ::testing::TempDir() + "rdaccel_cursor_test.rdstream";
  std::string err;
  {
    auto file = rdaccel::CreateRecordingTransport(path, &err);
    ASSERT_TRUE(file) << err;
    rdaccel::Host host(std::move(file));
    std::vector<uint8_t> px;
    rdaccel::RenderTestPattern(0, kW, kH, px);
    ASSERT_TRUE(host.SubmitFrame({px.data(), kW, kH, kW * 4}, 1, {}, &err)) << err;
    rdaccel::CursorImage img;
    img.width = img.height = 4;
    img.bgra.assign(64, 255);
    host.SetCursorImage(img);
    host.SetCursor(5, 6);
  }
  std::vector<uint8_t> bytes;
  ASSERT_TRUE(rdaccel::ReadRecording(path, bytes, &err)) << err;
  int cursors = 0;
  rdaccel::ViewerHandlers h;
  h.on_cursor = [&](const rdaccel::CursorState& s) {
    ++cursors;
    EXPECT_EQ(s.x, 5);
    EXPECT_TRUE(s.image);
  };
  auto viewer = rdaccel::Viewer::Create(nullptr, std::move(h), &err);
  ASSERT_TRUE(viewer) << err;
  viewer->FeedRecording(bytes);
  EXPECT_EQ(cursors, 1);
  std::remove(path.c_str());
}

TEST(PublicApi, ViewerCanAskForAKeyframe) {
  auto [host_end, viewer_end] = rdaccel::CreateLocalPipe();
  Received got;
  std::string err;
  auto viewer = rdaccel::Viewer::Create(std::move(viewer_end), got.Handlers(), &err);
  ASSERT_TRUE(viewer) << err;
  rdaccel::Host host(std::move(host_end));
  std::vector<uint8_t> px;
  for (int i = 0; i < 3; ++i) {
    const auto dirty = rdaccel::RenderTestPattern(i, kW, kH, px);
    ASSERT_TRUE(host.SubmitFrame({px.data(), kW, kH, kW * 4}, rdaccel::NowMicros(), dirty, &err)) << err;
    if (i == 1) {
      ASSERT_TRUE(got.WaitFor(2));
      viewer->RequestKeyframe();
      for (int k = 0; k < 500 && host.stats().keyframe_requests == 0; ++k) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
    }
  }
  ASSERT_TRUE(got.WaitFor(3));
  EXPECT_EQ(viewer->stats().keyframe_requests, 1u);
  EXPECT_EQ(host.stats().keyframe_requests, 1u);
  EXPECT_EQ(host.stats().keyframes, 2u);
}

TEST(PublicApi, KeyframeComesEvenWhenTheScreenIsStatic) {
  // Sources such as ScreenCaptureKit deliver no frames while nothing
  // changes; a keyframe the viewer asks for must not wait for a change.
  auto [host_end, viewer_end] = rdaccel::CreateLocalPipe();
  Received got;
  std::string err;
  auto viewer = rdaccel::Viewer::Create(std::move(viewer_end), got.Handlers(), &err);
  ASSERT_TRUE(viewer) << err;
  rdaccel::Host host(std::move(host_end));
  std::vector<uint8_t> px;
  rdaccel::RenderTestPattern(0, kW, kH, px);
  ASSERT_TRUE(host.SubmitFrame({px.data(), kW, kH, kW * 4}, rdaccel::NowMicros(), {}, &err)) << err;
  ASSERT_TRUE(got.WaitFor(1));
  viewer->RequestKeyframe();  // and no more frames are submitted
  ASSERT_TRUE(got.WaitFor(2));
  const auto st = host.stats();
  EXPECT_EQ(st.keyframes, 2u);
  EXPECT_EQ(st.frames_resubmitted, 1u);
  // Nothing more is owed: no further frames.
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  EXPECT_EQ(host.stats().frames_resubmitted, 1u);
  EXPECT_EQ(host.stats().frames_encoded, st.frames_encoded);
}
