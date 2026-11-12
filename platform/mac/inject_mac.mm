// Input injection on macOS with CGEvent, posted at the HID level. Needs the
// Accessibility permission (System Settings > Privacy & Security >
// Accessibility).

#import <Carbon/Carbon.h>
#import <CoreGraphics/CoreGraphics.h>
#import <IOKit/pwr_mgt/IOPMLib.h>
#include <dispatch/dispatch.h>
#include <pthread.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <atomic>
#include <mutex>
#include <thread>

#include "log/log.h"
#include "platform.h"
#include "mac/displays_mac.h"
#include "rdacore/keycodes.h"

namespace rdacore::platform {
namespace {

// CGMouseButton numbers: 0 left, 1 right, 2 middle, 3 back, 4 forward.
int MacButton(uint32_t button) {
  switch (button) {
    case kButtonLeft: return 0;
    case kButtonRight: return 1;
    case kButtonMiddle: return 2;
    case kButtonBack: return 3;
    case kButtonForward: return 4;
  }
  return -1;
}

CGEventFlags Flags(uint32_t mods) {
  CGEventFlags f = 0;
  if (mods & kModShift) f |= kCGEventFlagMaskShift;
  if (mods & kModCtrl) f |= kCGEventFlagMaskControl;
  if (mods & kModAlt) f |= kCGEventFlagMaskAlternate;
  if (mods & kModMeta) f |= kCGEventFlagMaskCommand;
  if (mods & kModCaps) f |= kCGEventFlagMaskAlphaShift;
  return f;
}

// Tells the power manager a user is active, at most once a second. Posted
// events alone do not wake a display that is off: on a locked Mac the
// display turns off after a while and the lock screen then takes no posted
// keys until something turns it back on. This turns it on with the first
// injected event, and keeps the Mac from idling into display sleep or the
// screen lock while someone works remotely, as for a local user.
void DeclareUserActivity() {
  static std::atomic<int64_t> last_ms{0};
  static std::mutex mu;
  static IOPMAssertionID id = kIOPMNullAssertionID;
  const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now().time_since_epoch())
                          .count();
  int64_t last = last_ms.load(std::memory_order_relaxed);
  if (last != 0 && now - last < 1000) return;
  if (!last_ms.compare_exchange_strong(last, now)) return;  // another thread does it
  std::lock_guard lock(mu);
  const IOReturn r = IOPMAssertionDeclareUserActivity(CFSTR("rdaccel remote input"), kIOPMUserActiveLocal, &id);
  if (r != kIOReturnSuccess) log::Debug("input: IOPMAssertionDeclareUserActivity failed: " + std::to_string(r));
}

void Post(CGEventRef ev) {
  if (!ev) return;
  DeclareUserActivity();
  CGEventPost(kCGHIDEventTap, ev);
  CFRelease(ev);
}

// Runs `fn` on the main thread (the Text Input Sources API must not be used
// on another one) and waits at most `timeout_ms`. False when the main thread
// does not run a run loop: `fn` may then still run later, so it may only
// touch what it owns.
bool RunOnMain(std::function<void()> fn, int timeout_ms) {
  if (pthread_main_np()) {
    fn();
    return true;
  }
  dispatch_semaphore_t done = dispatch_semaphore_create(0);
  auto task = std::make_shared<std::function<void()>>(std::move(fn));
  dispatch_async(dispatch_get_main_queue(), ^{
    (*task)();
    dispatch_semaphore_signal(done);
  });
  return dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, int64_t{timeout_ms} * NSEC_PER_MSEC)) == 0;
}

// An input method (Pinyin, Kana, ...) as opposed to a plain keyboard layout.
bool IsInputMethod(TISInputSourceRef src) {
  if (!src) return false;
  CFStringRef type = static_cast<CFStringRef>(TISGetInputSourceProperty(src, kTISPropertyInputSourceType));
  return type && (CFEqual(type, kTISTypeKeyboardInputMode) || CFEqual(type, kTISTypeKeyboardInputMethodWithoutModes) ||
                  CFEqual(type, kTISTypeKeyboardInputMethodModeEnabled));
}

