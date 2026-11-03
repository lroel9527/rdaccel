// X11 capture, display selection and XTest input injection, against a real X server (CI runs these
// under Xvfb; without DISPLAY they skip).

#include <gtest/gtest.h>

#include <chrono>
#include <thread>
#include <condition_variable>
#include <mutex>
#include <vector>

#include "linux/capture_x11.h"
#include "platform.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

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

// A focused window that records the input events it gets.
class InputTarget {
 public:
  InputTarget() : dpy_(XOpenDisplay(nullptr)) {
    if (!dpy_) return;
    XSetWindowAttributes a{};
    a.override_redirect = True;
    a.event_mask = KeyPressMask | ButtonPressMask | ButtonReleaseMask | StructureNotifyMask;
    w_ = XCreateWindow(dpy_, DefaultRootWindow(dpy_), 0, 0, 600, 400, 0, CopyFromParent, InputOnly,
                       CopyFromParent, CWOverrideRedirect | CWEventMask, &a);
    XMapRaised(dpy_, w_);
    XEvent ev;
    do XWindowEvent(dpy_, w_, StructureNotifyMask, &ev); while (ev.type != MapNotify);
    XSetInputFocus(dpy_, w_, RevertToParent, CurrentTime);
    XSync(dpy_, False);
  }
  ~InputTarget() {
    if (dpy_) XCloseDisplay(dpy_);
  }
  bool ok() const { return dpy_ != nullptr; }
  Display* dpy() const { return dpy_; }

