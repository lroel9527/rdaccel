// X11 capture and display selection, against a real X server (CI runs these
// under Xvfb; without DISPLAY they skip).

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <vector>

#include "linux/capture_x11.h"
#include "platform.h"

#include <X11/Xlib.h>

using namespace rdacore;

namespace {

// Solid-colour windows on the X screen, kept while this lives (an X server
// may reset its screen when the last client leaves).
class Painter {
 public:
  Painter() : dpy_(XOpenDisplay(nullptr)) {}
  ~Painter() {
    if (dpy_) XCloseDisplay(dpy_);
  }
  bool ok() const { return dpy_ != nullptr; }
  void Fill(Rect r, unsigned long rgb) {
    XSetWindowAttributes a{};
    a.override_redirect = True;
    a.background_pixel = rgb;
    const Window w = XCreateWindow(dpy_, DefaultRootWindow(dpy_), r.x, r.y, static_cast<unsigned>(r.w),
                                   static_cast<unsigned>(r.h), 0, CopyFromParent, InputOutput, CopyFromParent,
                                   CWOverrideRedirect | CWBackPixel, &a);
    XMapRaised(dpy_, w);
    XSync(dpy_, False);
  }

 private:
  Display* dpy_;
};

struct Grab {
  int32_t w = 0, h = 0;
  std::vector<uint8_t> bgra;
};

bool CaptureOne(capture::ICapturer& cap, const capture::CaptureConfig& cfg, Grab& out, std::string* err) {
  std::mutex mu;
  std::condition_variable cv;
  bool got = false;
  if (!cap.Start(cfg, [&](const capture::CapturedFrame& f) {
        std::lock_guard<std::mutex> lock(mu);
        if (got) return;
        out.w = f.bgra.width;
        out.h = f.bgra.height;
        out.bgra.resize(static_cast<size_t>(f.bgra.width) * f.bgra.height * 4);
        for (int32_t y = 0; y < f.bgra.height; ++y) {
          std::copy_n(f.bgra.data + static_cast<size_t>(y) * f.bgra.stride, f.bgra.width * 4,
                      out.bgra.data() + static_cast<size_t>(y) * f.bgra.width * 4);
        }
        got = true;
        cv.notify_all();
      })) {
    if (err) *err = cap.last_error();
    return false;
  }
  std::unique_lock<std::mutex> lock(mu);
  const bool ok = cv.wait_for(lock, std::chrono::seconds(5), [&] { return got; });
  lock.unlock();
  cap.Stop();
  return ok;
}

uint32_t PixelRgb(const Grab& g, int32_t x, int32_t y) {
  const uint8_t* p = &g.bgra[(static_cast<size_t>(y) * g.w + x) * 4];
  return (uint32_t{p[2]} << 16) | (uint32_t{p[1]} << 8) | p[0];
}

}  // namespace

TEST(PlatformX11, CapturesTheChosenAreaOnly) {
  Painter paint;
  if (!paint.ok()) GTEST_SKIP() << "no X display";
  // Red inside the area, blue just left of it.
  paint.Fill({40, 20, 64, 32}, 0xff0000);
  paint.Fill({20, 20, 20, 32}, 0x0000ff);
  platform::X11Capturer cap(Rect{40, 20, 64, 32});
  Grab g;
  std::string err;
  ASSERT_TRUE(CaptureOne(cap, {}, g, &err)) << err;
  EXPECT_EQ(g.w, 64);
  EXPECT_EQ(g.h, 32);
  EXPECT_EQ(PixelRgb(g, 0, 0), 0xff0000u);
  EXPECT_EQ(PixelRgb(g, 63, 31), 0xff0000u);

  // One pixel further left is the blue window.
  platform::X11Capturer wider(Rect{38, 20, 66, 32});
  ASSERT_TRUE(CaptureOne(wider, {}, g, &err)) << err;
  EXPECT_EQ(PixelRgb(g, 0, 0), 0x0000ffu);
  EXPECT_EQ(PixelRgb(g, 2, 0), 0xff0000u);
}

TEST(PlatformX11, DisplayIndexPicksTheDisplay) {
  std::vector<capture::DisplayInfo> d;
  if (!platform::ListDisplays(d, nullptr)) GTEST_SKIP() << "no X display";
  platform::X11Capturer cap;
  capture::CaptureConfig cfg;
  Grab g;
  std::string err;
  ASSERT_TRUE(CaptureOne(cap, cfg, g, &err)) << err;
  EXPECT_EQ(g.w, d[0].width & ~1);
  EXPECT_EQ(g.h, d[0].height & ~1);

  cfg.display_index = static_cast<int32_t>(d.size());  // one past the last
  platform::X11Capturer none;
  EXPECT_FALSE(CaptureOne(none, cfg, g, &err));
  EXPECT_NE(err.find("no display"), std::string::npos) << err;
  EXPECT_EQ(platform::CreateCursorSource(cfg.display_index, &err), nullptr);
}

TEST(PlatformX11, CursorIsRelativeToTheDisplay) {
  std::string err;
  auto src = platform::CreateCursorSource(0, &err);
  if (!src) GTEST_SKIP() << err;
  Display* dpy = XOpenDisplay(nullptr);
  ASSERT_NE(dpy, nullptr);
  XWarpPointer(dpy, None, DefaultRootWindow(dpy), 0, 0, 0, 0, 30, 40);
  XSync(dpy, False);
  XCloseDisplay(dpy);
  capture::CursorSample s;
  ASSERT_TRUE(src->Poll(s));
  EXPECT_EQ(s.x, 30);
  EXPECT_EQ(s.y, 40);
  EXPECT_TRUE(s.visible);
}
