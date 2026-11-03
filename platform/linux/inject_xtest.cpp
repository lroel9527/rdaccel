// Input injection on Linux through the XTest extension.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <vector>

#include "platform.h"
#include "linux/displays_x11.h"
#include "rdacore/keycodes.h"

// After our headers: Xlib defines macros such as None and Status.
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XTest.h>
#include <X11/keysym.h>

namespace rdacore::platform {
namespace {

// X core pointer buttons: 1 left, 2 middle, 3 right, 4..7 the wheel, 8 back,
// 9 forward.
unsigned XButton(uint32_t button) {
  switch (button) {
    case kButtonLeft: return 1;
    case kButtonMiddle: return 2;
    case kButtonRight: return 3;
    case kButtonBack: return 8;
    case kButtonForward: return 9;
  }
  return 0;
}

// The keysym that types `c`, or NoSymbol.
KeySym KeysymFor(uint32_t c) {
  switch (c) {
    case 0x08: return XK_BackSpace;
    case 0x09: return XK_Tab;
    case 0x0A:
    case 0x0D: return XK_Return;
    case 0x1B: return XK_Escape;
    case 0x7F: return XK_Delete;
  }
  if (c < 0x20 || (c >= 0x80 && c < 0xA0)) return NoSymbol;  // other control characters
  if (c < 0x100) return static_cast<KeySym>(c);              // Latin-1 keysyms are the code point
  return static_cast<KeySym>(0x01000000u | c);               // Unicode keysyms
}

class XTestInjector final : public IInputInjector {
 public:
  XTestInjector(Display* dpy, const capture::DisplayInfo& d, int32_t src_w, int32_t src_h)
      : dpy_(dpy), d_(d), src_w_(src_w > 0 ? src_w : d.width), src_h_(src_h > 0 ? src_h : d.height) {
    XDisplayKeycodes(dpy_, &min_kc_, &max_kc_);
    shift_ = XKeysymToKeycode(dpy_, XK_Shift_L);
  }

  ~XTestInjector() override {
    // Release what the viewer left pressed, and give back the borrowed keys.
    for (unsigned b : held_buttons_) XTestFakeButtonEvent(dpy_, b, False, CurrentTime);
    for (KeyCode k : held_keys_) XTestFakeKeyEvent(dpy_, k, False, CurrentTime);
    for (const auto& s : scratch_) {
      KeySym none[2] = {NoSymbol, NoSymbol};
      XChangeKeyboardMapping(dpy_, s.keycode, 2, none, 1);
    }
    XSync(dpy_, False);
    XCloseDisplay(dpy_);
  }

  bool Inject(const InputEvent& ev, std::string* error) override {
    std::lock_guard<std::mutex> lock(mu_);
    bool ok = true;
    switch (ev.kind) {
      case InputKind::kMouseMove: Move(ev.x, ev.y); break;
      case InputKind::kMouseDown:
      case InputKind::kMouseUp: {
        const unsigned b = XButton(ev.code);
        if (!b) {
          if (error) *error = "unknown mouse button " + std::to_string(ev.code);
          return false;
        }
        const bool down = ev.kind == InputKind::kMouseDown;
        Move(ev.x, ev.y);
        XTestFakeButtonEvent(dpy_, b, down ? True : False, CurrentTime);
        Track(held_buttons_, b, down);
        break;
      }
      case InputKind::kScroll:
        // One wheel click per unit: y > 0 up (4), y < 0 down (5); x > 0
        // right (7), x < 0 left (6).
        Move(ev.x, ev.y);
        Wheel(ev.delta_y > 0 ? 4 : 5, ev.delta_y);
        Wheel(ev.delta_x > 0 ? 7 : 6, ev.delta_x);
        break;
      case InputKind::kKeyDown:
      case InputKind::kKeyUp: {
        const uint32_t evdev = HidToEvdevKeyCode(ev.code);
        const int kc = evdev == kNoKeyCode ? 0 : static_cast<int>(evdev) + 8;
        if (kc < min_kc_ || kc > max_kc_) {
          if (error) *error = "no X key for HID usage " + std::to_string(ev.code);
          return false;
        }
        const bool down = ev.kind == InputKind::kKeyDown;
        XTestFakeKeyEvent(dpy_, static_cast<KeyCode>(kc), down ? True : False, CurrentTime);
        Track(held_keys_, static_cast<KeyCode>(kc), down);
        break;
      }
      case InputKind::kText: ok = Type(ev.code, error); break;
    }
    // A round trip: the events are in effect once this returns.
    XSync(dpy_, False);
    return ok;
  }

 private:
  struct Scratch {
    KeyCode keycode;
    KeySym keysym;
  };

  template <class T>
  static void Track(std::vector<T>& held, T v, bool down) {
    auto it = std::find(held.begin(), held.end(), v);
    if (down && it == held.end()) held.push_back(v);
    if (!down && it != held.end()) held.erase(it);
  }

