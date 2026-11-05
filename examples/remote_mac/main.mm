// rdaccel_remote: a windowed viewer for macOS that controls another machine.
// Connects to rdaccel_serve (run it with --inject on the other machine),
// shows the remote screen in a window, and sends this window's mouse,
// scroll wheel and keyboard back. Uses only the public API
// (rdaccel/rdaccel.hpp).
//
//   rdaccel_remote HOST [--udp=PORT | --tcp=PORT] (--key=<64 hex digits> | --plaintext)
//
// Over the picture the pointer is this Mac's own, drawn by the system the
// moment the mouse moves, in the remote cursor's shape: waiting for the
// remote cursor's position would add a network round trip to every move.
// When the pointer is outside the window, the remote cursor is drawn where
// the remote machine has it.
//
// Pictures go into two IOSurfaces that Core Animation shows directly; each
// frame copies only the areas that changed.
//
// Keys are sent as physical keys (USB HID codes), so the remote machine's
// own keyboard layout and input method decide what is typed; Command
// shortcuts go to the remote machine too. Close the window to quit.
//
// Once per second the title bar and stdout show: shown fps, received kbps,
// decode time, and two delays measured on this machine's clock alone:
//   pointer   from sending a pointer move to the remote cursor arriving at
//             that position (network round trip + the host's cursor poll);
//   input→screen  from a key press, click or scroll on a quiet screen to the
//             first picture that changed (round trip + capture + encode +
//             decode). Inputs while the screen is busy are not counted:
//             the next changed picture would not be their result.
//
// The library plays and captures no sound, and neither does this program.

#import <AppKit/AppKit.h>
#import <IOSurface/IOSurface.h>
#import <QuartzCore/QuartzCore.h>

#include <rdaccel/rdaccel.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "../args.h"

namespace {

rdaccel::examples::Args* g_args = nullptr;

uint32_t Modifiers(NSEventModifierFlags f) {
  uint32_t m = 0;
  if (f & NSEventModifierFlagShift) m |= rdaccel::kModShift;
  if (f & NSEventModifierFlagControl) m |= rdaccel::kModCtrl;
  if (f & NSEventModifierFlagOption) m |= rdaccel::kModAlt;
  if (f & NSEventModifierFlagCommand) m |= rdaccel::kModMeta;
  if (f & NSEventModifierFlagCapsLock) m |= rdaccel::kModCaps;
  return m;
}

// The modifier flag that a modifier key's keyCode (kVK_*) changes, or 0.
NSEventModifierFlags FlagOfModifierKey(unsigned short keyCode) {
  switch (keyCode) {
    case 56: case 60: return NSEventModifierFlagShift;    // kVK_Shift, kVK_RightShift
    case 59: case 62: return NSEventModifierFlagControl;  // kVK_Control, kVK_RightControl
    case 58: case 61: return NSEventModifierFlagOption;   // kVK_Option, kVK_RightOption
    case 55: case 54: return NSEventModifierFlagCommand;  // kVK_Command, kVK_RightCommand
    case 57: return NSEventModifierFlagCapsLock;          // kVK_CapsLock
    case 63: return NSEventModifierFlagFunction;          // kVK_Function
  }
  return 0;
}

uint32_t ButtonOf(NSEvent* e) {
  switch (e.buttonNumber) {
    case 0: return rdaccel::kButtonLeft;
    case 1: return rdaccel::kButtonRight;
    case 2: return rdaccel::kButtonMiddle;
    case 3: return rdaccel::kButtonBack;
    case 4: return rdaccel::kButtonForward;
  }
  return 0;
}

IOSurfaceRef MakeSurface(int32_t w, int32_t h) {
  NSDictionary* props = @{
    (id)kIOSurfaceWidth : @(w),
    (id)kIOSurfaceHeight : @(h),
    (id)kIOSurfaceBytesPerElement : @4,
    (id)kIOSurfacePixelFormat : @(0x42475241u),  // 'BGRA'
  };
  IOSurfaceRef s = IOSurfaceCreate((__bridge CFDictionaryRef)props);
  if (s) IOSurfaceSetValue(s, kIOSurfaceColorSpace, kCGColorSpaceSRGB);
  return s;
}

// Delay samples of one second: "-" or "avg ms (max, n)".
NSString* Summary(std::vector<double>& v) {
  if (v.empty()) return @"-";
  double sum = 0, mx = 0;
  for (double x : v) {
    sum += x;
    mx = std::max(mx, x);
  }
  NSString* s = [NSString stringWithFormat:@"%.0f ms (max %.0f, n=%zu)", sum / v.size(), mx, v.size()];
  v.clear();
  return s;
}

}  // namespace

