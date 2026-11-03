// Tests of the public API, written the way an application would use it:
// only <rdaccel/rdaccel.h>, real platform codecs.

#include <rdaccel/rdaccel.h>

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <string>
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
  EXPECT_STREQ(rdaccel::Version(), "0.1.0");
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
  // After the first frame the encoder may refine the static picture for
  // `refine_frames` frames; after that, nothing more is sent.
  const int n = cfg.refine_frames + 20;
  for (int i = 0; i < n; ++i) {
    ASSERT_TRUE(host.SubmitFrame({px.data(), kW, kH, kW * 4}, rdaccel::NowMicros()));
  }
  ASSERT_TRUE(got.WaitFor(1));
  const auto st = host.stats();
  EXPECT_LE(st.frames_encoded, 1u + static_cast<uint64_t>(cfg.refine_frames));
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