// While an input method is the current input source it takes the injected
// characters as keystrokes of a composition (or drops them while one is
// open). So text is typed with an ASCII-capable layout selected, and the
// user's input method comes back once text has stopped for a while (key
// events go through it as on the real keyboard) or the injector goes away.
// Shared with blocks that may outlive the injector (see RunOnMain).
struct InputSourceSwitch {
  std::mutex mu;
  TISInputSourceRef saved = nullptr;  // the input method we switched away from
  TISInputSourceRef ours = nullptr;   // the layout we selected

  // Main thread. True if it switched just now.
  bool LeaveInputMethod() {
    std::lock_guard<std::mutex> lock(mu);
    if (saved) return false;
    TISInputSourceRef cur = TISCopyCurrentKeyboardInputSource();
    if (!IsInputMethod(cur)) {
      if (cur) CFRelease(cur);
      return false;
    }
    TISInputSourceRef ascii = TISCopyCurrentASCIICapableKeyboardLayoutInputSource();
    if (!ascii || TISSelectInputSource(ascii) != noErr) {
      if (ascii) CFRelease(ascii);
      CFRelease(cur);
      return false;
    }
    saved = cur;
    ours = ascii;
    return true;
  }

  // Main thread. Gives the input method back, unless the user picked
  // another source meanwhile.
  void Restore() {
    std::lock_guard<std::mutex> lock(mu);
    if (!saved) return;
    TISInputSourceRef cur = TISCopyCurrentKeyboardInputSource();
    if (cur && CFEqual(cur, ours)) TISSelectInputSource(saved);
    if (cur) CFRelease(cur);
    CFRelease(saved);
    CFRelease(ours);
    saved = ours = nullptr;
  }

  bool switched() {
    std::lock_guard<std::mutex> lock(mu);
    return saved != nullptr;
  }

  // Main thread. Drops the remembered input method without selecting it
  // again: the application picks the input source itself now.
  void Forget() {
    std::lock_guard<std::mutex> lock(mu);
    if (!saved) return;
    CFRelease(saved);
    CFRelease(ours);
    saved = ours = nullptr;
  }
};

std::string CFString(CFTypeRef v) {
  if (!v || CFGetTypeID(v) != CFStringGetTypeID()) return {};
  const CFStringRef s = static_cast<CFStringRef>(v);
  if (const char* fast = CFStringGetCStringPtr(s, kCFStringEncodingUTF8)) return fast;
  const CFIndex max = CFStringGetMaximumSizeForEncoding(CFStringGetLength(s), kCFStringEncodingUTF8) + 1;
  std::string out(static_cast<size_t>(max), '\0');
  if (!CFStringGetCString(s, out.data(), max, kCFStringEncodingUTF8)) return {};
  out.resize(std::strlen(out.c_str()));
  return out;
}

InputSourceInfo Describe(TISInputSourceRef src) {
  InputSourceInfo info;
  info.id = CFString(TISGetInputSourceProperty(src, kTISPropertyInputSourceID));
  info.name = CFString(TISGetInputSourceProperty(src, kTISPropertyLocalizedName));
  auto langs = static_cast<CFArrayRef>(TISGetInputSourceProperty(src, kTISPropertyInputSourceLanguages));
  if (langs && CFGetTypeID(langs) == CFArrayGetTypeID() && CFArrayGetCount(langs) > 0) {
    info.language = CFString(CFArrayGetValueAtIndex(langs, 0));
  }
  info.input_method = IsInputMethod(src);
  auto ascii = static_cast<CFBooleanRef>(TISGetInputSourceProperty(src, kTISPropertyInputSourceIsASCIICapable));
  info.ascii_capable = ascii && CFGetTypeID(ascii) == CFBooleanGetTypeID() && CFBooleanGetValue(ascii);
  return info;
}

