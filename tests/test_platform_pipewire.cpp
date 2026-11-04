// Wayland capture: the PipeWire stream reader against a fake screen (a
// PipeWire video source made here, with damage and cursor metadata), and the
// whole portal path against a real compositor. CI runs these inside
// tools/wayland_headless.sh (sway + PipeWire + xdg-desktop-portal-wlr);
// without a PipeWire daemon or a portal they skip. xdg-desktop-portal-wlr
// 0.7.1 delivers frames only for the first screen-cast session it serves, so
// CI runs each PipeWirePortal test in a session of its own.

#include <gtest/gtest.h>

#include <pipewire/pipewire.h>
#include <spa/buffer/meta.h>
#include <spa/param/video/format-utils.h>
#include <spa/pod/builder.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

#include "linux/capture_pipewire.h"
#include "platform.h"

using namespace rdacore;
using namespace std::chrono_literals;

namespace {

TEST(PipeWireFormat, ChannelOrderAndConversion) {
  int b = 0, g = 0, r = 0;
  ASSERT_TRUE(platform::pw_detail::ChannelOrder(SPA_VIDEO_FORMAT_BGRx, b, g, r));
  EXPECT_EQ(b, 0);
  EXPECT_EQ(r, 2);
  ASSERT_FALSE(platform::pw_detail::ChannelOrder(SPA_VIDEO_FORMAT_NV12, b, g, r));
  // One pixel R=10 G=20 B=30 in each 4-byte layout.
  struct Case {
    uint32_t format;
    uint8_t px[4];
  } cases[] = {
      {SPA_VIDEO_FORMAT_BGRx, {30, 20, 10, 0}}, {SPA_VIDEO_FORMAT_BGRA, {30, 20, 10, 9}},
      {SPA_VIDEO_FORMAT_RGBx, {10, 20, 30, 0}}, {SPA_VIDEO_FORMAT_RGBA, {10, 20, 30, 9}},
      {SPA_VIDEO_FORMAT_xRGB, {0, 10, 20, 30}}, {SPA_VIDEO_FORMAT_ARGB, {9, 10, 20, 30}},
      {SPA_VIDEO_FORMAT_xBGR, {0, 30, 20, 10}}, {SPA_VIDEO_FORMAT_ABGR, {9, 30, 20, 10}},
  };
  for (const auto& c : cases) {
    ASSERT_TRUE(platform::pw_detail::ChannelOrder(c.format, b, g, r)) << c.format;
    // 2x2 picture with a padded source stride.
    uint8_t src[2 * 12] = {};
    for (int y = 0; y < 2; ++y) {
      for (int x = 0; x < 2; ++x) std::memcpy(&src[y * 12 + x * 4], c.px, 4);
    }
    uint8_t dst[16] = {};
    platform::pw_detail::ToBgra(src, 12, 2, 2, b, g, r, dst, 8);
    for (int i = 0; i < 4; ++i) {
      EXPECT_EQ(dst[i * 4 + 0], 30) << c.format;
      EXPECT_EQ(dst[i * 4 + 1], 20) << c.format;
      EXPECT_EQ(dst[i * 4 + 2], 10) << c.format;
      EXPECT_EQ(dst[i * 4 + 3], 255) << c.format;
    }
  }
}

// A fake screen: a PipeWire video source of 320x240 RGBx with a moving white
// square, damage metadata around it and cursor metadata (an 8x8 red cursor).
class FakeScreen {
 public:
  static constexpr int kW = 320, kH = 240;