  // Events until `n` have come or a second has passed. Keeps the keyboard
  // mapping current, as a real client would.
  std::vector<XEvent> Collect(size_t n) {
    std::vector<XEvent> out;
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (out.size() < n && std::chrono::steady_clock::now() < end) {
      while (XPending(dpy_) && out.size() < n) {
        XEvent ev;
        XNextEvent(dpy_, &ev);
        if (ev.type == MappingNotify) XRefreshKeyboardMapping(&ev.xmapping);
        if (ev.type == KeyPress || ev.type == ButtonPress || ev.type == ButtonRelease) out.push_back(ev);
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return out;
  }

 private:
  Display* dpy_;
  Window w_ = 0;
};

std::pair<int, int> Pointer(Display* dpy) {
  Window root, child;
  int rx = 0, ry = 0, wx = 0, wy = 0;
  unsigned mask = 0;
  XQueryPointer(dpy, DefaultRootWindow(dpy), &root, &child, &rx, &ry, &wx, &wy, &mask);
  return {rx, ry};
}

InputEvent Ev(InputKind kind, int32_t x = 0, int32_t y = 0, uint32_t code = 0) {
  InputEvent e;
  e.kind = kind;
  e.x = x;
  e.y = y;
  e.code = code;
  return e;
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

TEST(PlatformX11, InjectsPointerMovesScaledFromTheCaptureSize) {
  InputTarget target;
  std::string err;
  auto inj = platform::CreateInputInjector({}, &err);
  if (!target.ok() || !inj) GTEST_SKIP() << err;
  ASSERT_TRUE(inj->Inject(Ev(InputKind::kMouseMove, 123, 45), &err)) << err;
  EXPECT_EQ(Pointer(target.dpy()), std::make_pair(123, 45));

  // The viewer sees a half-size picture: its coordinates are doubled.
  std::vector<capture::DisplayInfo> d;
  ASSERT_TRUE(platform::ListDisplays(d, &err)) << err;
  auto half = platform::CreateInputInjector({0, d[0].width / 2, d[0].height / 2}, &err);
  ASSERT_NE(half, nullptr) << err;
  ASSERT_TRUE(half->Inject(Ev(InputKind::kMouseMove, 100, 50), &err)) << err;
  EXPECT_EQ(Pointer(target.dpy()), std::make_pair(200, 100));
  // Outside the picture: kept on the display.
  ASSERT_TRUE(half->Inject(Ev(InputKind::kMouseMove, -10, 1 << 20), &err)) << err;
  EXPECT_EQ(Pointer(target.dpy()), std::make_pair(0, d[0].height - 1));

  EXPECT_EQ(platform::CreateInputInjector({static_cast<int32_t>(d.size()), 0, 0}, &err), nullptr);
  EXPECT_NE(err.find("no display"), std::string::npos) << err;
}

TEST(PlatformX11, InjectsClicksAndScrolling) {
  InputTarget target;
  std::string err;
  auto inj = platform::CreateInputInjector({}, &err);
  if (!target.ok() || !inj) GTEST_SKIP() << err;
  ASSERT_TRUE(inj->Inject(Ev(InputKind::kMouseDown, 50, 60, kButtonLeft), &err)) << err;
  ASSERT_TRUE(inj->Inject(Ev(InputKind::kMouseUp, 50, 60, kButtonLeft), &err)) << err;
  ASSERT_TRUE(inj->Inject(Ev(InputKind::kMouseDown, 70, 80, kButtonRight), &err)) << err;
  ASSERT_TRUE(inj->Inject(Ev(InputKind::kMouseUp, 70, 80, kButtonRight), &err)) << err;
  InputEvent scroll = Ev(InputKind::kScroll, 10, 10);
  scroll.delta_y = -2;  // down, two lines
  ASSERT_TRUE(inj->Inject(scroll, &err)) << err;
  EXPECT_FALSE(inj->Inject(Ev(InputKind::kMouseDown, 0, 0, 99), &err));

  const auto evs = target.Collect(8);
  ASSERT_EQ(evs.size(), 8u);
  EXPECT_EQ(evs[0].type, ButtonPress);
  EXPECT_EQ(evs[0].xbutton.button, 1u);
  EXPECT_EQ(evs[0].xbutton.x, 50);
  EXPECT_EQ(evs[0].xbutton.y, 60);
  EXPECT_EQ(evs[1].type, ButtonRelease);
  EXPECT_EQ(evs[2].xbutton.button, 3u);
  EXPECT_EQ(evs[2].xbutton.x, 70);
  for (size_t i = 4; i < 8; ++i) EXPECT_EQ(evs[i].xbutton.button, 5u) << i;
}

TEST(PlatformX11, InjectsKeysAndTypesAnyCharacter) {
  InputTarget target;
  std::string err;
  auto inj = platform::CreateInputInjector({}, &err);
  if (!target.ok() || !inj) GTEST_SKIP() << err;
  // A physical key (HID 0x04 = A), then text: plain, shifted, and two
  // characters no US layout has.
  ASSERT_TRUE(inj->Inject(Ev(InputKind::kKeyDown, 0, 0, 0x04), &err)) << err;
  ASSERT_TRUE(inj->Inject(Ev(InputKind::kKeyUp, 0, 0, 0x04), &err)) << err;
  for (uint32_t c : {0x62u, 0x5Au, 0x4E2Du, 0x1F600u}) {
    ASSERT_TRUE(inj->Inject(Ev(InputKind::kText, 0, 0, c), &err)) << std::hex << c << ": " << err;
  }
  EXPECT_FALSE(inj->Inject(Ev(InputKind::kText, 0, 0, 0x01), &err));  // a control character

  std::vector<KeySym> typed;
  for (int tries = 0; tries < 20 && typed.size() < 5; ++tries) {
    for (XEvent& ev : target.Collect(1)) {
      const KeySym ks = XLookupKeysym(&ev.xkey, (ev.xkey.state & ShiftMask) ? 1 : 0);
      if (ks != XK_Shift_L && ks != XK_Shift_R) typed.push_back(ks);
    }
  }
  ASSERT_EQ(typed.size(), 5u);
  EXPECT_EQ(typed[0], static_cast<KeySym>(XK_a));
  EXPECT_EQ(typed[1], static_cast<KeySym>(XK_b));
  EXPECT_EQ(typed[2], static_cast<KeySym>(XK_Z));
  EXPECT_EQ(typed[3], static_cast<KeySym>(0x01004E2D));
  EXPECT_EQ(typed[4], static_cast<KeySym>(0x0101F600));
}