@class AppDelegate;

// The picture, the remote cursor on top, and all local input.
@interface RemoteView : NSView
@property(nonatomic, weak) AppDelegate* app;
@property(nonatomic) BOOL pointerInside;
@end

@interface AppDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate>
- (void)sendEvent:(const rdaccel::InputEvent&)ev;
- (BOOL)streamPoint:(NSPoint)p x:(int32_t*)x y:(int32_t*)y;
- (void)releaseHeld;
- (NSCursor*)remoteCursor;
- (void)placeCursor;
@end

@implementation RemoteView {
  NSTrackingArea* _tracking;
  double _scrollX, _scrollY;  // fractions of a line not sent yet
}

- (BOOL)acceptsFirstResponder {
  return YES;
}
- (BOOL)acceptsFirstMouse:(NSEvent*)e {
  return YES;
}

- (void)updateTrackingAreas {
  [super updateTrackingAreas];
  if (_tracking) [self removeTrackingArea:_tracking];
  _tracking = [[NSTrackingArea alloc]
      initWithRect:self.bounds
           options:NSTrackingMouseEnteredAndExited | NSTrackingMouseMoved | NSTrackingActiveInKeyWindow |
                   NSTrackingInVisibleRect
             owner:self
          userInfo:nil];
  [self addTrackingArea:_tracking];
}

// The system pointer, in the remote cursor's shape, over the whole view.
- (void)resetCursorRects {
  NSCursor* c = [self.app remoteCursor];
  [self addCursorRect:self.bounds cursor:c ?: NSCursor.arrowCursor];
}

- (void)mouseEntered:(NSEvent*)e {
  self.pointerInside = YES;
  [self.app placeCursor];
}
- (void)mouseExited:(NSEvent*)e {
  self.pointerInside = NO;
  [self.app placeCursor];
}

- (void)pointer:(NSEvent*)e kind:(rdaccel::InputKind)kind {
  rdaccel::InputEvent ev;
  ev.kind = kind;
  const NSPoint p = [self convertPoint:e.locationInWindow fromView:nil];
  if (![self.app streamPoint:p x:&ev.x y:&ev.y]) return;
  if (kind != rdaccel::InputKind::kMouseMove) ev.code = ButtonOf(e);
  if (kind != rdaccel::InputKind::kMouseMove && ev.code == 0) return;
  ev.modifiers = Modifiers(e.modifierFlags);
  [self.app sendEvent:ev];
}

- (void)mouseMoved:(NSEvent*)e { [self pointer:e kind:rdaccel::InputKind::kMouseMove]; }
- (void)mouseDragged:(NSEvent*)e { [self pointer:e kind:rdaccel::InputKind::kMouseMove]; }
- (void)rightMouseDragged:(NSEvent*)e { [self pointer:e kind:rdaccel::InputKind::kMouseMove]; }
- (void)otherMouseDragged:(NSEvent*)e { [self pointer:e kind:rdaccel::InputKind::kMouseMove]; }
- (void)mouseDown:(NSEvent*)e { [self pointer:e kind:rdaccel::InputKind::kMouseDown]; }
- (void)mouseUp:(NSEvent*)e { [self pointer:e kind:rdaccel::InputKind::kMouseUp]; }
- (void)rightMouseDown:(NSEvent*)e { [self pointer:e kind:rdaccel::InputKind::kMouseDown]; }
- (void)rightMouseUp:(NSEvent*)e { [self pointer:e kind:rdaccel::InputKind::kMouseUp]; }
- (void)otherMouseDown:(NSEvent*)e { [self pointer:e kind:rdaccel::InputKind::kMouseDown]; }
- (void)otherMouseUp:(NSEvent*)e { [self pointer:e kind:rdaccel::InputKind::kMouseUp]; }

