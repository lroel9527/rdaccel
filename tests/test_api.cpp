// Tests of the public API, written the way an application would use it:
// only <rdaccel/rdaccel.hpp>, real platform codecs.

#include <rdaccel/rdaccel.hpp>

#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <functional>
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
  EXPECT_STREQ(rdaccel::Version(), "1.9.4");
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

// Both handler forms (and clearing it) compile and are accepted: code
// written against the string-only form keeps building.
TEST(PublicApi, ErrorHandlerForms) {
  auto [host_end, viewer_end] = rdaccel::CreateLocalPipe();
  rdaccel::Host host(std::move(host_end), rdaccel::HostConfig{});
  ASSERT_TRUE(host.ok());
  host.SetErrorHandler([](const std::string&) {});
  host.SetErrorHandler([](rda_status, const std::string&) {});
  rdaccel::Host::ErrorHandler old_style = [](const std::string&) {};
  host.SetErrorHandler(old_style);
  host.SetErrorHandler(nullptr);
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
  // what stays video, `refine_frames` refinements of it and, the screen
  // being still, 60 finer pictures of it (the still-screen polish); after
  // that, nothing more is sent.
  constexpr int kStillFrames = 60;
  const int n = cfg.refine_frames + kStillFrames + 40;
  for (int i = 0; i < n; ++i) {
    ASSERT_TRUE(host.SubmitFrame({px.data(), kW, kH, kW * 4}, rdaccel::NowMicros()));
  }
  ASSERT_TRUE(got.WaitFor(1));
  const auto st = host.stats();
  EXPECT_LE(st.frames_encoded,
            2u + static_cast<uint64_t>(cfg.refine_frames) + kStillFrames + 8u + st.progressive_tiles);
  EXPECT_EQ(st.frames_encoded + st.frames_skipped, static_cast<uint64_t>(n));
  const uint64_t sent = st.bytes_sent;
  for (int i = 0; i < 10; ++i) {
    ASSERT_TRUE(host.SubmitFrame({px.data(), kW, kH, kW * 4}, rdaccel::NowMicros()));
  }
  EXPECT_EQ(host.stats().bytes_sent, sent);
}

TEST(PublicApi, RejectsBadImages) {
  auto [host_end, viewer_end] = rdaccel::CreateLocalPipe();
  rdaccel::Host host(std::move(host_end));
  std::vector<uint8_t> px;
  rdaccel::RenderTestPattern(0, kW, kH, px);
  std::string err;
  EXPECT_FALSE(host.SubmitFrame({nullptr, kW, kH, kW * 4}, 0, {}, &err));
  EXPECT_FALSE(host.SubmitFrame({px.data(), kW, kH, kW}, 0, {}, &err));  // stride too small
  EXPECT_FALSE(host.SubmitFrame({px.data(), kW + 1, kH, kW * 4 + 4}, 0, {}, &err));  // odd size
  EXPECT_TRUE(host.SubmitFrame({px.data(), kW, kH, kW * 4}, 0, {}, &err)) << err;
}

