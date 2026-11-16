// Which codec a host sends (codec/codec_choice.h), with fake measurements:
// HEVC whenever it is usable, H.264 only for a viewer that decodes it when
// hardware HEVC is missing or too slow and hardware H.264 is faster.

#include <gtest/gtest.h>

#include <chrono>
#include <thread>
#include <vector>

#include "codec/codec_choice.h"

using namespace rdacore;
using codec::ChooseVideoCodec;
using codec::CodecPreference;
using codec::VideoCodec;

namespace {

// Hardware fps per codec, and which codecs were measured.
struct FakeHardware {
  double hevc = 60;
  double h264 = 60;
  std::vector<VideoCodec> asked;
  codec::HardwareFpsFn Fn() {
    return [this](VideoCodec c, int32_t, int32_t) {
      asked.push_back(c);
      return c == VideoCodec::kHevc ? hevc : h264;
    };
  }
};

}  // namespace

TEST(CodecChoice, ViewerWithoutH264GetsHevcAndNothingIsMeasured) {
  for (auto pref : {CodecPreference::kAuto, CodecPreference::kHevc}) {
    FakeHardware hw;
    hw.hevc = 0;  // even with no hardware HEVC at all
    const auto c = ChooseVideoCodec(pref, /*viewer_decodes_h264=*/false, 2880, 1800, hw.Fn());
    EXPECT_EQ(c.codec, VideoCodec::kHevc);
    EXPECT_FALSE(c.measured);
    EXPECT_FALSE(c.warn);
    EXPECT_TRUE(hw.asked.empty());
  }
}

TEST(CodecChoice, AutoWithFastHardwareHevcStaysHevcWithoutTryingH264) {
  FakeHardware hw;
  hw.hevc = 120;
  const auto c = ChooseVideoCodec(CodecPreference::kAuto, true, 2880, 1800, hw.Fn());
  EXPECT_EQ(c.codec, VideoCodec::kHevc);
  EXPECT_TRUE(c.measured);
  EXPECT_FALSE(c.warn);
  EXPECT_EQ(hw.asked, std::vector<VideoCodec>{VideoCodec::kHevc});
  // Exactly the threshold is usable.
  FakeHardware edge;
  edge.hevc = codec::kMinUsableFps;
  EXPECT_EQ(ChooseVideoCodec(CodecPreference::kAuto, true, 1920, 1080, edge.Fn()).codec, VideoCodec::kHevc);
  EXPECT_EQ(edge.asked.size(), 1u);
}

TEST(CodecChoice, AutoWithSlowHardwareHevcTakesFasterH264) {
  // The Intel MacBook Pro 2016 at 2880 x 1800.
  FakeHardware hw;
  hw.hevc = 15;
  hw.h264 = 52;
  const auto c = ChooseVideoCodec(CodecPreference::kAuto, true, 2880, 1800, hw.Fn());
  EXPECT_EQ(c.codec, VideoCodec::kH264);
  EXPECT_TRUE(c.warn);
  EXPECT_DOUBLE_EQ(c.hevc_fps, 15);
  EXPECT_DOUBLE_EQ(c.h264_fps, 52);
  EXPECT_EQ(hw.asked, (std::vector<VideoCodec>{VideoCodec::kHevc, VideoCodec::kH264}));
  EXPECT_NE(c.reason.find("H.264"), std::string::npos) << c.reason;
}

TEST(CodecChoice, AutoWithoutHardwareHevcTakesWorkingH264) {
  FakeHardware hw;
  hw.hevc = 0;  // no hardware HEVC encoder for this size (above 4096 x 2304)
  hw.h264 = 40;
  const auto c = ChooseVideoCodec(CodecPreference::kAuto, true, 5120, 2880, hw.Fn());
  EXPECT_EQ(c.codec, VideoCodec::kH264);
  EXPECT_TRUE(c.warn);
}