  bool Start(std::string* error) {
    pw_init(nullptr, nullptr);
    loop_ = pw_thread_loop_new("fake-screen", nullptr);
    context_ = pw_context_new(pw_thread_loop_get_loop(loop_), nullptr, 0);
    if (!context_) {
      *error = "no PipeWire client configuration (PipeWire is not installed)";
      return false;
    }
    pw_thread_loop_start(loop_);
    pw_thread_loop_lock(loop_);
    core_ = pw_context_connect(context_, nullptr, 0);
    if (!core_) {
      pw_thread_loop_unlock(loop_);
      *error = "no PipeWire daemon";
      return false;
    }
    stream_ = pw_stream_new(core_, "rdaccel-fake-screen",
                            pw_properties_new(PW_KEY_MEDIA_CLASS, "Video/Source", PW_KEY_MEDIA_TYPE, "Video",
                                              PW_KEY_MEDIA_CATEGORY, "Source", nullptr));
    static const pw_stream_events events = [] {
      pw_stream_events e{};
      e.version = PW_VERSION_STREAM_EVENTS;
      e.state_changed = [](void* d, pw_stream_state, pw_stream_state s, const char*) {
        auto* self = static_cast<FakeScreen*>(d);
        self->state_ = s;
        pw_thread_loop_signal(self->loop_, false);
      };
      e.param_changed = [](void* d, uint32_t id, const spa_pod* p) {
        if (id == SPA_PARAM_Format && p) static_cast<FakeScreen*>(d)->OnFormat();
      };
      e.process = [](void* d) { static_cast<FakeScreen*>(d)->Produce(); };
      return e;
    }();
    pw_stream_add_listener(stream_, &listener_, &events, this);
    uint8_t buf[1024];
    spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
    spa_rectangle size{kW, kH};
    spa_fraction rate{30, 1};
    const spa_pod* params[1] = {static_cast<const spa_pod*>(spa_pod_builder_add_object(
        &b, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat, SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
        SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw), SPA_FORMAT_VIDEO_format,
        SPA_POD_Id(SPA_VIDEO_FORMAT_RGBx), SPA_FORMAT_VIDEO_size, SPA_POD_Rectangle(&size),
        SPA_FORMAT_VIDEO_framerate, SPA_POD_Fraction(&rate)))};
    pw_stream_connect(stream_, PW_DIRECTION_OUTPUT, PW_ID_ANY,
                      static_cast<pw_stream_flags>(PW_STREAM_FLAG_DRIVER | PW_STREAM_FLAG_MAP_BUFFERS),
                      params, 1);
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (state_ != PW_STREAM_STATE_PAUSED && state_ != PW_STREAM_STATE_STREAMING &&
           std::chrono::steady_clock::now() < deadline) {
      pw_thread_loop_timed_wait(loop_, 1);
    }
    node_ = pw_stream_get_node_id(stream_);
    // Drive: one frame every 20 ms.
    timer_ = pw_loop_add_timer(
        pw_thread_loop_get_loop(loop_),
        [](void* d, uint64_t) {
          auto* self = static_cast<FakeScreen*>(d);
          if (self->state_ == PW_STREAM_STATE_STREAMING) pw_stream_trigger_process(self->stream_);
        },
        this);
    timespec interval{0, 20'000'000};
    pw_loop_update_timer(pw_thread_loop_get_loop(loop_), timer_, &interval, &interval, false);
    pw_thread_loop_unlock(loop_);
    if (node_ == SPA_ID_INVALID) {
      *error = "fake screen did not get a node id";
      return false;
    }
    return true;
  }

  ~FakeScreen() {
    if (!loop_) return;
    pw_thread_loop_stop(loop_);
    if (stream_) pw_stream_destroy(stream_);
    if (core_) pw_core_disconnect(core_);
    if (context_) pw_context_destroy(context_);
    pw_thread_loop_destroy(loop_);
  }

  uint32_t node() const { return node_; }
  int frames() const { return frames_; }