// The wire carries whole lines (wheel notches), y > 0 up. A trackpad gives
// points; about 10 points make a line. Fractions are carried over.
- (void)scrollWheel:(NSEvent*)e {
  rdaccel::InputEvent ev;
  ev.kind = rdaccel::InputKind::kScroll;
  const NSPoint p = [self convertPoint:e.locationInWindow fromView:nil];
  if (![self.app streamPoint:p x:&ev.x y:&ev.y]) return;
  const double per_line = e.hasPreciseScrollingDeltas ? 10.0 : 1.0;
  _scrollX += -e.scrollingDeltaX / per_line;  // AppKit: x > 0 is left
  _scrollY += e.scrollingDeltaY / per_line;
  ev.delta_x = static_cast<int32_t>(std::trunc(_scrollX));
  ev.delta_y = static_cast<int32_t>(std::trunc(_scrollY));
  _scrollX -= ev.delta_x;
  _scrollY -= ev.delta_y;
  if (e.phase == NSEventPhaseEnded || e.momentumPhase == NSEventPhaseEnded) _scrollX = _scrollY = 0;
  if (ev.delta_x == 0 && ev.delta_y == 0) return;
  ev.modifiers = Modifiers(e.modifierFlags);
  [self.app sendEvent:ev];
}

@end

@implementation AppDelegate {
  NSWindow* _window;
  RemoteView* _view;
  CALayer* _layer;
  CALayer* _cursorLayer;
  CGImageRef _cursorImage;       // owned, main thread
  NSCursor* _nsCursor;           // the same image as a system pointer
  rdaccel::CursorState _cursor;  // main thread
  int32_t _cursorW, _cursorH, _hotX, _hotY;
  int32_t _streamW, _streamH;  // main thread
  NSTimer* _timer;
  id _keyMonitor;

  std::unique_ptr<rdaccel::Viewer> _viewer;
  std::set<uint32_t> _heldKeys;     // HID codes, main thread
  std::set<uint32_t> _heldButtons;  // main thread
  int32_t _lastX, _lastY;

  // Two surfaces: the decode thread fills the one not on screen with what
  // changed since it was last filled, the main thread shows it.
  std::mutex _surfMu;  // guards the four below (decode thread vs resize)
  IOSurfaceRef _surf[2];
  int32_t _surfW, _surfH;
  std::vector<rdaccel::Rect> _owed[2];  // changed since that surface was filled
  int _front;                           // the surface on screen, -1 = none
  std::atomic<bool> _presentPending;
  std::atomic<uint64_t> _presented;
  std::atomic<uint64_t> _lastChangeUs;  // the last picture that changed

  // Delays (see the top of the file).
  std::mutex _latencyMu;
  std::vector<double> _pointerMs, _screenMs;
  uint64_t _moveSentUs;  // under _latencyMu: the oldest unanswered move
  int32_t _moveX, _moveY;
  uint64_t _inputSentUs;  // under _latencyMu: a quiet-screen input waiting
  rdaccel::ViewerStats _last;
  bool _connected;
}

