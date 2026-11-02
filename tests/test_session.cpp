// HostSession -> LocalPipe -> ViewerSession with a fake codec, so the frame
// pipeline (tile skip, regions, reassembly, input path) is tested without any
// real encoder.

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <mutex>

#include "capture/synthetic_capturer.h"
#include "session/host_session.h"
#include "session/viewer_session.h"
#include "transport/local_pipe.h"

using namespace rdaccel;

namespace {

// "Encodes" a frame as its raw pixels (first row only, to stay small) inside
// a fake slice NAL; the decoder expands it back.
class FakeEncoder final : public codec::IHevcEncoder {
 public:
  std::string name() const override { return "fake"; }
  const std::string& last_error() const override { return err_; }
  void SetBitrate(int64_t) override {}
  std::optional<codec::HevcAccessUnit> Encode(const tile::ImageView& img, uint64_t,
                                              bool force_keyframe) override {
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
  std::string name() const override { return "fake"; }
  const std::string& last_error() const override { return err_; }
  bool Decode(const std::vector<std::vector<uint8_t>>& ps, const std::vector<uint8_t>& nal_data,
              uint64_t pts, const codec::DecodedFrameFn& on_frame) override {
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

}  // namespace

TEST(Session, FramesFlowAndStaticFramesAreSkipped) {
  auto [host_end, viewer_end] = transport::MakeLocalPipe();
  constexpr int W = 200, H = 120;
  session::HostSession host(host_end.get(), std::make_unique<FakeEncoder>(), W, H);
  session::ViewerSession viewer(viewer_end.get(), std::make_unique<FakeDecoder>());

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

  std::vector<uint8_t> px(static_cast<size_t>(W) * H * 4, 50);
  capture::CapturedFrame f{{px.data(), W, H, W * 4}, {}, 0};
  host.OnFrame(f);  // keyframe
  host.OnFrame(f);  // identical -> skipped
  px[(70 * W + 150) * 4] = 99;  // one pixel in tile (2, 1)
  host.OnFrame(f);

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
