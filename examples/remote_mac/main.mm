// rdaccel_remote: a windowed viewer for macOS that controls another machine.
// Connects to rdaccel_serve (run it with --inject on the other machine),
// shows the remote screen in a window, and sends this window's mouse,
// scroll wheel and keyboard back. Uses only the public API
// (rdaccel/rdaccel.hpp).
//
//   rdaccel_remote HOST [--udp=PORT | --tcp=PORT] (--key=<64 hex digits> | --plaintext)
//
// Keys are sent as physical keys (USB HID codes), so the remote machine's
// own keyboard layout and input method decide what is typed; Command
// shortcuts go to the remote machine too. Close the window to quit.
//
// Once per second the title bar and stdout show: decoded fps, received
// kbps, decode time, and "input→update", the time from sending an input
// event to the next picture that changed (the two machines' clocks are
// never compared, so this is measured on this machine alone; it includes
// the network round trip, capture, encoding and decoding).
//
// The library plays and captures no sound, and neither does this program.

#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>

#include <rdaccel/rdaccel.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
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

}  // namespace

@class AppDelegate;

// The picture, the remote cursor on top, and all local input.
@interface RemoteView : NSView
@property(nonatomic, weak) AppDelegate* app;
- (void)showLocalCursor;
@end

@interface AppDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate>
- (void)sendEvent:(const rdaccel::InputEvent&)ev;
- (BOOL)streamPoint:(NSPoint)p x:(int32_t*)x y:(int32_t*)y;
- (void)releaseHeld;
@end