- (void)applicationDidFinishLaunching:(NSNotification*)note {
  _front = -1;
  rdaccel::examples::Args& args = *g_args;
  const std::string host = args.positional();
  if (host.empty()) {
    std::fprintf(stderr, "usage: rdaccel_remote HOST [--udp=PORT | --tcp=PORT] (--key=<64 hex> | --plaintext)\n");
    [NSApp terminate:nil];
    return;
  }
  rdaccel::ViewerOptions options;
  if (!args.key("key", options.encryption_key)) {
    [NSApp terminate:nil];
    return;
  }
  options.allow_plaintext = args.has("plaintext");
  if (options.encryption_key.empty() && !options.allow_plaintext) {
    std::fprintf(stderr, "pass --key=<64 hex digits> (the same on both sides) or --plaintext\n");
    [NSApp terminate:nil];
    return;
  }
  const bool tcp = args.has("tcp");
  const auto port = static_cast<uint16_t>(args.num(tcp ? "tcp" : "udp", 7300));

  _window = [[NSWindow alloc] initWithContentRect:NSMakeRect(80, 80, 1280, 800)
                                        styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                  NSWindowStyleMaskResizable |
                                                  NSWindowStyleMaskMiniaturizable
                                          backing:NSBackingStoreBuffered
                                            defer:NO];
  _window.title = [NSString stringWithFormat:@"rdaccel remote: connecting to %s ...", host.c_str()];
  _window.delegate = self;
  _window.acceptsMouseMovedEvents = YES;
  _view = [[RemoteView alloc] initWithFrame:_window.contentView.bounds];
  _view.app = self;
  _view.wantsLayer = YES;
  _window.contentView = _view;
  _layer = _view.layer;
  _layer.backgroundColor = NSColor.blackColor.CGColor;
  _layer.contentsGravity = kCAGravityResizeAspect;
  _cursorLayer = [CALayer layer];
  _cursorLayer.anchorPoint = CGPointZero;
  _cursorLayer.hidden = YES;
  [_layer addSublayer:_cursorLayer];
  [_window makeKeyAndOrderFront:nil];
  [_window makeFirstResponder:_view];
  [NSApp activateIgnoringOtherApps:YES];

  // Keys go through a local monitor rather than keyDown: it also sees
  // Command shortcuts (and their key-up, which AppKit otherwise swallows)
  // and keeps the system from beeping at keys no view handles.
  __weak AppDelegate* weakSelf = self;
  _keyMonitor = [NSEvent
      addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown | NSEventMaskKeyUp | NSEventMaskFlagsChanged
                                   handler:^NSEvent*(NSEvent* e) {
                                     AppDelegate* me = weakSelf;
                                     if (!me || e.window != me->_window) return e;
                                     [me key:e];
                                     return nil;
                                   }];

  std::string err;
  auto transport = tcp ? rdaccel::ConnectTcp(host, port, &err) : rdaccel::ConnectUdp(host, port, &err);
  if (!transport) {
    [self fail:[NSString stringWithUTF8String:err.c_str()]];
    return;
  }
  rdaccel::ViewerHandlers handlers;
  handlers.on_frame = [weakSelf](const rdaccel::ImageView& img, std::span<const rdaccel::Rect> changed,
                                 const rdaccel::FrameInfo&) {
    [weakSelf presentFrame:img changed:changed];
  };
  handlers.on_stream_info = [weakSelf](int32_t w, int32_t h) {
    dispatch_async(dispatch_get_main_queue(), ^{
      [weakSelf streamSizeChanged:w height:h];
    });
  };
  handlers.on_cursor = [weakSelf](const rdaccel::CursorState& c) {
    // Runs on the transport thread; the image pointer is only valid now.
    [weakSelf cursorArrivedAt:c.x y:c.y];
    CGImageRef image = nullptr;
    if (c.image_changed && c.image) image = [AppDelegate makeCursorImage:*c.image];
    rdaccel::CursorState state = c;
    state.image = nullptr;
    const int32_t iw = c.image ? c.image->width : -1, ih = c.image ? c.image->height : -1;
    const int32_t hx = c.image ? c.image->hotspot_x : 0, hy = c.image ? c.image->hotspot_y : 0;
    dispatch_async(dispatch_get_main_queue(), ^{
      AppDelegate* me = weakSelf;
      if (!me) {
        if (image) CGImageRelease(image);
        return;
      }
      [me cursorChanged:state image:image width:iw height:ih hotX:hx hotY:hy];
    });
  };
  handlers.on_state = [weakSelf](rdaccel::ConnectionState st, rda_status reason) {
    dispatch_async(dispatch_get_main_queue(), ^{
      AppDelegate* me = weakSelf;
      if (!me) return;
      if (st == rdaccel::ConnectionState::kConnected) {
        me->_connected = true;
        std::printf("connected\n");
        std::fflush(stdout);
      } else if (st == rdaccel::ConnectionState::kDisconnected) {
        [me fail:[NSString stringWithFormat:@"disconnected: %s", rda_status_str(reason)]];
      }
    });
  };
  _viewer = rdaccel::Viewer::Create(std::move(transport), std::move(handlers), options, &err);
  if (!_viewer) {
    [self fail:[NSString stringWithUTF8String:err.c_str()]];
    return;
  }
  _timer = [NSTimer scheduledTimerWithTimeInterval:1.0
                                            target:self
                                          selector:@selector(tick)
                                          userInfo:nil
                                           repeats:YES];
}