TEST(CodecChoice, AutoKeepsHevcWhenH264IsNoBetter) {
  {
    FakeHardware hw;  // both missing: HEVC (software), as before
    hw.hevc = 0;
    hw.h264 = 0;
    const auto c = ChooseVideoCodec(CodecPreference::kAuto, true, 2880, 1800, hw.Fn());
    EXPECT_EQ(c.codec, VideoCodec::kHevc);
    EXPECT_FALSE(c.warn);
  }
  {
    FakeHardware hw;  // both slow, H.264 slower still
    hw.hevc = 20;
    hw.h264 = 18;
    EXPECT_EQ(ChooseVideoCodec(CodecPreference::kAuto, true, 2880, 1800, hw.Fn()).codec, VideoCodec::kHevc);
  }
}

TEST(CodecChoice, ForcedHevcMeasuresNothing) {
  FakeHardware hw;
  hw.hevc = 1;
  const auto c = ChooseVideoCodec(CodecPreference::kHevc, true, 2880, 1800, hw.Fn());
  EXPECT_EQ(c.codec, VideoCodec::kHevc);
  EXPECT_FALSE(c.measured);
  EXPECT_FALSE(c.warn);
  EXPECT_TRUE(hw.asked.empty());
}

TEST(CodecChoice, ForcedH264) {
  FakeHardware hw;
  // A viewer that decodes it: H.264, nothing measured.
  auto c = ChooseVideoCodec(CodecPreference::kH264, true, 1280, 720, hw.Fn());
  EXPECT_EQ(c.codec, VideoCodec::kH264);
  EXPECT_FALSE(c.measured);
  EXPECT_FALSE(c.warn);
  // One that does not: HEVC, with a warning.
  c = ChooseVideoCodec(CodecPreference::kH264, false, 1280, 720, hw.Fn());
  EXPECT_EQ(c.codec, VideoCodec::kHevc);
  EXPECT_TRUE(c.warn);
  EXPECT_TRUE(hw.asked.empty());
}

namespace {

// Takes `ms_per_frame` per picture; counts them.
class SlowEncoder final : public codec::IHevcEncoder {
 public:
  explicit SlowEncoder(int ms_per_frame, bool fail = false) : ms_(ms_per_frame), fail_(fail) {}
  int calls = 0;
  std::string name() const override { return "slow"; }
  void SetBitrate(int64_t) override {}
  const std::string& last_error() const override { return err_; }
  std::optional<codec::HevcAccessUnit> Encode(const tile::ImageView& img, uint64_t, bool key) override {
    ++calls;
    EXPECT_GT(img.width, 0);
    if (fail_) {
      err_ = "broken";
      return std::nullopt;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(ms_));
    codec::HevcAccessUnit au;
    au.keyframe = key;
    au.nal_data = {0, 0, 0, 1, 0x26};
    return au;
  }

 private:
  int ms_;
  bool fail_;
  std::string err_;
};

}  // namespace

TEST(CodecChoice, MeasureEncodeFpsIsShortAndGivesUpOnSlowEncoders) {
  {
    SlowEncoder fast(2);
    const double fps = codec::MeasureEncodeFps(fast, 320, 200, 8);
    EXPECT_EQ(fast.calls, 9);  // warm-up keyframe + 8
    EXPECT_GT(fps, codec::kMinUsableFps);
  }
  {
    // 100 ms a picture: 8 at 30 fps is a budget of 267 ms, so it stops
    // after 3 timed pictures.
    SlowEncoder slow(100);
    const auto t0 = std::chrono::steady_clock::now();
    const double fps = codec::MeasureEncodeFps(slow, 320, 200, 8);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    EXPECT_LT(slow.calls, 9);
    EXPECT_LT(fps, codec::kMinUsableFps);
    EXPECT_GT(fps, 3.0);
    EXPECT_LT(ms, 800);
  }
  {
    SlowEncoder broken(0, /*fail=*/true);
    EXPECT_EQ(codec::MeasureEncodeFps(broken, 320, 200, 8), 0.0);
    EXPECT_EQ(broken.calls, 1);
  }
}
