// The CGDisplayStream capturer on this Mac's main display (the fallback for
// macOS 12 and 13; Apple keeps the API working on later systems too, which
// is where it is tested). Needs the Screen Recording permission.

#include "mac/capture_cgstream.h"

#import <CoreGraphics/CoreGraphics.h>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

#include "mac/displays_mac.h"

namespace rdacore::platform {
namespace {

using namespace std::chrono_literals;

TEST(CgStreamCapturer, DeliversPicturesOfTheMainDisplay) {
  if (!CGPreflightScreenCaptureAccess()) GTEST_SKIP() << "no Screen Recording permission";
  CgStreamCapturer cap;
  std::mutex mu;
  int frames = 0;
  int32_t w = 0, h = 0;
  bool bright = false;
  ASSERT_TRUE(cap.Start({}, [&](const capture::CapturedFrame& f) {
    std::lock_guard<std::mutex> l(mu);
    ++frames;
    w = f.bgra.width;
    h = f.bgra.height;
    // Not all black: a real picture, not a placeholder.
    for (int32_t y = 0; y < f.bgra.height && !bright; y += 37) {
      const uint8_t* row = f.bgra.data + static_cast<size_t>(y) * f.bgra.stride;
      for (int32_t x = 0; x < f.bgra.width; x += 41) {
        if (row[x * 4] > 16 || row[x * 4 + 1] > 16 || row[x * 4 + 2] > 16) {
          bright = true;
          break;
        }
      }
    }
  })) << cap.last_error();
  for (int i = 0; i < 100; ++i) {
    {
      std::lock_guard<std::mutex> l(mu);
      if (frames > 0) break;
    }
    std::this_thread::sleep_for(20ms);
  }
  cap.Stop();
  std::lock_guard<std::mutex> l(mu);
  ASSERT_GT(frames, 0) << "no picture in 2 s";
  CGDirectDisplayID main = 0;
  ASSERT_TRUE(MacDisplayId(0, main, nullptr));
  const CGRect b = CGDisplayBounds(main);
  const double scale = MacDisplayScale(main);
  EXPECT_EQ(w, static_cast<int32_t>(std::lround(b.size.width * scale)));
  EXPECT_EQ(h, static_cast<int32_t>(std::lround(b.size.height * scale)));
  EXPECT_TRUE(bright);
}

TEST(CgStreamCapturer, StopsAndStartsAgain) {
  if (!CGPreflightScreenCaptureAccess()) GTEST_SKIP() << "no Screen Recording permission";
  CgStreamCapturer cap;
  std::atomic<int> frames{0};
  for (int round = 0; round < 2; ++round) {
    frames = 0;
    ASSERT_TRUE(cap.Start({}, [&](const capture::CapturedFrame&) { ++frames; })) << cap.last_error();
    for (int i = 0; i < 100 && frames == 0; ++i) std::this_thread::sleep_for(20ms);
    cap.Stop();
    EXPECT_GT(frames, 0) << "round " << round;
  }
}

}  // namespace
}  // namespace rdacore::platform