// Main thread. Sizes the window to the stream once, at most 85% of the screen.
- (void)streamSizeChanged:(int32_t)w height:(int32_t)h {
  if (w == _streamW && h == _streamH) return;
  const bool first = _streamW == 0;
  _streamW = w;
  _streamH = h;
  std::printf("stream %dx%d\n", w, h);
  std::fflush(stdout);
  [self rebuildCursor];
  if (!first || w <= 0 || h <= 0) return;
  const NSRect screen = (_window.screen ?: NSScreen.mainScreen).visibleFrame;
  const double scale = _window.backingScaleFactor > 0 ? _window.backingScaleFactor : 1;
  double cw = w / scale, ch = h / scale;  // one stream pixel per backing pixel
  const double fit = std::min({1.0, screen.size.width * 0.85 / cw, screen.size.height * 0.85 / ch});
  cw *= fit;
  ch *= fit;
  [_window setContentSize:NSMakeSize(cw, ch)];
  _window.contentAspectRatio = NSMakeSize(w, h);
  [_window center];
}

// View points per stream pixel, and the picture's offset in the view
// (aspect-fitted). False before the stream starts.
- (BOOL)fitScale:(double*)s ox:(double*)ox oy:(double*)oy {
  const NSSize b = _view.bounds.size;
  if (_streamW <= 0 || _streamH <= 0 || b.width <= 0 || b.height <= 0) return NO;
  *s = std::min(b.width / _streamW, b.height / _streamH);
  *ox = (b.width - _streamW * *s) / 2;
  *oy = (b.height - _streamH * *s) / 2;
  return YES;
}

// Converts a point in the view to stream pixels. False outside the picture
// or before the stream starts.
- (BOOL)streamPoint:(NSPoint)p x:(int32_t*)x y:(int32_t*)y {
  double s, ox, oy;
  if (![self fitScale:&s ox:&ox oy:&oy]) return NO;
  const double sx = (p.x - ox) / s, sy = _streamH - (p.y - oy) / s;  // view origin is bottom left
  if (sx < 0 || sy < 0 || sx >= _streamW || sy >= _streamH) return NO;
  *x = static_cast<int32_t>(sx);
  *y = static_cast<int32_t>(sy);
  return YES;
}

- (void)sendEvent:(const rdaccel::InputEvent&)ev {
  if (!_viewer || !_connected) return;
  switch (ev.kind) {
    case rdaccel::InputKind::kMouseDown: _heldButtons.insert(ev.code); break;
    case rdaccel::InputKind::kMouseUp: _heldButtons.erase(ev.code); break;
    case rdaccel::InputKind::kKeyDown: _heldKeys.insert(ev.code); break;
    case rdaccel::InputKind::kKeyUp: _heldKeys.erase(ev.code); break;
    default: break;
  }
  if (ev.kind != rdaccel::InputKind::kKeyDown && ev.kind != rdaccel::InputKind::kKeyUp) {
    _lastX = ev.x;
    _lastY = ev.y;
  }
  std::string err;
  if (!_viewer->SendInput(ev, &err)) {
    std::fprintf(stderr, "input: %s\n", err.c_str());
    return;
  }
  const uint64_t now = rdaccel::NowMicros();
  std::lock_guard<std::mutex> lock(_latencyMu);
  if (ev.kind == rdaccel::InputKind::kMouseMove) {
    if (!_moveSentUs) _moveSentUs = now;  // the oldest move not answered yet
    _moveX = ev.x;
    _moveY = ev.y;
  } else if (ev.kind != rdaccel::InputKind::kKeyUp && ev.kind != rdaccel::InputKind::kMouseUp && !_inputSentUs &&
             now - _lastChangeUs.load() > 150'000) {
    _inputSentUs = now;  // quiet screen: the next change is this input's
  }
}