TEST(PublicApi, ResolutionChangesMidStream) {
  auto [host_end, viewer_end] = rdaccel::CreateLocalPipe();
  std::mutex mu;
  std::vector<std::pair<int32_t, int32_t>> infos, frame_sizes;
  double min_psnr = 1e9;
  std::vector<uint8_t> ref;
  rdaccel::ViewerHandlers h;
  h.on_stream_info = [&](int32_t w, int32_t ht) {
    std::lock_guard<std::mutex> lock(mu);
    infos.emplace_back(w, ht);
  };
  h.on_frame = [&](const rdaccel::ImageView& img, std::span<const rdaccel::Rect>, const rdaccel::FrameInfo& info) {
    std::lock_guard<std::mutex> lock(mu);
    frame_sizes.emplace_back(img.width, img.height);
    ref.clear();
    rdaccel::RenderTestPattern(info.frame_id - 1, img.width, img.height, ref);
    (void)info;
    min_psnr = std::min(min_psnr, Psnr(img, ref));
  };
  std::string err;
  auto viewer = rdaccel::Viewer::Create(std::move(viewer_end), std::move(h), &err);
  ASSERT_NE(viewer, nullptr) << err;
  rdaccel::HostConfig cfg;
  cfg.progressive = false;
  rdaccel::Host host(std::move(host_end), cfg);
  // 640x360, then 320x180, then back: frame i shows test pattern i at the
  // current size, rendered from scratch.
  const std::pair<int32_t, int32_t> sizes[] = {{kW, kH}, {kW / 2, kH / 2}, {kW, kH}};
  uint64_t index = 0;
  for (const auto& [w, ht] : sizes) {
    for (int k = 0; k < 6; ++k, ++index) {
      std::vector<uint8_t> px;
      rdaccel::RenderTestPattern(index, w, ht, px);
      ASSERT_TRUE(host.SubmitFrame({px.data(), w, ht, w * 4}, rdaccel::NowMicros(), {}, &err)) << err;
    }
  }
  for (int i = 0; i < 2000; ++i) {
    {
      std::lock_guard<std::mutex> lock(mu);
      if (frame_sizes.size() >= 18) break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  std::lock_guard<std::mutex> lock(mu);
  ASSERT_EQ(frame_sizes.size(), 18u);
  for (size_t i = 0; i < 18; ++i) EXPECT_EQ(frame_sizes[i], sizes[i / 6]) << i;
  ASSERT_EQ(infos.size(), 3u);
  EXPECT_EQ(infos[1], sizes[1]);
  EXPECT_GT(min_psnr, 30.0);
  EXPECT_EQ(host.stats().resolution_changes, 2u);
  EXPECT_EQ(viewer->stats().decode_errors, 0u);
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

TEST(PublicApi, ConnectionAndMessages) {
  auto [host_t, viewer_t] = rdaccel::CreateLocalPipe();
  rdaccel::HostConfig cfg;
  cfg.auth = [](std::span<const uint8_t> token) { return token.size() == 2; };
  rdaccel::Host host(std::move(host_t), cfg);
  ASSERT_TRUE(host.ok());
  std::mutex mu;
  std::vector<std::string> host_got, viewer_got;
  host.SetMessageHandler([&](uint32_t type, std::span<const uint8_t> p) {
    std::lock_guard<std::mutex> lock(mu);
    host_got.push_back(std::to_string(type) + ":" + std::string(p.begin(), p.end()));
  });
  std::atomic<int> viewer_state{-1};
  rdaccel::ViewerHandlers h;
  h.on_state = [&](rdaccel::ConnectionState s, rda_status) { viewer_state = static_cast<int>(s); };
  h.on_message = [&](uint32_t type, std::span<const uint8_t> p) {
    std::lock_guard<std::mutex> lock(mu);
    viewer_got.push_back(std::to_string(type) + ":" + std::string(p.begin(), p.end()));
  };
  rdaccel::ViewerOptions opts;
  opts.auth_token = {1, 2};
  std::string err;
  auto viewer = rdaccel::Viewer::Create(std::move(viewer_t), std::move(h), opts, &err);
  ASSERT_NE(viewer, nullptr) << err;
  auto wait = [](const std::function<bool()>& f) {
    for (int i = 0; i < 2000 && !f(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    return f();
  };
  ASSERT_TRUE(wait([&] { return viewer->state() == rdaccel::ConnectionState::kConnected; }));
  // The state changes before its callback runs.
  EXPECT_TRUE(wait([&] { return viewer_state.load() == static_cast<int>(rdaccel::ConnectionState::kConnected); }));
  EXPECT_EQ(host.state(), rdaccel::ConnectionState::kConnected);
  ASSERT_TRUE(viewer->SendMessage(rdaccel::kClipboardTextMessage, std::string_view("copy"), &err)) << err;
  ASSERT_TRUE(host.SendMessage(rdaccel::kFirstUserMessage, std::string_view("ping"), &err)) << err;
  EXPECT_FALSE(host.SendMessage(5, std::string_view("x"), &err));
  ASSERT_TRUE(wait([&] {
    std::lock_guard<std::mutex> lock(mu);
    return host_got.size() == 1 && viewer_got.size() == 1;
  }));
  EXPECT_EQ(host_got[0], "1:copy");
  EXPECT_EQ(viewer_got[0], "65536:ping");

  // Frames still flow alongside messages.
  std::vector<uint8_t> px;
  rdaccel::RenderTestPattern(0, kW, kH, px);
  EXPECT_TRUE(host.SubmitFrame({px.data(), kW, kH, kW * 4}, rdaccel::NowMicros(), {}, &err)) << err;
  ASSERT_TRUE(wait([&] { return viewer->stats().frames_decoded >= 1; }));

  viewer.reset();
  ASSERT_TRUE(wait([&] { return host.state() == rdaccel::ConnectionState::kDisconnected; }));
  EXPECT_FALSE(host.SubmitFrame({px.data(), kW, kH, kW * 4}, rdaccel::NowMicros(), {}, &err));
  EXPECT_NE(err.find("disconnected"), std::string::npos) << err;
}

TEST(PublicApi, TextInputReachesTheHost) {
  auto [host_t, viewer_t] = rdaccel::CreateLocalPipe();
  rdaccel::Host host(std::move(host_t));
  ASSERT_TRUE(host.ok());
  std::mutex mu;
  std::vector<rdaccel::InputEvent> got;
  host.SetInputHandler([&](const rdaccel::InputEvent& ev) {
    std::lock_guard<std::mutex> lock(mu);
    got.push_back(ev);
  });
  std::string err;
  auto viewer = rdaccel::Viewer::Create(std::move(viewer_t), {}, &err);
  ASSERT_NE(viewer, nullptr) << err;
  auto wait = [](const std::function<bool()>& f) {
    for (int i = 0; i < 2000 && !f(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    return f();
  };
  ASSERT_TRUE(wait([&] { return viewer->state() == rdaccel::ConnectionState::kConnected; }));
  EXPECT_FALSE(viewer->SendText("ab\xC3", &err));  // cut-off UTF-8: nothing sent
  EXPECT_FALSE(viewer->SendText("\xED\xA0\x80", &err));  // an encoded surrogate
  EXPECT_FALSE(viewer->SendText("\xC0\xAF", &err));  // overlong
  ASSERT_TRUE(viewer->SendText("a\xE4\xB8\xAD\xF0\x9F\x98\x80", &err)) << err;  // a, 中, 😀
  ASSERT_TRUE(wait([&] {
    std::lock_guard<std::mutex> lock(mu);
    return got.size() == 3;
  }));
  std::lock_guard<std::mutex> lock(mu);
  for (const auto& ev : got) EXPECT_EQ(ev.kind, rdaccel::InputKind::kText);
  EXPECT_EQ(got[0].code, 0x61u);
  EXPECT_EQ(got[1].code, 0x4E2Du);
  EXPECT_EQ(got[2].code, 0x1F600u);
}

namespace {

// Submits frames until `viewer` has decoded `n` or time runs out.
bool StreamUntil(rdaccel::Host& host, rdaccel::Viewer& viewer, uint64_t n, int max_frames = 600) {
  std::vector<uint8_t> px;
  for (int i = 0; i < max_frames; ++i) {
    const auto dirty = rdaccel::RenderTestPattern(static_cast<uint64_t>(i), kW, kH, px);
    host.SubmitFrame({px.data(), kW, kH, kW * 4}, rdaccel::NowMicros(), dirty);
    if (viewer.stats().frames_decoded >= n) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
}

}  // namespace

TEST(PublicApi, UdpBindDoesNotBlockAndHoldsFramesUntilTheViewerComes) {
  std::string err;
  auto host_t = rdaccel::BindUdp(47311, &err);
  ASSERT_NE(host_t, nullptr) << err;
  // Encrypted, as real UDP use should be.
  const std::vector<uint8_t> key(32, 0x5a);
  rdaccel::HostConfig cfg;
  cfg.encryption_key = key;
  rdaccel::Host host(std::move(host_t), cfg);
  ASSERT_TRUE(host.ok());
  // Frames before any viewer: accepted, held, not queued.
  std::vector<uint8_t> px;
  for (int i = 0; i < 30; ++i) {
    const auto dirty = rdaccel::RenderTestPattern(static_cast<uint64_t>(i), kW, kH, px);
    ASSERT_TRUE(host.SubmitFrame({px.data(), kW, kH, kW * 4}, rdaccel::NowMicros(), dirty, &err)) << err;
  }
  EXPECT_EQ(host.stats().frames_encoded, 0u);
  EXPECT_EQ(host.state(), rdaccel::ConnectionState::kConnecting);

  rdaccel::ViewerOptions vo;
  vo.encryption_key = key;
  auto viewer = rdaccel::Viewer::Create(rdaccel::ConnectUdp("127.0.0.1", 47311, &err), {}, vo, &err);
  ASSERT_NE(viewer, nullptr) << err;
  ASSERT_TRUE(StreamUntil(host, *viewer, 5));
  EXPECT_EQ(host.state(), rdaccel::ConnectionState::kConnected);
  EXPECT_EQ(viewer->stats().decode_errors, 0u);
}

TEST(PublicApi, TcpBindDoesNotBlockAndHoldsFramesUntilTheViewerComes) {
  std::string err;
  auto host_t = rdaccel::BindTcp(47312, &err);
  ASSERT_NE(host_t, nullptr) << err;
  const std::vector<uint8_t> key(32, 0x6b);
  rdaccel::HostConfig cfg;
  cfg.encryption_key = key;
  rdaccel::Host host(std::move(host_t), cfg);
  ASSERT_TRUE(host.ok());
  std::vector<uint8_t> px;
  for (int i = 0; i < 30; ++i) {
    const auto dirty = rdaccel::RenderTestPattern(static_cast<uint64_t>(i), kW, kH, px);
    ASSERT_TRUE(host.SubmitFrame({px.data(), kW, kH, kW * 4}, rdaccel::NowMicros(), dirty, &err)) << err;
  }
  EXPECT_EQ(host.stats().frames_encoded, 0u);
  EXPECT_EQ(host.state(), rdaccel::ConnectionState::kConnecting);

  rdaccel::ViewerOptions vo;
  vo.encryption_key = key;
  auto viewer = rdaccel::Viewer::Create(rdaccel::ConnectTcp("127.0.0.1", 47312, &err), {}, vo, &err);
  ASSERT_NE(viewer, nullptr) << err;
  ASSERT_TRUE(StreamUntil(host, *viewer, 5));
  EXPECT_EQ(host.state(), rdaccel::ConnectionState::kConnected);
  EXPECT_EQ(viewer->stats().decode_errors, 0u);
}

TEST(PublicApi, UdpFromYourOwnSockets) {
  int fds[2];
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_DGRAM, 0, fds), 0);
  std::string err;
  EXPECT_EQ(rdaccel::UdpFromFd(-1, false, &err), nullptr);
  auto host_t = rdaccel::UdpFromFd(fds[0], false, &err);
  ASSERT_NE(host_t, nullptr) << err;
  auto viewer_t = rdaccel::UdpFromFd(fds[1], true, &err);
  ASSERT_NE(viewer_t, nullptr) << err;
  rdaccel::HostConfig cfg;
  cfg.allow_plaintext = true;
  rdaccel::Host host(std::move(host_t), cfg);
  rdaccel::ViewerOptions vo;
  vo.allow_plaintext = true;
  auto viewer = rdaccel::Viewer::Create(std::move(viewer_t), {}, vo, &err);
  ASSERT_NE(viewer, nullptr) << err;
  ASSERT_TRUE(StreamUntil(host, *viewer, 5));
  EXPECT_EQ(viewer->state(), rdaccel::ConnectionState::kConnected);
}

TEST(PublicApi, ViewerGivesUpWhenNoHostAnswers) {
  rdaccel::ViewerOptions opts;
  opts.peer_timeout_ms = 300;
  opts.allow_plaintext = true;
  std::atomic<int> reason{0};
  rdaccel::ViewerHandlers h;
  h.on_state = [&](rdaccel::ConnectionState s, rda_status r) {
    if (s == rdaccel::ConnectionState::kDisconnected) reason = r;
  };
  std::string err;
  auto viewer = rdaccel::Viewer::Create(rdaccel::ConnectUdp("127.0.0.1", 47312, &err), std::move(h), opts, &err);
  ASSERT_NE(viewer, nullptr) << err;
  for (int i = 0; i < 400 && viewer->state() != rdaccel::ConnectionState::kDisconnected; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  EXPECT_EQ(viewer->state(), rdaccel::ConnectionState::kDisconnected);
  EXPECT_EQ(reason.load(), RDA_ERR_TIMEOUT);
}

TEST(PublicApi, NetworkTransportsNeedAKeyOrExplicitPlaintext) {
  std::string err;
  rdaccel::Host host(rdaccel::BindUdp(47313, &err));
  EXPECT_FALSE(host.ok());
  EXPECT_FALSE(host.SubmitFrame({nullptr, 0, 0, 0}, 0, {}, &err));
  EXPECT_NE(err.find("allow_plaintext"), std::string::npos) << err;
  auto viewer = rdaccel::Viewer::Create(rdaccel::ConnectUdp("127.0.0.1", 47313, &err), {}, &err);
  EXPECT_EQ(viewer, nullptr);
  rdaccel::HostConfig bad;
  bad.encryption_key = {1, 2, 3};
  rdaccel::Host host2(rdaccel::BindUdp(47314, &err), bad);
  EXPECT_FALSE(host2.ok());
}

TEST(PublicApi, WrongKeyDisconnectsBothSides) {
  std::string err;
  rdaccel::HostConfig cfg;
  cfg.encryption_key = std::vector<uint8_t>(32, 1);
  std::atomic<int> host_reason{0}, viewer_reason{0};
  rdaccel::Host host(rdaccel::BindUdp(47315, &err), cfg);
  ASSERT_TRUE(host.ok()) << err;
  host.SetStateHandler([&](rdaccel::ConnectionState s, rda_status r) {
    if (s == rdaccel::ConnectionState::kDisconnected) host_reason = r;
  });
  rdaccel::ViewerOptions vo;
  vo.encryption_key = std::vector<uint8_t>(32, 2);
  rdaccel::ViewerHandlers h;
  h.on_state = [&](rdaccel::ConnectionState s, rda_status r) {
    if (s == rdaccel::ConnectionState::kDisconnected) viewer_reason = r;
  };
  auto viewer = rdaccel::Viewer::Create(rdaccel::ConnectUdp("127.0.0.1", 47315, &err), std::move(h), vo, &err);
  ASSERT_NE(viewer, nullptr) << err;
  for (int i = 0; i < 1000 && (host_reason == 0 || viewer_reason == 0); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  EXPECT_EQ(viewer_reason.load(), RDA_ERR_AUTH);
  EXPECT_EQ(host_reason.load(), RDA_ERR_AUTH);
  EXPECT_EQ(viewer->stats().frames_decoded, 0u);
}

// ---- one viewer after another (HostConfig::keep_serving) --------------------

namespace {

// The host's state changes, in order.
struct StateLog {
  std::mutex mu;
  std::vector<std::pair<rdaccel::ConnectionState, rda_status>> seen;
  void Attach(rdaccel::Host& host) {
    host.SetStateHandler([this](rdaccel::ConnectionState s, rda_status r) {
      std::lock_guard<std::mutex> l(mu);
      seen.emplace_back(s, r);
    });
  }
  // Waits until the host has gone back to waiting after `disconnects`
  // disconnects; returns the reason of the last one (0 on time out).
  int WaitWaiting(rdaccel::Host& host, size_t disconnects) {
    for (int i = 0; i < 1000; ++i) {
      {
        std::lock_guard<std::mutex> l(mu);
        size_t d = 0;
        int last = 0;
        for (auto& [s, r] : seen) {
          if (s == rdaccel::ConnectionState::kDisconnected) {
            ++d;
            last = r;
          }
        }
        if (d >= disconnects && !seen.empty() && seen.back().first == rdaccel::ConnectionState::kConnecting &&
            host.state() == rdaccel::ConnectionState::kConnecting) {
          return last;
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return 0;
  }
};

std::unique_ptr<rdaccel::Viewer> Connect(bool tcp, uint16_t port, std::vector<uint8_t> key,
                                         std::vector<uint8_t> token = {}) {
  std::string err;
  rdaccel::ViewerOptions vo;
  vo.allow_plaintext = key.empty();
  vo.encryption_key = std::move(key);
  vo.auth_token = std::move(token);
  auto t = tcp ? rdaccel::ConnectTcp("127.0.0.1", port, &err) : rdaccel::ConnectUdp("127.0.0.1", port, &err);
  return rdaccel::Viewer::Create(std::move(t), {}, vo, &err);
}

void ServesViewersOneAfterAnother(bool tcp, uint16_t port) {
  std::string err;
  const std::vector<uint8_t> key(32, 0x3c);
  rdaccel::HostConfig cfg;
  cfg.encryption_key = key;
  rdaccel::Host host(tcp ? rdaccel::BindTcp(port, &err) : rdaccel::BindUdp(port, &err), cfg);
  ASSERT_TRUE(host.ok()) << err;
  StateLog log;
  log.Attach(host);
  uint64_t encoded = 0;
  for (int round = 0; round < 3; ++round) {
    auto viewer = Connect(tcp, port, key);
    ASSERT_NE(viewer, nullptr);
    ASSERT_TRUE(StreamUntil(host, *viewer, 5)) << "viewer " << round;
    EXPECT_EQ(host.state(), rdaccel::ConnectionState::kConnected);
    EXPECT_EQ(viewer->stats().decode_errors, 0u) << "viewer " << round;
    EXPECT_GT(host.stats().frames_encoded, encoded) << "stats go on counting";
    encoded = host.stats().frames_encoded;
    viewer.reset();  // leaves with a Goodbye
    EXPECT_EQ(log.WaitWaiting(host, static_cast<size_t>(round) + 1), RDA_ERR_CLOSED) << "viewer " << round;
  }
}

}  // namespace

TEST(PublicApi, UdpHostServesViewersOneAfterAnother) { ServesViewersOneAfterAnother(false, 47321); }
TEST(PublicApi, TcpHostServesViewersOneAfterAnother) { ServesViewersOneAfterAnother(true, 47322); }

TEST(PublicApi, WrongKeyOrRefusedViewerDoesNotEndTheHost) {
  std::string err;
  const std::vector<uint8_t> key(32, 0x4d);
  rdaccel::HostConfig cfg;
  cfg.encryption_key = key;
  cfg.auth = [](std::span<const uint8_t> t) { return std::string(t.begin(), t.end()) == "1234"; };
  rdaccel::Host host(rdaccel::BindUdp(47323, &err), cfg);
  ASSERT_TRUE(host.ok()) << err;
  StateLog log;
  log.Attach(host);
  {
    auto wrong_key = Connect(false, 47323, std::vector<uint8_t>(32, 0x4e), {'1', '2', '3', '4'});
    ASSERT_NE(wrong_key, nullptr);
    EXPECT_EQ(log.WaitWaiting(host, 1), RDA_ERR_AUTH);
  }
  {
    auto refused = Connect(false, 47323, key, {'0', '0', '0', '0'});
    ASSERT_NE(refused, nullptr);
    EXPECT_EQ(log.WaitWaiting(host, 2), RDA_ERR_AUTH);
    EXPECT_EQ(refused->stats().frames_decoded, 0u);
  }
  auto good = Connect(false, 47323, key, {'1', '2', '3', '4'});
  ASSERT_NE(good, nullptr);
  ASSERT_TRUE(StreamUntil(host, *good, 5));
  EXPECT_EQ(good->stats().decode_errors, 0u);
}

TEST(PublicApi, StraySendersDoNotTakeTheViewersPlace) {
  std::string err;
  rdaccel::HostConfig cfg;
  cfg.allow_plaintext = true;
  cfg.peer_timeout_ms = 400;
  rdaccel::Host host(rdaccel::BindUdp(47324, &err), cfg);
  ASSERT_TRUE(host.ok()) << err;
  StateLog log;
  log.Attach(host);
  auto send_raw = [](std::vector<uint8_t> bytes) {
    const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(47324);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ::sendto(fd, bytes.data(), bytes.size(), 0, reinterpret_cast<sockaddr*>(&a), sizeof(a));
    ::close(fd);
  };
  // Not this protocol at all (a port scan): ignored at once.
  send_raw({'G', 'E', 'T', ' ', '/'});
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  {
    auto viewer = Connect(false, 47324, {});
    ASSERT_NE(viewer, nullptr);
    ASSERT_TRUE(StreamUntil(host, *viewer, 3));
  }
  ASSERT_EQ(log.WaitWaiting(host, 1), RDA_ERR_CLOSED);
  // Looks like the protocol but never says hello: dropped after the timeout.
  send_raw({3});
  EXPECT_EQ(log.WaitWaiting(host, 2), RDA_ERR_TIMEOUT);
  auto viewer = Connect(false, 47324, {});
  ASSERT_NE(viewer, nullptr);
  ASSERT_TRUE(StreamUntil(host, *viewer, 3));
}

TEST(PublicApi, WithoutKeepServingTheFirstDisconnectIsFinal) {
  std::string err;
  rdaccel::HostConfig cfg;
  cfg.allow_plaintext = true;
  cfg.keep_serving = false;
  rdaccel::Host host(rdaccel::BindUdp(47325, &err), cfg);
  ASSERT_TRUE(host.ok()) << err;
  {
    auto viewer = Connect(false, 47325, {});
    ASSERT_NE(viewer, nullptr);
    ASSERT_TRUE(StreamUntil(host, *viewer, 3));
  }
  for (int i = 0; i < 400 && host.state() != rdaccel::ConnectionState::kDisconnected; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  EXPECT_EQ(host.state(), rdaccel::ConnectionState::kDisconnected);
}
