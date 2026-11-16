// Sound on macOS without a sound: the host-time clock, the player's null
// device (and that the environment keeps every test away from a real one),
// and, only with RDACCEL_TEST_AUDIO_TAP=1, a Process Tap that does not mute
// (CATapUnmuted) read for two seconds. Samples are counted and their times
// checked, never kept, never played.

#include <gtest/gtest.h>

#import <CoreAudio/CoreAudio.h>
#include <mach/mach_time.h>
#include <rdaccel/rdaccel.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "audio/audio_source.h"
#include "audio/player.h"
#include "capture/capturer.h"
#include "mac/audio_tap.h"

using namespace rdacore;

namespace {

// Every test here runs under CTest, which sets RDACCEL_AUDIO_NULL_OUTPUT=1
// for all of them (CMakeLists.txt). Refuse to go on without it.
bool NullOutputSet() {
  const char* v = std::getenv("RDACCEL_AUDIO_NULL_OUTPUT");
  return v && std::strcmp(v, "1") == 0;
}

size_t TapCount() {
  AudioObjectPropertyAddress a = {kAudioHardwarePropertyTapList, kAudioObjectPropertyScopeGlobal,
                                  kAudioObjectPropertyElementMain};
  UInt32 size = 0;
  if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &a, 0, nullptr, &size) != noErr) return 0;
  return size / sizeof(AudioObjectID);
}

}  // namespace

TEST(PlatformAudioMac, HostTimeIsTheLibrarysClock) {
  for (int i = 0; i < 5; ++i) {
    const uint64_t a = platform::HostTimeToMicros(mach_absolute_time());
    const uint64_t b = capture::MonotonicMicros();
    EXPECT_LT(a > b ? a - b : b - a, 1000u);
    std::this_thread::sleep_for(std::chrono::milliseconds(3));
  }
}

TEST(PlatformAudioMac, NullDeviceTakesRealTimeAndOpensNothing) {
  ASSERT_TRUE(NullOutputSet()) << "run under CTest (RDACCEL_AUDIO_NULL_OUTPUT=1): this test must not reach a device";
  std::atomic<uint64_t> frames{0};
  {
    auto p = audio::CreateNullPlayer([&](float* out, size_t n, uint64_t) {
      std::memset(out, 0, n * 2 * sizeof(float));
      frames += n;
      return size_t{0};
    });
    std::this_thread::sleep_for(std::chrono::seconds(5));
  }
  EXPECT_NEAR(static_cast<double>(frames.load()), 48'000.0 * 5, 48'000.0 * 5 * 0.01);
  // The public player, asked for the real device, still makes the null one
  // while the variable is set.
  rda_viewer* v = nullptr;
  ASSERT_EQ(rda_viewer_create(nullptr, nullptr, &v), RDA_OK) << rda_last_error();
  rda_audio_player_config c;
  rda_audio_player_config_default(&c);
  EXPECT_EQ(c.null_device, 0);
  rda_audio_player* p = nullptr;
  ASSERT_EQ(rda_audio_player_create(v, &c, &p), RDA_OK) << rda_last_error();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  EXPECT_EQ(audio::DevicePlayersOpened(), 0u);
  EXPECT_EQ(rda_audio_player_output_muted(p), -1);  // the null device knows of no output
  rda_audio_player_destroy(p);
  rda_viewer_destroy(v);
  EXPECT_EQ(audio::DevicePlayersOpened(), 0u);
}