// Enabled keyboard input sources the user can select (no palettes such as
// the emoji viewer). Main thread. `id` empty = all of them.
CFArrayRef CopySelectable(const std::string& id) {
  CFMutableDictionaryRef filter =
      CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
  CFDictionarySetValue(filter, kTISPropertyInputSourceCategory, kTISCategoryKeyboardInputSource);
  CFDictionarySetValue(filter, kTISPropertyInputSourceIsSelectCapable, kCFBooleanTrue);
  CFStringRef wanted = nullptr;
  if (!id.empty()) {
    wanted = CFStringCreateWithCString(nullptr, id.c_str(), kCFStringEncodingUTF8);
    if (wanted) CFDictionarySetValue(filter, kTISPropertyInputSourceID, wanted);
  }
  CFArrayRef list = TISCreateInputSourceList(filter, false);
  if (wanted) CFRelease(wanted);
  CFRelease(filter);
  return list;
}

constexpr int kSourcesTimeoutMs = 500;

class MacInjector final : public IInputInjector {
 public:
  // src_w, src_h: the size of the picture the viewer's positions are in;
  // 0 = the display's own pixels, whatever its mode is at the time.
  MacInjector(CGDirectDisplayID id, int32_t src_w, int32_t src_h) : id_(id), src_w_(src_w), src_h_(src_h) {}

  ~MacInjector() override {
    // Release what the viewer left pressed.
    for (int b = 0; b < 5; ++b) {
      if (held_ & (1u << b)) Post(MouseEvent(false, b, last_));
    }
    if (ime_->switched()) RunOnMain([ime = ime_] { ime->Restore(); }, kMainTimeoutMs);
  }

  bool Inject(const InputEvent& ev, std::string* error) override {
    std::lock_guard<std::mutex> lock(mu_);
    switch (ev.kind) {
      case InputKind::kMouseMove: {
        last_ = Point(ev.x, ev.y);
        // With a button held, a move is a drag.
        CGEventType type = kCGEventMouseMoved;
        CGMouseButton button = kCGMouseButtonLeft;
        if (held_ & 1u) {
          type = kCGEventLeftMouseDragged;
        } else if (held_ & 2u) {
          type = kCGEventRightMouseDragged;
          button = kCGMouseButtonRight;
        } else if (held_) {
          type = kCGEventOtherMouseDragged;
          button = static_cast<CGMouseButton>(__builtin_ctz(held_));
        }
        CGEventRef e = CGEventCreateMouseEvent(nullptr, type, last_, button);
        if (e) CGEventSetFlags(e, Flags(ev.modifiers));
        Post(e);
        return true;
      }
      case InputKind::kMouseDown:
      case InputKind::kMouseUp: {
        const int b = MacButton(ev.code);
        if (b < 0) {
          if (error) *error = "unknown mouse button " + std::to_string(ev.code);
          return false;
        }
        const bool down = ev.kind == InputKind::kMouseDown;
        last_ = Point(ev.x, ev.y);
        if (down) CountClick(b);
        CGEventRef e = MouseEvent(down, b, last_);
        if (e) CGEventSetFlags(e, Flags(ev.modifiers));
        Post(e);
        if (down) {
          held_ |= 1u << b;
        } else {
          held_ &= ~(1u << b);
        }
        return true;
      }
      case InputKind::kScroll: {
        // Lines: y > 0 up, x > 0 right (CGEvent's wheel 2 is positive left).
        CGEventRef e = CGEventCreateScrollWheelEvent2(nullptr, kCGScrollEventUnitLine, 2, ev.delta_y,
                                                      -ev.delta_x, 0);
        if (e) {
          CGEventSetLocation(e, Point(ev.x, ev.y));
          CGEventSetFlags(e, Flags(ev.modifiers));
        }
        Post(e);
        return true;
      }
      case InputKind::kKeyDown:
      case InputKind::kKeyUp: {
        if (ev.kind == InputKind::kKeyDown && ime_->switched() &&
            std::chrono::steady_clock::now() - last_text_ > kTextIdle) {
          RunOnMain([ime = ime_] { ime->Restore(); }, kMainTimeoutMs);
        }
        const uint32_t vk = HidToMacKeyCode(ev.code);
        if (vk == kNoKeyCode) {
          if (error) *error = "no macOS key for HID usage " + std::to_string(ev.code);
          return false;
        }
        CGEventRef e = CGEventCreateKeyboardEvent(nullptr, static_cast<CGKeyCode>(vk),
                                                  ev.kind == InputKind::kKeyDown);
        if (e) CGEventSetFlags(e, Flags(ev.modifiers));
        Post(e);
        return true;
      }
      case InputKind::kText: {
        LeaveInputMethod();
        last_text_ = std::chrono::steady_clock::now();
        // The character itself rides on the event, whatever key 0 is.
        UniChar utf16[2];
        UniCharCount n = 1;
        const uint32_t c = ev.code;
        if (c >= 0x10000) {
          utf16[0] = static_cast<UniChar>(0xD800 + ((c - 0x10000) >> 10));
          utf16[1] = static_cast<UniChar>(0xDC00 + ((c - 0x10000) & 0x3FF));
          n = 2;
        } else {
          utf16[0] = static_cast<UniChar>(c);
        }
        for (bool down : {true, false}) {
          CGEventRef e = CGEventCreateKeyboardEvent(nullptr, 0, down);
          if (e) {
            CGEventSetFlags(e, 0);
            CGEventKeyboardSetUnicodeString(e, n, utf16);
          }
          Post(e);
        }
        return true;
      }
    }
    if (error) *error = "unknown input kind";
    return false;
  }