@implementation RemoteView {
  NSTrackingArea* _tracking;
  BOOL _cursorHidden;
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

// The remote cursor is drawn as a layer; hide the local one over the view.
- (void)mouseEntered:(NSEvent*)e {
  if (!_cursorHidden) [NSCursor hide];
  _cursorHidden = YES;
}
- (void)mouseExited:(NSEvent*)e {
  [self showLocalCursor];
}
- (void)showLocalCursor {
  if (_cursorHidden) [NSCursor unhide];
  _cursorHidden = NO;
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
  CGImageRef _cursorImage;       // owned
  rdaccel::CursorState _cursor;  // main thread
  int32_t _cursorW, _cursorH, _hotX, _hotY;
  int32_t _streamW, _streamH;  // main thread
  NSTimer* _timer;
  id _keyMonitor;

  std::unique_ptr<rdaccel::Viewer> _viewer;
  std::set<uint32_t> _heldKeys;     // HID codes, main thread
  std::set<uint32_t> _heldButtons;  // main thread
  int32_t _lastX, _lastY;

  std::atomic<bool> _presentPending;
  std::atomic<uint64_t> _presented;
  // Input→update: when the oldest input not yet followed by a changed
  // picture was sent (0 = none), and the samples of the current second.
  std::atomic<uint64_t> _inputPendingSince;
  std::mutex _latencyMu;
  std::vector<double> _latencyMs;
  rdaccel::ViewerStats _last;
  bool _connected;
}

- (void)applicationDidFinishLaunching:(NSNotification*)note {
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
    [weakSelf presentFrame:img changed:!changed.empty()];
  };
  handlers.on_stream_info = [weakSelf](int32_t w, int32_t h) {
    dispatch_async(dispatch_get_main_queue(), ^{
      [weakSelf streamSizeChanged:w height:h];
    });
  };
  handlers.on_cursor = [weakSelf](const rdaccel::CursorState& c) {
    // Runs on the transport thread; the image pointer is only valid now.
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
  const bool first = _streamW == 0;
  if (w == _streamW && h == _streamH) return;
  _streamW = w;
  _streamH = h;
  std::printf("stream %dx%d\n", w, h);
  std::fflush(stdout);
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

// Converts a point in the view to stream pixels (the picture is
// aspect-fitted). False outside the picture or before the stream starts.
- (BOOL)streamPoint:(NSPoint)p x:(int32_t*)x y:(int32_t*)y {
  const NSSize b = _view.bounds.size;
  if (_streamW <= 0 || _streamH <= 0 || b.width <= 0 || b.height <= 0) return NO;
  const double s = std::min(b.width / _streamW, b.height / _streamH);
  const double ox = (b.width - _streamW * s) / 2, oy = (b.height - _streamH * s) / 2;
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
  uint64_t none = 0;
  _inputPendingSince.compare_exchange_strong(none, rdaccel::NowMicros());
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
  [_view showLocalCursor];
}

- (void)windowDidResize:(NSNotification*)note {
  [self placeCursor];
}

// Runs on the viewer's decode thread. Copies the picture (the view is only
// valid during the callback) and hands it to the main thread, dropping
// frames while the main thread is still busy with the previous one.
- (void)presentFrame:(const rdaccel::ImageView&)img changed:(bool)changed {
  if (changed) {
    const uint64_t since = _inputPendingSince.exchange(0);
    if (since) {
      std::lock_guard<std::mutex> lock(_latencyMu);
      _latencyMs.push_back((rdaccel::NowMicros() - since) / 1000.0);
    }
  }
  if (_presentPending.exchange(true)) return;
  const size_t bytes = static_cast<size_t>(img.stride) * img.height;
  CFDataRef data = CFDataCreate(kCFAllocatorDefault, img.data, static_cast<CFIndex>(bytes));
  CGDataProviderRef provider = CGDataProviderCreateWithCFData(data);
  CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
  CGImageRef image = CGImageCreate(img.width, img.height, 8, 32, img.stride, cs,
                                   static_cast<CGBitmapInfo>(kCGBitmapByteOrder32Little) |
                                       static_cast<CGBitmapInfo>(kCGImageAlphaNoneSkipFirst),
                                   provider, nullptr, false, kCGRenderingIntentDefault);
  CGColorSpaceRelease(cs);
  CGDataProviderRelease(provider);
  CFRelease(data);
  dispatch_async(dispatch_get_main_queue(), ^{
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    self->_layer.contents = (__bridge id)image;
    [CATransaction commit];
    CGImageRelease(image);
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
  }
  [self placeCursor];
}

// Main thread. Places the cursor layer where the pointer is in the
// aspect-fitted picture.
- (void)placeCursor {
  [CATransaction begin];
  [CATransaction setDisableActions:YES];
  const CGSize b = _layer.bounds.size;
  if (_streamW > 0 && _streamH > 0 && _cursorW > 0 && b.width > 0 && b.height > 0) {
    const double s = std::min(b.width / _streamW, b.height / _streamH);
    const double ox = (b.width - _streamW * s) / 2, oy = (b.height - _streamH * s) / 2;
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
  const uint64_t dec = vs.frames_decoded - _last.frames_decoded;
  const double kbps = (vs.bytes_received - _last.bytes_received) * 8.0 / 1000.0;
  const double dec_ms = dec ? (vs.decode_us_total - _last.decode_us_total) / 1000.0 / dec : 0;
  const uint64_t errors = vs.decode_errors - _last.decode_errors;
  _last = vs;
  std::vector<double> lat;
  {
    std::lock_guard<std::mutex> lock(_latencyMu);
    lat.swap(_latencyMs);
  }
  NSString* latency = @"-";
  if (!lat.empty()) {
    std::sort(lat.begin(), lat.end());
    double sum = 0;
    for (double v : lat) sum += v;
    latency = [NSString stringWithFormat:@"%.0f ms (max %.0f, n=%zu)", sum / lat.size(), lat.back(), lat.size()];
  }
  NSString* line =
      [NSString stringWithFormat:@"%llu fps | %.0f kbps | dec %.1f ms | input→update %@%@",
                                 static_cast<unsigned long long>(dec), kbps, dec_ms, latency,
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
  [_view showLocalCursor];
  [NSApp terminate:nil];
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)app {
  return YES;
}

- (void)applicationWillTerminate:(NSNotification*)note {
  [_timer invalidate];
  if (_keyMonitor) [NSEvent removeMonitor:_keyMonitor];
  [self releaseHeld];
  _viewer.reset();  // says goodbye to the host
  if (_cursorImage) CGImageRelease(_cursorImage);
  _cursorImage = nullptr;
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