TEST(PlatformAudioMac, ProcessTapDeliversTimedFrames) {
  ASSERT_TRUE(NullOutputSet());
  const char* on = std::getenv("RDACCEL_TEST_AUDIO_TAP");
  if (!on || std::strcmp(on, "1") != 0) GTEST_SKIP() << "set RDACCEL_TEST_AUDIO_TAP=1 (it may ask for a permission)";
  if (platform::AudioCapturePermission() == 1) GTEST_SKIP() << "no permission (System Audio Recording denied)";
  const size_t taps_before = TapCount();
  auto src = audio::CreateSystemSource();
  ASSERT_TRUE(src);
  audio::SourceConfig cfg;
  cfg.mute_local = false;  // CATapUnmuted: this computer is never silenced by a test
  std::mutex mu;
  std::vector<std::pair<uint64_t, size_t>> chunks;  // time, frames
  std::atomic<int> running{0}, failed{0};
  std::string why;
  ASSERT_TRUE(src->Start(
      cfg,
      [&](const float* pcm, size_t n, uint64_t t) {
        (void)pcm;  // looked at by no one: counted and dropped
        std::lock_guard<std::mutex> l(mu);
        chunks.emplace_back(t, n);
      },
      [&](audio::SourceState st, const std::string& msg) {
        if (st == audio::SourceState::kRunning) ++running;
        if (st == audio::SourceState::kFailed) {
          ++failed;
          std::lock_guard<std::mutex> l(mu);
          why = msg;
        }
      }));
  std::this_thread::sleep_for(std::chrono::milliseconds(2500));
  EXPECT_FALSE(src->muting_local());
  src->Stop();
  EXPECT_EQ(failed.load(), 0) << why;
  EXPECT_GE(running.load(), 1);
  std::lock_guard<std::mutex> l(mu);
  size_t frames = 0;
  for (size_t i = 0; i < chunks.size(); ++i) {
    frames += chunks[i].second;
    if (i > 0) EXPECT_GT(chunks[i].first, chunks[i - 1].first) << i;
  }
  // About 2.5 s at 48 kHz (whatever the device's rate, it arrives as 48 kHz).
  std::printf("tap: %zu callbacks, %zu frames in 2.5 s\n", chunks.size(), frames);
  EXPECT_GT(frames, 48'000u * 2);
  EXPECT_LT(frames, 48'000u * 3);
  ASSERT_FALSE(chunks.empty());
  // The times are the capture times on the library's clock: the last one
  // shortly before now.
  const uint64_t now = capture::MonotonicMicros();
  EXPECT_LT(now - chunks.back().first, 1'000'000u);
  // The tap is gone afterwards (coreaudiod drops it from the list shortly).
  bool gone = false;
  for (int i = 0; i < 100 && !gone; ++i) {
    gone = TapCount() <= taps_before;
    if (!gone) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_TRUE(gone) << TapCount() << " taps, " << taps_before << " before";
}

TEST(PlatformAudioMac, UnderTheTestEnvironmentNoTapMutes) {
  // CTest sets RDACCEL_AUDIO_NULL_OUTPUT=1: a system source asked to keep
  // this computer silent never builds a muting tap, even with the
  // permission known. (The decision only: no tap is built here.)
  ASSERT_TRUE(NullOutputSet());
  EXPECT_FALSE(platform::LocalMuteAllowed());
  EXPECT_FALSE(platform::TapMutesLocal(/*permission_confirmed=*/true, /*mute_local=*/true));
  // What it would decide elsewhere, from the values alone: the variables
  // stay set the whole time.
  EXPECT_TRUE(platform::LocalMuteAllowedFor(nullptr, nullptr));
  EXPECT_TRUE(platform::LocalMuteAllowedFor("0", ""));
  EXPECT_FALSE(platform::LocalMuteAllowedFor("1", nullptr));
  EXPECT_FALSE(platform::LocalMuteAllowedFor(nullptr, "1"));
  EXPECT_TRUE(platform::TapMutesLocalWith(true, true, /*allowed=*/true));
  EXPECT_FALSE(platform::TapMutesLocalWith(false, true, true));  // no muting before the permission is known
  EXPECT_FALSE(platform::TapMutesLocalWith(true, false, true));
  EXPECT_FALSE(platform::TapMutesLocalWith(true, true, /*allowed=*/false));
  EXPECT_TRUE(NullOutputSet());
}