  bool SelectInputSource(const std::string& id, std::string* error) override {
    struct Result {
      bool ok = false;
      std::string error;
    };
    auto result = std::make_shared<Result>();
    const bool answered = RunOnMain(
        [ime = ime_, id, result] {
          // Text typing may have stepped away from an input method; the
          // application's choice replaces what it would have restored.
          ime->Forget();
          CFArrayRef list = CopySelectable(id);
          if (!list || CFArrayGetCount(list) == 0) {
            result->error = "input source " + id + " is not enabled on this computer";
          } else {
            auto src = static_cast<TISInputSourceRef>(const_cast<void*>(CFArrayGetValueAtIndex(list, 0)));
            const OSStatus st = TISSelectInputSource(src);
            result->ok = st == noErr;
            if (!result->ok) result->error = "TISSelectInputSource failed (" + std::to_string(st) + ")";
          }
          if (list) CFRelease(list);
        },
        kSourcesTimeoutMs);
    if (!answered) {
      if (error) *error = "the main thread does not run a run loop";
      return false;
    }
    if (!result->ok && error) *error = result->error;
    return result->ok;
  }

 private:
  static constexpr int kMainTimeoutMs = 200;
  static constexpr auto kTextIdle = std::chrono::seconds(1);

  void LeaveInputMethod() {
    if (!main_ok_ || ime_->switched()) return;
    bool now = false;
    auto flag = std::make_shared<std::atomic<bool>>(false);
    main_ok_ = RunOnMain([ime = ime_, flag] { *flag = ime->LeaveInputMethod(); }, kMainTimeoutMs);
    now = *flag;
    if (!main_ok_) {
      log::Warn("input: the main thread runs no run loop, so text is injected without leaving the input method "
                "(an active one such as Pinyin may swallow it)");
      return;
    }
    // Let the switch land before the first character.
    if (now) std::this_thread::sleep_for(std::chrono::milliseconds(30));
  }

  // Viewer pixels -> global points on this display. The display's mode is
  // read each time: it may change while the session runs (a resized virtual
  // display, a new resolution chosen in System Settings).
  CGPoint Point(int32_t x, int32_t y) const {
    const CGRect b = CGDisplayBounds(id_);
    int32_t w = src_w_, h = src_h_;
    if (w <= 0 || h <= 0) {
      const double scale = MacDisplayScale(id_);
      w = std::max(1, static_cast<int32_t>(std::lround(b.size.width * scale)));
      h = std::max(1, static_cast<int32_t>(std::lround(b.size.height * scale)));
    }
    const double px = std::clamp<double>(x, 0, w - 1), py = std::clamp<double>(y, 0, h - 1);
    return CGPointMake(b.origin.x + px * b.size.width / w, b.origin.y + py * b.size.height / h);
  }