// Transport thread: the remote cursor reached (x, y). Answers the pending
// pointer moves once it is where the last one asked.
- (void)cursorArrivedAt:(int32_t)x y:(int32_t)y {
  std::lock_guard<std::mutex> lock(_latencyMu);
  if (_moveSentUs && std::abs(x - _moveX) <= 2 && std::abs(y - _moveY) <= 2) {
    _pointerMs.push_back((rdaccel::NowMicros() - _moveSentUs) / 1000.0);
    _moveSentUs = 0;
  }
}

// Main thread, from the key monitor.
- (void)key:(NSEvent*)e {
  rdaccel::InputEvent ev;
  ev.modifiers = Modifiers(e.modifierFlags);
  ev.code = rdaccel::KeyCodeFromMac(e.keyCode);
  if (ev.code == 0) return;  // no HID equivalent (e.g. Fn)
  if (e.type == NSEventTypeFlagsChanged) {
    const NSEventModifierFlags flag = FlagOfModifierKey(e.keyCode);
    if (flag == 0) return;
    if (flag == NSEventModifierFlagCapsLock) {
      // One flagsChanged per press: the host toggles on a full press.
      ev.kind = rdaccel::InputKind::kKeyDown;
      [self sendEvent:ev];
      ev.kind = rdaccel::InputKind::kKeyUp;
      [self sendEvent:ev];
      return;
    }
    // Held already: this is its release (the flag may stay set while the
    // key on the other side is still down). Not held: a press if the flag is set.
    if (_heldKeys.count(ev.code)) {
      ev.kind = rdaccel::InputKind::kKeyUp;
    } else if (e.modifierFlags & flag) {
      ev.kind = rdaccel::InputKind::kKeyDown;
    } else {
      return;
    }
  } else {
    ev.kind = e.type == NSEventTypeKeyDown ? rdaccel::InputKind::kKeyDown : rdaccel::InputKind::kKeyUp;
  }
  [self sendEvent:ev];  // auto-repeat arrives as further key-downs
}

// Lets go of every key and button still held on the remote side, e.g. when
// this window loses focus with a key down.
- (void)releaseHeld {
  const std::set<uint32_t> keys = _heldKeys, buttons = _heldButtons;
  for (uint32_t k : keys) {
    rdaccel::InputEvent ev;
    ev.kind = rdaccel::InputKind::kKeyUp;
    ev.code = k;
    [self sendEvent:ev];
  }
  for (uint32_t b : buttons) {
    rdaccel::InputEvent ev;
    ev.kind = rdaccel::InputKind::kMouseUp;
    ev.code = b;
    ev.x = _lastX;
    ev.y = _lastY;
    [self sendEvent:ev];
  }
}

- (void)windowDidResignKey:(NSNotification*)note {
  [self releaseHeld];
}

- (void)windowDidResize:(NSNotification*)note {
  [self rebuildCursor];
  [self placeCursor];
}

// Remembers `r` as owed by both surfaces; many small areas become one.
static void Owe(std::vector<rdaccel::Rect>& owed, const rdaccel::Rect& r, int32_t w, int32_t h) {
  if (owed.size() == 1 && owed[0].x == 0 && owed[0].y == 0 && owed[0].w == w && owed[0].h == h) return;
  if (owed.size() >= 64 || (r.x <= 0 && r.y <= 0 && r.w >= w && r.h >= h)) {
    owed.assign(1, rdaccel::Rect{0, 0, w, h});
    return;
  }
  owed.push_back(r);
}