 private:
  void OnFormat() {
    uint8_t buf[1024];
    spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
    const spa_pod* params[4];
    params[0] = static_cast<const spa_pod*>(spa_pod_builder_add_object(
        &b, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers, SPA_PARAM_BUFFERS_buffers, SPA_POD_Int(4),
        SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1), SPA_PARAM_BUFFERS_size, SPA_POD_Int(kW * kH * 4),
        SPA_PARAM_BUFFERS_stride, SPA_POD_Int(kW * 4), SPA_PARAM_BUFFERS_dataType,
        SPA_POD_CHOICE_FLAGS_Int((1 << SPA_DATA_MemFd) | (1 << SPA_DATA_MemPtr))));
    params[1] = static_cast<const spa_pod*>(spa_pod_builder_add_object(
        &b, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta, SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
        SPA_PARAM_META_size, SPA_POD_Int(sizeof(spa_meta_header))));
    params[2] = static_cast<const spa_pod*>(spa_pod_builder_add_object(
        &b, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta, SPA_PARAM_META_type, SPA_POD_Id(SPA_META_VideoDamage),
        SPA_PARAM_META_size,
        SPA_POD_CHOICE_RANGE_Int(sizeof(spa_meta_region) * 16, sizeof(spa_meta_region), sizeof(spa_meta_region) * 16)));
    constexpr int kCursorMeta = sizeof(spa_meta_cursor) + sizeof(spa_meta_bitmap) + 8 * 8 * 4;
    params[3] = static_cast<const spa_pod*>(spa_pod_builder_add_object(
        &b, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta, SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Cursor),
        SPA_PARAM_META_size, SPA_POD_CHOICE_RANGE_Int(kCursorMeta, kCursorMeta, 256 * 256 * 4 + 64)));
    pw_stream_update_params(stream_, params, 4);
  }

  void Produce() {
    pw_buffer* pb = pw_stream_dequeue_buffer(stream_);
    if (!pb) return;
    spa_buffer* buf = pb->buffer;
    spa_data& d = buf->datas[0];
    if (!d.data) {
      pw_stream_queue_buffer(stream_, pb);
      return;
    }
    // Background R=200 G=100 B=50; a white 16x16 square moving right.
    auto* px = static_cast<uint8_t*>(d.data);
    for (int i = 0; i < kW * kH; ++i) {
      px[i * 4 + 0] = 200;
      px[i * 4 + 1] = 100;
      px[i * 4 + 2] = 50;
      px[i * 4 + 3] = 0;
    }
    const int sx = (frames_ * 8) % (kW - 16), sy = 40;
    for (int y = sy; y < sy + 16; ++y) std::memset(px + (y * kW + sx) * 4, 255, 16 * 4);
    d.chunk->offset = 0;
    d.chunk->stride = kW * 4;
    d.chunk->size = kW * kH * 4;
    if (spa_meta* m = spa_buffer_find_meta(buf, SPA_META_VideoDamage)) {
      auto* r = static_cast<spa_meta_region*>(m->data);
      const size_t n = m->size / sizeof(spa_meta_region);
      // Old and new square position, as one rect (wraps are rare; report all then).
      const int prev = frames_ == 0 ? -1 : ((frames_ - 1) * 8) % (kW - 16);
      if (prev < 0 || prev > sx) {
        r[0].region = SPA_REGION(0, 0, kW, kH);
      } else {
        r[0].region = SPA_REGION(prev, sy, static_cast<uint32_t>(sx - prev + 16), 16);
      }
      if (n > 1) r[1].region = SPA_REGION(0, 0, 0, 0);
    }
    constexpr size_t kCursorBytes = sizeof(spa_meta_cursor) + sizeof(spa_meta_bitmap) + 8 * 8 * 4;
    if (auto* c = static_cast<spa_meta_cursor*>(spa_buffer_find_meta_data(buf, SPA_META_Cursor, kCursorBytes))) {
      c->id = 1;
      c->flags = 0;
      c->position = SPA_POINT(100 + frames_ % 10, 50);
      c->hotspot = SPA_POINT(1, 2);
      c->bitmap_offset = sizeof(spa_meta_cursor);
      auto* bm = SPA_PTROFF(c, c->bitmap_offset, spa_meta_bitmap);
      bm->format = SPA_VIDEO_FORMAT_RGBA;
      bm->size = SPA_RECTANGLE(8, 8);
      bm->stride = 8 * 4;
      bm->offset = sizeof(spa_meta_bitmap);
      auto* bits = SPA_PTROFF(bm, bm->offset, uint8_t);
      for (int i = 0; i < 64; ++i) {
        bits[i * 4 + 0] = 255;
        bits[i * 4 + 1] = 0;
        bits[i * 4 + 2] = 0;
        bits[i * 4 + 3] = 255;
      }
    }
    ++frames_;
    pw_stream_queue_buffer(stream_, pb);
  }

  pw_thread_loop* loop_ = nullptr;
  pw_context* context_ = nullptr;
  pw_core* core_ = nullptr;
  pw_stream* stream_ = nullptr;
  spa_hook listener_{};
  spa_source* timer_ = nullptr;
  std::atomic<pw_stream_state> state_{PW_STREAM_STATE_UNCONNECTED};
  uint32_t node_ = SPA_ID_INVALID;
  std::atomic<int> frames_{0};
};

struct Received {
  std::mutex mu;
  std::condition_variable cv;
  int frames = 0;
  int32_t w = 0, h = 0;
  uint8_t first_px[4] = {};
  uint8_t center_px[4] = {};
  bool first_full = false;
  int partial = 0;  // frames with dirty rects
  bool dirty_in_bounds = true;

