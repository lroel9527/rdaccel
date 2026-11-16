// FallbackCapturer: the preferred capturer whenever it works, the fallback
// only when it does not start or gives no picture in time.

#include "capture/fallback_capturer.h"

#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>

namespace rdacore::capture {
namespace {

using namespace std::chrono_literals;

// Delivers frames on a thread of its own, or never (`frames` false), or does
// not start at all (`starts` false).
class FakeCapturer final : public ICapturer {
 public:
  FakeCapturer(bool starts, bool frames, std::atomic<int>* started, std::atomic<int>* stopped)
      : starts_(starts), frames_(frames), started_(started), stopped_(stopped) {}
  ~FakeCapturer() override { Stop(); }

  bool Start(const CaptureConfig&, FrameFn on_frame) override {
    if (!starts_) {
      error_ = "refused";
      return false;
    }
    ++*started_;
    running_ = true;
    if (frames_) {
      thread_ = std::thread([this, on_frame] {
        std::vector<uint8_t> px(4 * 4 * 4, 0x80);
        while (running_) {
          CapturedFrame f;
          f.bgra = {px.data(), 4, 4, 16};
          f.capture_time_us = MonotonicMicros();
          on_frame(f);
          std::this_thread::sleep_for(5ms);
        }
      });
    }
    return true;
  }
  void Stop() override {
    if (!running_) return;
    running_ = false;
    if (thread_.joinable()) thread_.join();
    ++*stopped_;
  }
  const std::string& last_error() const override { return error_; }

 private:
  bool starts_, frames_;
  std::atomic<int>* started_;
  std::atomic<int>* stopped_;
  std::atomic<bool> running_{false};
  std::thread thread_;
  std::string error_;
};

struct Counts {
  std::atomic<int> preferred_started{0}, preferred_stopped{0}, fallback_started{0}, fallback_stopped{0};
  std::atomic<int> frames{0};
};

std::unique_ptr<FallbackCapturer> Make(Counts& c, bool p_starts, bool p_frames, bool f_starts, bool f_frames) {
  return std::make_unique<FallbackCapturer>(
      [&c, p_starts, p_frames] {
        return std::make_unique<FakeCapturer>(p_starts, p_frames, &c.preferred_started, &c.preferred_stopped);
      },
      "new",
      [&c, f_starts, f_frames] {
        return std::make_unique<FakeCapturer>(f_starts, f_frames, &c.fallback_started, &c.fallback_stopped);
      },
      "old", 200ms);
}

TEST(FallbackCapturer, UsesThePreferredOneWhenItGivesPictures) {
  Counts c;
  auto cap = Make(c, true, true, true, true);
  ASSERT_TRUE(cap->Start({}, [&](const CapturedFrame&) { ++c.frames; }));
  EXPECT_FALSE(cap->using_fallback());
  EXPECT_EQ(c.preferred_started, 1);
  EXPECT_EQ(c.fallback_started, 0);  // never even tried
  std::this_thread::sleep_for(30ms);
  EXPECT_GT(c.frames, 0);
  cap->Stop();
  EXPECT_EQ(c.preferred_stopped, 1);
}

TEST(FallbackCapturer, FallsBackWhenThePreferredOneGivesNoPicture) {
  Counts c;
  auto cap = Make(c, true, false, true, true);
  const auto t = std::chrono::steady_clock::now();
  ASSERT_TRUE(cap->Start({}, [&](const CapturedFrame&) { ++c.frames; }));
  EXPECT_GE(std::chrono::steady_clock::now() - t, 200ms);  // it had its chance
  EXPECT_TRUE(cap->using_fallback());
  EXPECT_EQ(c.preferred_started, 1);
  EXPECT_EQ(c.preferred_stopped, 1);  // and was stopped before the fallback started
  EXPECT_EQ(c.fallback_started, 1);
  std::this_thread::sleep_for(30ms);
  EXPECT_GT(c.frames, 0);
}

TEST(FallbackCapturer, FallsBackWhenThePreferredOneDoesNotStart) {
  Counts c;
  auto cap = Make(c, false, false, true, true);
  ASSERT_TRUE(cap->Start({}, [&](const CapturedFrame&) { ++c.frames; }));
  EXPECT_TRUE(cap->using_fallback());
  EXPECT_EQ(c.fallback_started, 1);
}

TEST(FallbackCapturer, SaysWhyWhenNeitherWorks) {
  Counts c;
  auto cap = Make(c, false, false, false, false);
  EXPECT_FALSE(cap->Start({}, [](const CapturedFrame&) {}));
  EXPECT_NE(cap->last_error().find("new did not start: refused"), std::string::npos) << cap->last_error();
  EXPECT_NE(cap->last_error().find("old did not start either: refused"), std::string::npos) << cap->last_error();
}

TEST(FallbackCapturer, TriesThePreferredOneAgainOnEveryStart) {
  Counts c;
  auto cap = Make(c, true, false, true, true);
  ASSERT_TRUE(cap->Start({}, [](const CapturedFrame&) {}));
  cap->Stop();
  ASSERT_TRUE(cap->Start({}, [](const CapturedFrame&) {}));
  EXPECT_EQ(c.preferred_started, 2);  // a failure is not remembered
  EXPECT_EQ(c.fallback_started, 2);
}

TEST(FallbackCapturer, SkipsThePreferredOneWhenToldItFailedBefore) {
  Counts c;
  auto cap = Make(c, true, true, true, true);  // the preferred one would work now
  cap->SkipPreferred(true);  // but its owner saw it fail earlier in the session
  const auto t = std::chrono::steady_clock::now();
  ASSERT_TRUE(cap->Start({}, [&](const CapturedFrame&) { ++c.frames; }));
  EXPECT_LT(std::chrono::steady_clock::now() - t, 200ms);  // no wait for it
  EXPECT_TRUE(cap->using_fallback());
  EXPECT_EQ(c.preferred_started, 0);
  EXPECT_EQ(c.fallback_started, 1);
  cap->Stop();
  cap->SkipPreferred(false);  // a new session asks the preferred one again
  ASSERT_TRUE(cap->Start({}, [](const CapturedFrame&) {}));
  EXPECT_FALSE(cap->using_fallback());
  EXPECT_EQ(c.preferred_started, 1);
}

}  // namespace
}  // namespace rdacore::capture