// Runs on the viewer's decode thread. Copies what changed into the surface
// that is not on screen and hands it to the main thread; while the main
// thread is still busy with the previous one, only remembers what changed.
- (void)presentFrame:(const rdaccel::ImageView&)img changed:(std::span<const rdaccel::Rect>)changed {
  // Every picture the host sends has changed (an empty list means all of it).
  const uint64_t now = rdaccel::NowMicros();
  _lastChangeUs = now;
  {
    std::lock_guard<std::mutex> lock(_latencyMu);
    if (_inputSentUs) {
      _screenMs.push_back((now - _inputSentUs) / 1000.0);
      _inputSentUs = 0;
    }
  }
  std::unique_lock<std::mutex> lock(_surfMu);
  if (img.width != _surfW || img.height != _surfH || !_surf[0]) {
    for (auto& s : _surf) {
      if (s) CFRelease(s);
      s = MakeSurface(img.width, img.height);
    }
    _surfW = img.width;
    _surfH = img.height;
    _owed[0].assign(1, rdaccel::Rect{0, 0, img.width, img.height});
    _owed[1] = _owed[0];
    _front = -1;
    if (!_surf[0] || !_surf[1]) return;
  } else {
    for (auto& o : _owed) {
      if (changed.empty()) Owe(o, rdaccel::Rect{0, 0, img.width, img.height}, img.width, img.height);
      for (const auto& r : changed) Owe(o, r, img.width, img.height);
    }
  }
  if (_presentPending.exchange(true)) return;
  const int back = _front == 0 ? 1 : 0;
  IOSurfaceRef s = _surf[back];
  IOSurfaceLock(s, 0, nullptr);
  auto* dst = static_cast<uint8_t*>(IOSurfaceGetBaseAddress(s));
  const size_t dst_stride = IOSurfaceGetBytesPerRow(s);
  for (const auto& r0 : _owed[back]) {
    const int32_t x0 = std::clamp(r0.x, 0, img.width), y0 = std::clamp(r0.y, 0, img.height);
    const int32_t x1 = std::clamp(r0.x + r0.w, 0, img.width), y1 = std::clamp(r0.y + r0.h, 0, img.height);
    if (x1 <= x0) continue;
    for (int32_t y = y0; y < y1; ++y) {
      std::memcpy(dst + static_cast<size_t>(y) * dst_stride + static_cast<size_t>(x0) * 4,
                  img.data + static_cast<size_t>(y) * img.stride + static_cast<size_t>(x0) * 4,
                  static_cast<size_t>(x1 - x0) * 4);
    }
  }
  _owed[back].clear();
  IOSurfaceUnlock(s, 0, nullptr);
  CFRetain(s);
  lock.unlock();
  dispatch_async(dispatch_get_main_queue(), ^{
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    self->_layer.contents = (__bridge id)s;
    [CATransaction commit];
    CFRelease(s);
    {
      std::lock_guard<std::mutex> l(self->_surfMu);
      self->_front = back;
    }
    ++self->_presented;
    self->_presentPending = false;
  });
}

+ (CGImageRef)makeCursorImage:(const rdaccel::CursorImage&)c {
  CFDataRef data = CFDataCreate(kCFAllocatorDefault, c.bgra.data(), static_cast<CFIndex>(c.bgra.size()));
  CGDataProviderRef provider = CGDataProviderCreateWithCFData(data);
  CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
  CGImageRef image = CGImageCreate(c.width, c.height, 8, 32, static_cast<size_t>(c.width) * 4, cs,
                                   static_cast<CGBitmapInfo>(kCGBitmapByteOrder32Little) |
                                       static_cast<CGBitmapInfo>(kCGImageAlphaPremultipliedFirst),
                                   provider, nullptr, false, kCGRenderingIntentDefault);
  CGColorSpaceRelease(cs);
  CGDataProviderRelease(provider);
  CFRelease(data);
  return image;
}

- (NSCursor*)remoteCursor {
  return _nsCursor;
}

// Main thread. The remote cursor image as a system pointer, at the size it
// has in the picture as shown.
- (void)rebuildCursor {
  double s, ox, oy;
  if (!_cursorImage || _cursorW <= 0 || ![self fitScale:&s ox:&ox oy:&oy]) return;
  NSImage* image = [[NSImage alloc] initWithCGImage:_cursorImage
                                               size:NSMakeSize(std::max(1.0, _cursorW * s),
                                                               std::max(1.0, _cursorH * s))];
  _nsCursor = [[NSCursor alloc] initWithImage:image hotSpot:NSMakePoint(_hotX * s, _hotY * s)];
  [_window invalidateCursorRectsForView:_view];
}