  capture::FrameFn Fn() {
    return [this](const capture::CapturedFrame& f) {
      std::lock_guard<std::mutex> lock(mu);
      if (frames == 0) first_full = f.dirty.empty();
      w = f.bgra.width;
      h = f.bgra.height;
      std::memcpy(first_px, f.bgra.data, 4);
      std::memcpy(center_px,
                  f.bgra.data + static_cast<ptrdiff_t>(f.bgra.height / 2) * f.bgra.stride + (f.bgra.width / 2) * 4, 4);
      if (!f.dirty.empty()) ++partial;
      for (const Rect& r : f.dirty) {
        if (r.x < 0 || r.y < 0 || r.w <= 0 || r.h <= 0 || r.x + r.w > f.bgra.width || r.y + r.h > f.bgra.height) {
          dirty_in_bounds = false;
        }
      }
      ++frames;
      cv.notify_all();
    };
  }
  bool WaitFrames(int n, std::chrono::seconds t = 10s) {
    std::unique_lock<std::mutex> lock(mu);
    return cv.wait_for(lock, t, [&] { return frames >= n; });
  }
};

TEST(PipeWire, ReadsAFakeScreenWithDamageAndCursor) {
  FakeScreen screen;
  std::string err;
  if (!screen.Start(&err)) GTEST_SKIP() << err;

  platform::PipeWireCapturer cap(screen.node());
  Received rx;
  capture::CaptureConfig cfg;
  cfg.fps = 30;
  ASSERT_TRUE(cap.Start(cfg, rx.Fn())) << cap.last_error();
  ASSERT_TRUE(rx.WaitFrames(10)) << "frames: " << rx.frames << ", produced " << screen.frames();
  {
    std::lock_guard<std::mutex> lock(rx.mu);
    EXPECT_EQ(rx.w, FakeScreen::kW);
    EXPECT_EQ(rx.h, FakeScreen::kH);
    EXPECT_TRUE(rx.first_full);  // nothing to diff against yet
    // RGBx 200,100,50 arrives as BGRA 50,100,200.
    EXPECT_EQ(rx.first_px[0], 50);
    EXPECT_EQ(rx.first_px[1], 100);
    EXPECT_EQ(rx.first_px[2], 200);
    EXPECT_GT(rx.partial, 0) << "damage metadata was not used";
    EXPECT_TRUE(rx.dirty_in_bounds);
  }

  std::string cerr;
  auto cursor = platform::CreatePipeWireCursorSource(&cerr);
  ASSERT_TRUE(cursor) << cerr;
  capture::CursorSample s;
  bool got = false;
  for (int i = 0; i < 100 && !got; ++i) {
    got = cursor->Poll(s);
    if (!got) std::this_thread::sleep_for(10ms);
  }
  ASSERT_TRUE(got);
  EXPECT_TRUE(s.visible);
  EXPECT_GE(s.x, 100);
  EXPECT_EQ(s.y, 50);
  EXPECT_EQ(s.screen_width, FakeScreen::kW);
  ASSERT_TRUE(s.image_changed);
  EXPECT_EQ(s.image.width, 8);
  EXPECT_EQ(s.image.hotspot_x, 1);
  EXPECT_EQ(s.image.hotspot_y, 2);
  ASSERT_EQ(s.image.bgra.size(), 8u * 8 * 4);
  EXPECT_EQ(s.image.bgra[0], 0);    // B
  EXPECT_EQ(s.image.bgra[2], 255);  // R
  EXPECT_EQ(s.image.bgra[3], 255);  // A
  // The same image is not reported again.
  ASSERT_TRUE(cursor->Poll(s));
  EXPECT_FALSE(s.image_changed);

  cap.Stop();
  EXPECT_FALSE(platform::CreatePipeWireCursorSource(&cerr));
}

TEST(PipeWire, ScalesTheCapture) {
  FakeScreen screen;
  std::string err;
  if (!screen.Start(&err)) GTEST_SKIP() << err;
  platform::PipeWireCapturer cap(screen.node());
  Received rx;
  capture::CaptureConfig cfg;
  cfg.width = 160;
  cfg.height = 120;
  ASSERT_TRUE(cap.Start(cfg, rx.Fn())) << cap.last_error();
  ASSERT_TRUE(rx.WaitFrames(3));
  std::lock_guard<std::mutex> lock(rx.mu);
  EXPECT_EQ(rx.w, 160);
  EXPECT_EQ(rx.h, 120);
  EXPECT_EQ(rx.partial, 0);  // damage is not scaled: whole frames
}

TEST(PipeWire, StartsAgain) {
  FakeScreen screen;
  std::string err;
  if (!screen.Start(&err)) GTEST_SKIP() << err;
  platform::PipeWireCapturer cap(screen.node());
  for (int round = 0; round < 3; ++round) {
    Received rx;
    ASSERT_TRUE(cap.Start({}, rx.Fn())) << "round " << round << ": " << cap.last_error();
    EXPECT_TRUE(rx.WaitFrames(3)) << "round " << round;
    cap.Stop();
  }
}