  CGEventRef MouseEvent(bool down, int b, CGPoint at) {
    CGEventType type;
    if (b == 0) {
      type = down ? kCGEventLeftMouseDown : kCGEventLeftMouseUp;
    } else if (b == 1) {
      type = down ? kCGEventRightMouseDown : kCGEventRightMouseUp;
    } else {
      type = down ? kCGEventOtherMouseDown : kCGEventOtherMouseUp;
    }
    CGEventRef e = CGEventCreateMouseEvent(nullptr, type, at, static_cast<CGMouseButton>(b));
    if (e) {
      CGEventSetIntegerValueField(e, kCGMouseEventButtonNumber, b);
      CGEventSetIntegerValueField(e, kCGMouseEventClickState, clicks_);
    }
    return e;
  }

  // Double and triple clicks: macOS counts them from the click state field.
  void CountClick(int b) {
    const auto now = std::chrono::steady_clock::now();
    const bool again = b == last_button_ && now - last_click_ < std::chrono::milliseconds(500) &&
                       std::abs(last_.x - click_at_.x) < 4 && std::abs(last_.y - click_at_.y) < 4;
    clicks_ = again ? clicks_ + 1 : 1;
    last_button_ = b;
    last_click_ = now;
    click_at_ = last_;
  }

  std::mutex mu_;
  std::shared_ptr<InputSourceSwitch> ime_ = std::make_shared<InputSourceSwitch>();
  bool main_ok_ = true;  // false once the main thread failed to answer
  std::chrono::steady_clock::time_point last_text_{};
  const CGDirectDisplayID id_;
  int32_t src_w_ = 1, src_h_ = 1;
  CGPoint last_ = CGPointZero;
  uint32_t held_ = 0;  // bit per CGMouseButton
  int64_t clicks_ = 1;
  int last_button_ = -1;
  std::chrono::steady_clock::time_point last_click_{};
  CGPoint click_at_ = CGPointZero;
};

}  // namespace

bool ListInputSources(std::vector<InputSourceInfo>& out, std::string* error) {
  auto all = std::make_shared<std::vector<InputSourceInfo>>();
  const bool answered = RunOnMain(
      [all] {
        CFArrayRef list = CopySelectable({});
        if (!list) return;
        for (CFIndex i = 0; i < CFArrayGetCount(list); ++i) {
          all->push_back(Describe(static_cast<TISInputSourceRef>(const_cast<void*>(CFArrayGetValueAtIndex(list, i)))));
        }
        CFRelease(list);
      },
      kSourcesTimeoutMs);
  if (!answered) {
    if (error) *error = "the main thread does not run a run loop";
    return false;
  }
  out = *all;
  return true;
}

bool CurrentInputSource(InputSourceInfo& out, std::string* error) {
  auto cur = std::make_shared<InputSourceInfo>();
  const bool answered = RunOnMain(
      [cur] {
        TISInputSourceRef src = TISCopyCurrentKeyboardInputSource();
        if (!src) return;
        *cur = Describe(src);
        CFRelease(src);
      },
      kSourcesTimeoutMs);
  if (!answered) {
    if (error) *error = "the main thread does not run a run loop";
    return false;
  }
  if (cur->id.empty()) {
    if (error) *error = "no current keyboard input source";
    return false;
  }
  out = *cur;
  return true;
}

std::unique_ptr<IInputInjector> CreateInputInjector(const InjectorConfig& cfg, std::string* error) {
  if (cfg.source_width < 0 || cfg.source_height < 0) {
    if (error) *error = "negative source size";
    return nullptr;
  }
  CGDirectDisplayID id = 0;
  if (!MacDisplayId(cfg.display_index, id, error)) return nullptr;
  // Without the permission CGEventPost drops events silently; say so up front.
  if (!CGPreflightPostEventAccess()) {
    if (error) {
      *error = "no permission to post input events (System Settings > Privacy & Security > Accessibility)";
    }
    return nullptr;
  }
  return std::make_unique<MacInjector>(id, cfg.source_width, cfg.source_height);
}

}  // namespace rdacore::platform