// Main thread. `image` (owned, may be null) replaces the old one; a size of
// -1 means the image did not come with this update.
- (void)cursorChanged:(const rdaccel::CursorState&)c
                image:(CGImageRef)image
                width:(int32_t)iw
               height:(int32_t)ih
                 hotX:(int32_t)hx
                 hotY:(int32_t)hy {
  _cursor = c;
  if (iw >= 0) {
    _cursorW = iw;
    _cursorH = ih;
    _hotX = hx;
    _hotY = hy;
  }
  if (image) {
    if (_cursorImage) CGImageRelease(_cursorImage);
    _cursorImage = image;
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    _cursorLayer.contents = (__bridge id)image;
    [CATransaction commit];
    [self rebuildCursor];
  }
  [self placeCursor];
}

// Main thread. With the pointer over the picture the system draws it (see
// resetCursorRects); otherwise the remote cursor is drawn as a layer where
// the remote machine has it.
- (void)placeCursor {
  [CATransaction begin];
  [CATransaction setDisableActions:YES];
  double s, ox, oy;
  if (!_view.pointerInside && _cursorW > 0 && [self fitScale:&s ox:&ox oy:&oy]) {
    _cursorLayer.frame = CGRectMake(ox + (_cursor.x - _hotX) * s,
                                    oy + (_streamH - (_cursor.y - _hotY) - _cursorH) * s, _cursorW * s,
                                    _cursorH * s);
    _cursorLayer.hidden = !_cursor.visible;
  } else {
    _cursorLayer.hidden = YES;
  }
  [CATransaction commit];
}

- (void)tick {
  if (!_viewer) return;
  const auto vs = _viewer->stats();
  const uint64_t shown = _presented.exchange(0);
  const double kbps = (vs.bytes_received - _last.bytes_received) * 8.0 / 1000.0;
  const uint64_t dec = vs.frames_decoded - _last.frames_decoded;
  const double dec_ms = dec ? (vs.decode_us_total - _last.decode_us_total) / 1000.0 / dec : 0;
  const uint64_t errors = vs.decode_errors - _last.decode_errors;
  _last = vs;
  NSString *pointer, *screen;
  {
    std::lock_guard<std::mutex> lock(_latencyMu);
    pointer = Summary(_pointerMs);
    screen = Summary(_screenMs);
  }
  NSString* line = [NSString
      stringWithFormat:@"%llu fps shown (%llu decoded) | %.0f kbps | dec %.1f ms | pointer %@ | input→screen %@%@",
                       static_cast<unsigned long long>(shown), static_cast<unsigned long long>(dec), kbps, dec_ms,
                       pointer, screen,
                       errors ? [NSString stringWithFormat:@" | %llu decode errors",
                                                           static_cast<unsigned long long>(errors)]
                              : @""];
  _window.title = line;
  std::printf("%s\n", line.UTF8String);
  std::fflush(stdout);
}

- (void)fail:(NSString*)message {
  static bool failing = false;
  if (failing) return;
  failing = true;
  std::fprintf(stderr, "%s\n", message.UTF8String);
  [NSApp terminate:nil];
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)app {
  return YES;
}

- (void)applicationWillTerminate:(NSNotification*)note {
  [_timer invalidate];
  if (_keyMonitor) [NSEvent removeMonitor:_keyMonitor];
  [self releaseHeld];
  _viewer.reset();  // says goodbye to the host; no callbacks after this
  if (_cursorImage) CGImageRelease(_cursorImage);
  _cursorImage = nullptr;
  for (auto& s : _surf) {
    if (s) CFRelease(s);
    s = nullptr;
  }
}

@end

int main(int argc, char** argv) {
  @autoreleasepool {
    rdaccel::examples::Args args(argc, argv);
    g_args = &args;
    NSApplication* app = NSApplication.sharedApplication;
    app.activationPolicy = NSApplicationActivationPolicyRegular;
    AppDelegate* delegate = [[AppDelegate alloc] init];
    app.delegate = delegate;
    [app run];
  }
  return 0;
}