TEST(PipeWire, NoSuchNodeFails) {
  FakeScreen screen;  // only to know that a daemon is there
  std::string err;
  if (!screen.Start(&err)) GTEST_SKIP() << err;
  platform::PipeWireCapturer cap(999999);
  Received rx;
  EXPECT_FALSE(cap.Start({}, rx.Fn()));
  EXPECT_FALSE(cap.last_error().empty());
}

// --- the real thing: portal + compositor (sway in CI) -----------------------

bool HaveSway() { return std::getenv("SWAYSOCK") && std::getenv("WAYLAND_DISPLAY"); }

// Paints the whole screen a colour not used before (wlroots sends a picture
// only when the screen changes) and returns it as 0xRRGGBB.
uint32_t NewBackground() {
  static uint32_t n = static_cast<uint32_t>(getpid()) * 7;
  ++n;
  const uint32_t rgb = ((40 + 37 * n) % 200 + 30) << 16 | ((90 + 53 * n) % 200 + 30) << 8 | ((10 + 71 * n) % 200 + 30);
  char cmd[128];
  std::snprintf(cmd, sizeof(cmd), "swaymsg -q output HEADLESS-1 bg '#%06x' solid_color", rgb);
  EXPECT_EQ(std::system(cmd), 0) << cmd;
  return rgb;
}

bool WaitForCenter(Received& rx, uint32_t rgb, std::chrono::seconds t = 10s) {
  std::unique_lock<std::mutex> lock(rx.mu);
  return rx.cv.wait_for(lock, t, [&] {
    auto near = [](uint8_t a, uint32_t e) { return std::abs(int(a) - int(e & 0xff)) <= 2; };
    return near(rx.center_px[0], rgb) && near(rx.center_px[1], rgb >> 8) && near(rx.center_px[2], rgb >> 16);
  });
}

TEST(PipeWirePortal, CapturesTheWaylandScreen) {
  if (!HaveSway()) GTEST_SKIP() << "not in a sway session (run under tools/wayland_headless.sh)";
  ASSERT_TRUE(platform::UseWaylandCapture());
  const uint32_t before = NewBackground();
  std::string err;
  auto cap = platform::CreateScreenCapturer(&err);
  ASSERT_TRUE(cap) << err;
  Received rx;
  capture::CaptureConfig cfg;
  ASSERT_TRUE(cap->Start(cfg, rx.Fn())) << cap->last_error();
  // The screen as it is when sharing starts.
  EXPECT_TRUE(WaitForCenter(rx, before));
  {
    std::lock_guard<std::mutex> lock(rx.mu);
    EXPECT_EQ(rx.w, 1280);
    EXPECT_EQ(rx.h, 720);
  }
  // Changes arrive as new frames.
  for (int i = 0; i < 3; ++i) EXPECT_TRUE(WaitForCenter(rx, NewBackground())) << i;
  cap->Stop();

  std::vector<capture::DisplayInfo> displays;
  ASSERT_TRUE(platform::ListDisplays(displays, &err)) << err;
  ASSERT_EQ(displays.size(), 1u);
  EXPECT_EQ(displays[0].width, 1280);
  EXPECT_EQ(displays[0].height, 720);
}

TEST(PipeWirePortal, RejectsOtherDisplays) {
  if (!HaveSway()) GTEST_SKIP() << "not in a sway session (run under tools/wayland_headless.sh)";
  std::string err;
  auto cap = platform::CreateScreenCapturer(&err);
  ASSERT_TRUE(cap) << err;
  Received rx;
  capture::CaptureConfig cfg;
  cfg.display_index = 1;
  EXPECT_FALSE(cap->Start(cfg, rx.Fn()));
  EXPECT_NE(cap->last_error().find("display_index"), std::string::npos);
}

TEST(PipeWirePortal, ScaledCapture) {
  if (!HaveSway()) GTEST_SKIP() << "not in a sway session (run under tools/wayland_headless.sh)";
  std::string err;
  auto cap = platform::CreateScreenCapturer(&err);
  Received rx;
  capture::CaptureConfig cfg;
  cfg.width = 640;
  cfg.height = 360;
  ASSERT_TRUE(cap->Start(cfg, rx.Fn())) << cap->last_error();
  EXPECT_TRUE(WaitForCenter(rx, NewBackground()));
  std::lock_guard<std::mutex> lock(rx.mu);
  EXPECT_EQ(rx.w, 640);
  EXPECT_EQ(rx.h, 360);
}

}  // namespace