  void Move(int32_t x, int32_t y) {
    // Viewer pixels -> X screen pixels on this display.
    const int sx = d_.x + static_cast<int>(std::clamp<int64_t>(int64_t{x} * d_.width / src_w_, 0, d_.width - 1));
    const int sy = d_.y + static_cast<int>(std::clamp<int64_t>(int64_t{y} * d_.height / src_h_, 0, d_.height - 1));
    XTestFakeMotionEvent(dpy_, DefaultScreen(dpy_), sx, sy, CurrentTime);
  }

  void Wheel(unsigned button, int32_t delta) {
    const int32_t n = std::min<int32_t>(std::abs(delta), 100);
    for (int32_t i = 0; i < n; ++i) {
      XTestFakeButtonEvent(dpy_, button, True, CurrentTime);
      XTestFakeButtonEvent(dpy_, button, False, CurrentTime);
    }
  }

  void Tap(KeyCode kc, bool shift) {
    if (shift && shift_) XTestFakeKeyEvent(dpy_, shift_, True, CurrentTime);
    XTestFakeKeyEvent(dpy_, kc, True, CurrentTime);
    XTestFakeKeyEvent(dpy_, kc, False, CurrentTime);
    if (shift && shift_) XTestFakeKeyEvent(dpy_, shift_, False, CurrentTime);
  }

  bool Type(uint32_t c, std::string* error) {
    const KeySym ks = KeysymFor(c);
    if (ks == NoSymbol) {
      if (error) *error = "cannot type U+" + Hex(c);
      return false;
    }
    // A key of the current layout that makes it, on level 1 or 2 (Shift).
    int per = 0;
    const int count = max_kc_ - min_kc_ + 1;
    KeySym* map = XGetKeyboardMapping(dpy_, static_cast<KeyCode>(min_kc_), count, &per);
    if (!map) {
      if (error) *error = "cannot read the keyboard mapping";
      return false;
    }
    std::vector<KeyCode> unused;
    for (int i = 0; i < count; ++i) {
      const KeySym* row = map + static_cast<size_t>(i) * per;
      for (int level = 0; level < std::min(per, 2); ++level) {
        if (row[level] == ks) {
          XFree(map);
          Tap(static_cast<KeyCode>(min_kc_ + i), level == 1);
          return true;
        }
      }
      if (std::all_of(row, row + per, [](KeySym s) { return s == NoSymbol; })) {
        unused.push_back(static_cast<KeyCode>(min_kc_ + i));
      }
    }
    XFree(map);
    // None: borrow a key with no symbols and map the character onto it. The
    // last few borrowed keys keep their mapping, because the receiving
    // client looks the key up only when it handles the event.
    if (scratch_.size() < kScratchKeys) {
      for (KeyCode kc : unused) {
        if (std::none_of(scratch_.begin(), scratch_.end(), [&](const Scratch& s) { return s.keycode == kc; })) {
          scratch_.push_back({kc, NoSymbol});
          break;
        }
      }
    }
    if (scratch_.empty()) {
      if (error) *error = "no free key to type U+" + Hex(c) + " with";
      return false;
    }
    Scratch& s = scratch_[next_scratch_++ % scratch_.size()];
    KeySym syms[2] = {ks, ks};
    XChangeKeyboardMapping(dpy_, s.keycode, 2, syms, 1);
    XSync(dpy_, False);
    s.keysym = ks;
    Tap(s.keycode, false);
    return true;
  }

  static std::string Hex(uint32_t c) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04X", c);
    return buf;
  }

  static constexpr size_t kScratchKeys = 8;

  std::mutex mu_;
  Display* dpy_;
  const capture::DisplayInfo d_;
  const int32_t src_w_, src_h_;
  int min_kc_ = 8, max_kc_ = 255;
  KeyCode shift_ = 0;
  std::vector<unsigned> held_buttons_;
  std::vector<KeyCode> held_keys_;
  std::vector<Scratch> scratch_;
  size_t next_scratch_ = 0;
};

}  // namespace

std::unique_ptr<IInputInjector> CreateInputInjector(const InjectorConfig& cfg, std::string* error) {
  Display* dpy = XOpenDisplay(nullptr);
  if (!dpy) {
    if (error) *error = "cannot open X display (is DISPLAY set?)";
    return nullptr;
  }
  int ev = 0, err = 0, major = 0, minor = 0;
  if (!XTestQueryExtension(dpy, &ev, &err, &major, &minor)) {
    if (error) *error = "X server lacks the XTEST extension";
    XCloseDisplay(dpy);
    return nullptr;
  }
  capture::DisplayInfo d;
  if (!X11Display(dpy, cfg.display_index, d, error)) {
    XCloseDisplay(dpy);
    return nullptr;
  }
  if (cfg.source_width < 0 || cfg.source_height < 0) {
    if (error) *error = "negative source size";
    XCloseDisplay(dpy);
    return nullptr;
  }
  return std::make_unique<XTestInjector>(dpy, d, cfg.source_width, cfg.source_height);
}

}  // namespace rdacore::platform
