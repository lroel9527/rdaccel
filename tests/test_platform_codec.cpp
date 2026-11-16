// Encodes synthetic frames with this platform's HEVC encoder (and on macOS
// its H.264 encoder) and decodes them with this platform's decoder. On Linux
// this exercises FFmpeg (x265 or NVENC); on macOS, VideoToolbox. Also a
// host and a viewer in one process choosing the codec (1.11).

#include <gtest/gtest.h>

#include <cmath>
#include <cstdio>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "capture/synthetic_capturer.h"
#include "codec/codec_choice.h"
#include "log/log.h"
#include "platform.h"
#include "proto/messages.h"
#include "rdacore/host.h"
#include "rdacore/transport.h"
#include "session/link.h"
#include "session/viewer_session.h"

using namespace rdacore;

namespace {

double Psnr(const tile::ImageView& a, const std::vector<uint8_t>& b) {
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

}  // namespace

// Encodes and decodes 30 synthetic frames with the encoder CreateVideoEncoder
// picks for `prefer` and `vc`; `used` gets its name.
void RoundTrip(const std::string& prefer, std::string& used, codec::VideoCodec vc = codec::VideoCodec::kHevc) {
  constexpr int W = 640, H = 360, kFrames = 30;
  codec::EncoderConfig cfg{W, H, 30, 4'000'000};
  cfg.codec = vc;
  const size_t ps_count = vc == codec::VideoCodec::kH264 ? 2 : 3;  // (VPS,) SPS, PPS
  std::string err;
  auto enc = platform::CreateVideoEncoder(cfg, prefer, &err);
  ASSERT_TRUE(enc) << err;
  EXPECT_EQ(enc->codec(), vc);
  used = enc->name();
  auto dec = platform::CreateVideoDecoder(vc, &err);
  ASSERT_TRUE(dec) << err;

  std::vector<uint8_t> px;
  int decoded = 0;
  double min_psnr = 1e9;
  for (int i = 0; i < kFrames; ++i) {
    capture::RenderSyntheticFrame(static_cast<uint64_t>(i), W, H, px);
    auto au = enc->Encode({px.data(), W, H, W * 4}, static_cast<uint64_t>(i) * 33'333, i == 0);
    ASSERT_TRUE(au) << "frame " << i << ": " << enc->last_error();
    if (i == 0) {
      EXPECT_TRUE(au->keyframe);
      EXPECT_EQ(au->parameter_sets.size(), ps_count) << "expected (VPS,) SPS, PPS";
      EXPECT_EQ(codec::DetectCodec(au->parameter_sets), vc);
    } else {
      EXPECT_FALSE(au->keyframe) << "unexpected keyframe at " << i;
    }
    ASSERT_TRUE(dec->Decode(au->parameter_sets, au->nal_data, 0,
                            [&](const tile::ImageView& img, uint64_t) {
                              ASSERT_EQ(img.width, W);
                              ASSERT_EQ(img.height, H);
                              min_psnr = std::min(min_psnr, Psnr(img, px));
                              ++decoded;
                            }))
        << dec->last_error();
  }
  EXPECT_EQ(decoded, kFrames) << "low-latency decode should emit one picture per input";
  EXPECT_GT(min_psnr, 30.0);

  // A forced keyframe mid-stream carries parameter sets again.
  capture::RenderSyntheticFrame(kFrames, W, H, px);
  auto key = enc->Encode({px.data(), W, H, W * 4}, kFrames * 33'333ull, true);
  ASSERT_TRUE(key);
  EXPECT_TRUE(key->keyframe);
  EXPECT_EQ(key->parameter_sets.size(), ps_count);
}

TEST(PlatformCodec, EncodeDecodeRoundTrip) {
  std::string used;
  RoundTrip("", used);
}

TEST(PlatformCodec, H264EncodeDecodeRoundTrip) {
  if (!platform::CanDecode(codec::VideoCodec::kH264)) GTEST_SKIP() << "no H.264 on this platform";
  std::string used;
  RoundTrip("", used, codec::VideoCodec::kH264);
  std::printf("H.264 encoder: %s\n", used.c_str());
  EXPECT_NE(used.find("H.264"), std::string::npos);
}

// What the automatic choice costs here: a hardware HEVC encoder measured at
// a Retina size (a fast Mac stays on HEVC after this one measurement).
TEST(PlatformCodec, MeasuringHardwareHevcIsCheap) {
  constexpr int W = 2880, H = 1800;
  codec::EncoderConfig cfg{W, H, 60, 8'000'000, 24};
  cfg.hardware_only = true;
  const auto t0 = std::chrono::steady_clock::now();
  auto enc = platform::CreateVideoEncoder(cfg, "", nullptr);
  if (!enc) GTEST_SKIP() << "no hardware HEVC encoder";
  const auto t1 = std::chrono::steady_clock::now();
  const double fps = codec::MeasureEncodeFps(*enc, W, H);
  const auto t2 = std::chrono::steady_clock::now();
  using std::chrono::duration_cast;
  using std::chrono::milliseconds;
  const auto ms = duration_cast<milliseconds>(t2 - t0).count();
  std::printf("%s: %dx%d at %.0f fps, measured in %lld ms (%lld creating, %lld encoding)\n", enc->name().c_str(), W,
              H, fps, static_cast<long long>(ms), static_cast<long long>(duration_cast<milliseconds>(t1 - t0).count()),
              static_cast<long long>(duration_cast<milliseconds>(t2 - t1).count()));
  EXPECT_GT(fps, 0) << enc->last_error();
  EXPECT_LT(ms, 2000);
}

TEST(PlatformCodec, VaapiWhenAskedForOrAFallback) {
  // On a machine with a VAAPI GPU this runs the whole round trip through
  // hevc_vaapi; elsewhere (CI) the encoder falls back and still works.
  std::string used;
  RoundTrip("hevc_vaapi", used);
  std::printf("encoder for hevc_vaapi: %s\n", used.c_str());
  EXPECT_FALSE(used.empty());
}

namespace {

// A viewer made of the internal pieces, so the test chooses what it says it
// decodes (rdacore::Viewer says kCapH264 wherever the platform decodes it).
struct TestViewer {
  std::unique_ptr<Transport> t;
  std::unique_ptr<session::ViewerSession> vs;
  std::unique_ptr<session::Link> link;
  std::mutex mu;
  std::condition_variable cv;
  std::vector<codec::VideoCodec> hello_codecs;
  int frames = 0;
  double min_psnr = 1e9;
  std::vector<uint8_t> ref;

  TestViewer(std::unique_ptr<Transport> tr, uint64_t extra_caps) : t(std::move(tr)) {
    vs = std::make_unique<session::ViewerSession>(nullptr,
                                                  platform::CreateVideoDecoder(codec::VideoCodec::kHevc, nullptr));
    vs->SetDecoderFactory([](codec::VideoCodec c) { return platform::CreateVideoDecoder(c, nullptr); });
    vs->SetHelloHandler([this](const proto::Hello& h) {
      std::lock_guard<std::mutex> lock(mu);
      hello_codecs.push_back(h.codec);
    });
    vs->SetFrameHandler([this](const tile::ImageView& img, const std::vector<Rect>&, uint64_t id, uint64_t) {
      std::lock_guard<std::mutex> lock(mu);
      capture::RenderSyntheticFrame(id - 1, img.width, img.height, ref);
      min_psnr = std::min(min_psnr, Psnr(img, ref));
      ++frames;
      cv.notify_all();
    });
    vs->Attach(t.get(), /*own_receiver=*/false);
    session::Link::Handlers h;
    h.on_raw = [this](Channel ch, std::span<const uint8_t> b) { vs->OnReceive(ch, b); };
    session::LinkOptions lo;
    lo.extra_caps = extra_caps;
    link = std::make_unique<session::Link>(session::Link::Role::kViewer, t.get(), lo, std::move(h));
    t->SetReceiver([this](Channel ch, std::span<const uint8_t> b) { link->OnReceive(ch, b); });
    link->Start();
  }
  ~TestViewer() {
    link->Shutdown();
    vs->Stop();
    t.reset();
  }
  bool WaitFor(int n) {
    std::unique_lock<std::mutex> lock(mu);
    return cv.wait_for(lock, std::chrono::seconds(30), [&] { return frames >= n; });
  }
};

// Streams synthetic frames from a Host with `pref` to a viewer that does or
// does not claim to decode H.264; returns the codec of the last Hello and the
// host's encoder name.
void StreamWithCodec(HostConfig::VideoCodecPreference pref, bool viewer_h264, codec::VideoCodec& hello_codec,
                     std::string& encoder) {
  constexpr int W = 640, H = 360, kFrames = 15;
  auto [host_end, viewer_end] = CreateLocalPipe();
  TestViewer viewer(std::move(viewer_end), viewer_h264 ? proto::kCapH264 : 0);
  HostConfig cfg;
  cfg.fps = 30;
  cfg.bitrate_bps = 4'000'000;
  cfg.video_regions = false;
  cfg.video_codec = pref;
  Host host(std::move(host_end), cfg);
  for (int i = 0; i < 200 && host.state() != ConnectionState::kConnected; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  ASSERT_EQ(host.state(), ConnectionState::kConnected);
  std::vector<uint8_t> px;
  std::string err;
  for (int i = 0; i < kFrames; ++i) {
    const auto dirty = capture::RenderSyntheticFrame(static_cast<uint64_t>(i), W, H, px);
    ASSERT_TRUE(host.SubmitFrame({px.data(), W, H, W * 4}, capture::MonotonicMicros(), dirty, &err)) << err;
  }
  ASSERT_TRUE(viewer.WaitFor(kFrames));
  encoder = host.encoder_name();
  std::lock_guard<std::mutex> lock(viewer.mu);
  ASSERT_FALSE(viewer.hello_codecs.empty());
  hello_codec = viewer.hello_codecs.back();
  EXPECT_GT(viewer.min_psnr, 30.0);
  EXPECT_EQ(viewer.vs->stats().decode_errors, 0u);
}

}  // namespace

TEST(PlatformCodecChoice, ForcedH264ReachesAViewerThatDecodesIt) {
  if (!platform::CanDecode(codec::VideoCodec::kH264)) GTEST_SKIP() << "no H.264 on this platform";
  codec::VideoCodec c{};
  std::string enc;
  StreamWithCodec(HostConfig::VideoCodecPreference::kH264, true, c, enc);
  EXPECT_EQ(c, codec::VideoCodec::kH264);
  EXPECT_NE(enc.find("H.264"), std::string::npos) << enc;
}

TEST(PlatformCodecChoice, AViewerWithoutH264GetsHevcEvenWhenH264IsAskedFor) {
  codec::VideoCodec c{};
  std::string enc;
  StreamWithCodec(HostConfig::VideoCodecPreference::kH264, false, c, enc);
  EXPECT_EQ(c, codec::VideoCodec::kHevc);
  EXPECT_NE(enc.find("HEVC"), std::string::npos) << enc;
  StreamWithCodec(HostConfig::VideoCodecPreference::kAuto, false, c, enc);
  EXPECT_EQ(c, codec::VideoCodec::kHevc);
}

// What rdmachine always does: the host captures before the viewer has said
// what it decodes, so its session starts as HEVC; once a viewer that decodes
// H.264 is there, the host starts over in H.264 (as for a resolution
// change, Hello with the frames) and the viewer follows.
TEST(PlatformCodecChoice, AHostThatStartedInHevcSwitchesOnceTheViewerSaysItDecodesH264) {
  if (!platform::CanDecode(codec::VideoCodec::kH264)) GTEST_SKIP() << "no H.264 on this platform";
  constexpr int W = 640, H = 360;
  auto [host_end, viewer_end] = CreateLocalPipe();
  HostConfig cfg;
  cfg.fps = 30;
  cfg.bitrate_bps = 4'000'000;
  cfg.video_regions = false;
  cfg.video_codec = HostConfig::VideoCodecPreference::kH264;
  Host host(std::move(host_end), cfg);
  std::vector<uint8_t> px;
  std::string err;
  uint64_t i = 0;
  // Before any viewer: frames go into an HEVC session.
  for (; i < 5; ++i) {
    const auto dirty = capture::RenderSyntheticFrame(i, W, H, px);
    ASSERT_TRUE(host.SubmitFrame({px.data(), W, H, W * 4}, capture::MonotonicMicros(), dirty, &err)) << err;
  }
  EXPECT_NE(host.encoder_name().find("HEVC"), std::string::npos) << host.encoder_name();
  TestViewer viewer(std::move(viewer_end), proto::kCapH264);
  for (int k = 0; k < 200 && host.state() != ConnectionState::kConnected; ++k) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  ASSERT_EQ(host.state(), ConnectionState::kConnected);
  for (; i < 25; ++i) {
    const auto dirty = capture::RenderSyntheticFrame(i, W, H, px);
    ASSERT_TRUE(host.SubmitFrame({px.data(), W, H, W * 4}, capture::MonotonicMicros(), dirty, &err)) << err;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ASSERT_TRUE(viewer.WaitFor(10));
  EXPECT_NE(host.encoder_name().find("H.264"), std::string::npos) << host.encoder_name();
  std::lock_guard<std::mutex> lock(viewer.mu);
  ASSERT_FALSE(viewer.hello_codecs.empty());
  EXPECT_EQ(viewer.hello_codecs.back(), codec::VideoCodec::kH264);
  EXPECT_NE(viewer.vs->decoder_name().find("H.264"), std::string::npos) << viewer.vs->decoder_name();
  // The HEVC Hello (control channel) and the H.264 one (with the frames)
  // may arrive in either order; the wrong order costs at most one picture.
  const auto errors = viewer.vs->stats().decode_errors;
  std::printf("frames %d, decode errors %llu, min PSNR %.1f\n", viewer.frames,
              static_cast<unsigned long long>(errors), viewer.min_psnr);
  EXPECT_LE(errors, 1u);
}

TEST(PlatformCodecChoice, AutoPicksAWorkingCodec) {
  // On a Mac with fast hardware HEVC (Apple Silicon) this stays HEVC; on one
  // whose HEVC is slow at this size it may be H.264. Either way the stream
  // and the encoder agree, and every picture decodes.
  static std::mutex log_mu;
  static std::vector<std::string> lines;
  log::SetSink(
      [](void*, int, const char* m) {
        std::lock_guard<std::mutex> l(log_mu);
        if (std::string(m).find("video codec") != std::string::npos) lines.emplace_back(m);
      },
      nullptr, log::Level::kInfo);
  codec::VideoCodec c{};
  std::string enc;
  const bool h264 = platform::CanDecode(codec::VideoCodec::kH264);
  StreamWithCodec(HostConfig::VideoCodecPreference::kAuto, h264, c, enc);
  log::SetSink(nullptr, nullptr, log::Level::kInfo);
  std::printf("auto: %s (%s)\n", codec::VideoCodecName(c), enc.c_str());
  EXPECT_NE(enc.find(codec::VideoCodecName(c)), std::string::npos) << enc;
  std::lock_guard<std::mutex> l(log_mu);
  for (const auto& m : lines) std::printf("  log: %s\n", m.c_str());
  // The decision is logged once it is made for a viewer that decodes H.264.
  if (h264) EXPECT_FALSE(lines.empty());
}

TEST(PlatformCursor, ReadsTheSystemCursor) {
  std::string err;
  auto src = platform::CreateCursorSource(0, &err);
  if (!src) GTEST_SKIP() << "no cursor source: " << err;  // e.g. no X display
  capture::CursorSample s;
  bool any_image = false;
  for (int i = 0; i < 8; ++i) {
    ASSERT_TRUE(src->Poll(s));
    EXPECT_GT(s.screen_width, 0);
    EXPECT_GT(s.screen_height, 0);
    if (s.image_changed) {
      any_image = true;
      EXPECT_GT(s.image.width, 0);
      EXPECT_LE(s.image.width, 256);
      EXPECT_EQ(s.image.bgra.size(), static_cast<size_t>(s.image.width) * s.image.height * 4);
      EXPECT_GE(s.image.hotspot_x, 0);
      EXPECT_LT(s.image.hotspot_x, s.image.width);
    }
  }
  std::printf("cursor at %.0f,%.0f on %.0fx%.0f, image %s\n", s.x, s.y, s.screen_width,
              s.screen_height, any_image ? "read" : "not available");
}

TEST(PlatformDisplays, ListsThePrimaryDisplayFirst) {
  std::vector<capture::DisplayInfo> d;
  std::string err;
  if (!platform::ListDisplays(d, &err)) GTEST_SKIP() << err;
  ASSERT_FALSE(d.empty());
  EXPECT_TRUE(d[0].primary);
  for (size_t i = 0; i < d.size(); ++i) {
    EXPECT_EQ(d[i].index, static_cast<int32_t>(i));
    EXPECT_GT(d[i].width, 0);
    EXPECT_GT(d[i].height, 0);
    if (i > 0) {
      EXPECT_FALSE(d[i].primary);
    }
  }
}
