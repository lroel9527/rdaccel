// rdaccel_loopback: host and viewer in one macOS process. An example of
// embedding the library; it uses only the public API (rdaccel/rdaccel.hpp).
//
//   ScreenCaptureKit -> tile diff -> VideoToolbox HEVC encode -> local pipe
//   -> VideoToolbox HEVC decode -> window
//
// The window shows the decoded stream of the main display (this app's own
// window is excluded from capture). The title bar and stdout show, once per
// second: encoded fps, bitrate, encode/decode time and capture-to-display
// latency (both ends share one clock, so the latency is exact).
//
// The mouse cursor arrives on the cursor channel and is shown as its own
// layer on top of the picture, so it moves even when no frame changes.
//
//   open rdaccel_loopback.app --args --fps=60 --bitrate=8000000

#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>

#include <rdaccel/rdaccel.hpp>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <memory>
#include <string>

namespace {

long ArgNum(const char* key, long def) {
  NSString* prefix = [NSString stringWithFormat:@"--%s=", key];
  for (NSString* a in NSProcessInfo.processInfo.arguments) {
    if ([a hasPrefix:prefix]) return [[a substringFromIndex:prefix.length] integerValue];
  }
  return def;
}

}  // namespace

@interface AppDelegate : NSObject <NSApplicationDelegate>
@end

@implementation AppDelegate {
  NSWindow* _window;
  CALayer* _layer;
  CALayer* _cursorLayer;
  CGImageRef _cursorImage;  // owned
  rdaccel::CursorState _cursor;  // main thread
  int32_t _streamW, _streamH;    // main thread
  NSTimer* _timer;

  std::unique_ptr<rdaccel::Host> _host;
  std::unique_ptr<rdaccel::Viewer> _viewer;

  std::atomic<bool> _presentPending;
  std::atomic<uint64_t> _latencySumUs, _latencyCount, _presented;
  rdaccel::HostStats _lastHost;
  rdaccel::ViewerStats _lastViewer;
  bool _printedEncoder;
}

- (void)applicationDidFinishLaunching:(NSNotification*)note {
  _window = [[NSWindow alloc] initWithContentRect:NSMakeRect(80, 80, 1280, 800)
                                        styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                  NSWindowStyleMaskResizable |
                                                  NSWindowStyleMaskMiniaturizable
                                          backing:NSBackingStoreBuffered
                                            defer:NO];
  _window.title = @"rdaccel loopback";
  NSView* view = _window.contentView;
  view.wantsLayer = YES;
  _layer = view.layer;
  _layer.backgroundColor = NSColor.blackColor.CGColor;
  _layer.contentsGravity = kCAGravityResizeAspect;
  _cursorLayer = [CALayer layer];
  _cursorLayer.anchorPoint = CGPointZero;
  _cursorLayer.hidden = YES;
  [_layer addSublayer:_cursorLayer];
  [_window makeKeyAndOrderFront:nil];
  [NSApp activateIgnoringOtherApps:YES];

  auto [hostEnd, viewerEnd] = rdaccel::CreateLocalPipe();

  std::string err;
  __weak AppDelegate* weakSelf = self;
  rdaccel::ViewerHandlers handlers;
  handlers.on_frame = [weakSelf](const rdaccel::ImageView& img, std::span<const rdaccel::Rect>,
                                 const rdaccel::FrameInfo& info) {
    [weakSelf presentFrame:img captureTime:info.capture_time_us];
  };
  handlers.on_stream_info = [weakSelf](int32_t w, int32_t h) {
    dispatch_async(dispatch_get_main_queue(), ^{
      AppDelegate* me = weakSelf;
      if (!me) return;
      me->_streamW = w;
      me->_streamH = h;
    });
  };
  handlers.on_cursor = [weakSelf](const rdaccel::CursorState& c) {
    // Runs on the transport thread; the image pointer is only valid now.
    CGImageRef image = nullptr;
    if (c.image_changed && c.image) image = [AppDelegate makeCursorImage:*c.image];
    rdaccel::CursorState state = c;
    state.image = nullptr;  // not valid after this call; the size is passed instead
    const int32_t iw = c.image ? c.image->width : 0, ih = c.image ? c.image->height : 0;
    const int32_t hx = c.image ? c.image->hotspot_x : 0, hy = c.image ? c.image->hotspot_y : 0;
    dispatch_async(dispatch_get_main_queue(), ^{
      AppDelegate* me = weakSelf;
      if (!me) {
        if (image) CGImageRelease(image);
        return;
      }
      [me showCursor:state image:image width:iw height:ih hotX:hx hotY:hy];
    });
  };
  _viewer = rdaccel::Viewer::Create(std::move(viewerEnd), std::move(handlers), &err);
  if (!_viewer) {
    [self fail:[NSString stringWithUTF8String:err.c_str()]];
    return;
  }

  rdaccel::HostConfig cfg;
  cfg.fps = static_cast<int32_t>(ArgNum("fps", 60));
  cfg.bitrate_bps = ArgNum("bitrate", 8'000'000);
  _host = std::make_unique<rdaccel::Host>(std::move(hostEnd), cfg);
  _host->SetErrorHandler([weakSelf](const std::string& e) {
    NSString* msg = [NSString stringWithUTF8String:e.c_str()];
    // Called on the capture queue; never stop capture from there.
    dispatch_async(dispatch_get_main_queue(), ^{
      [weakSelf fail:msg];
    });
  });
  if (!_host->StartScreenCapture(&err)) {
    [self fail:[NSString stringWithUTF8String:err.c_str()]];
    return;
  }
  _timer = [NSTimer scheduledTimerWithTimeInterval:1.0
                                            target:self
                                          selector:@selector(tick)
                                          userInfo:nil
                                           repeats:YES];
}

// Runs on the viewer's transport thread. Copies the picture (the view is only
// valid during the callback) and hands it to the main thread, dropping frames
// if the main thread is still busy with the previous one.
- (void)presentFrame:(const rdaccel::ImageView&)img captureTime:(uint64_t)captureTime {
  if (_presentPending.exchange(true)) return;
  const size_t bytes = static_cast<size_t>(img.stride) * img.height;
  CFDataRef data = CFDataCreate(kCFAllocatorDefault, img.data, static_cast<CFIndex>(bytes));
  CGDataProviderRef provider = CGDataProviderCreateWithCFData(data);
  CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
  CGImageRef image = CGImageCreate(img.width, img.height, 8, 32, img.stride, cs,
                                   static_cast<CGBitmapInfo>(kCGBitmapByteOrder32Little) |
                                       static_cast<CGBitmapInfo>(kCGImageAlphaNoneSkipFirst), provider,
                                   nullptr, false, kCGRenderingIntentDefault);
  CGColorSpaceRelease(cs);
  CGDataProviderRelease(provider);
  CFRelease(data);
  dispatch_async(dispatch_get_main_queue(), ^{
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    self->_layer.contents = (__bridge id)image;
    [CATransaction commit];
    CGImageRelease(image);
    const uint64_t now = rdaccel::NowMicros();
    self->_latencySumUs += now - captureTime;
    ++self->_latencyCount;
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

// Main thread. Places the cursor layer where the pointer is in the
// aspect-fitted picture. `image` (owned, may be null) replaces the old one.
- (void)showCursor:(const rdaccel::CursorState&)c
             image:(CGImageRef)image
             width:(int32_t)iw
            height:(int32_t)ih
              hotX:(int32_t)hx
              hotY:(int32_t)hy {
  [CATransaction begin];
  [CATransaction setDisableActions:YES];
  if (image) {
    if (_cursorImage) CGImageRelease(_cursorImage);
    _cursorImage = image;
    _cursorLayer.contents = (__bridge id)image;
  }
  const CGSize b = _layer.bounds.size;
  if (_streamW > 0 && _streamH > 0 && iw > 0 && b.width > 0 && b.height > 0) {
    const double s = std::min(b.width / _streamW, b.height / _streamH);
    const double ox = (b.width - _streamW * s) / 2, oy = (b.height - _streamH * s) / 2;
    // Layer coordinates have their origin at the bottom left.
    _cursorLayer.frame = CGRectMake(ox + (c.x - hx) * s, oy + (_streamH - (c.y - hy) - ih) * s, iw * s, ih * s);
    _cursorLayer.hidden = !c.visible;
  } else {
    _cursorLayer.hidden = YES;
  }
  [CATransaction commit];
}

- (void)tick {
  if (!_host || !_viewer) return;
  const auto hs = _host->stats();
  const auto vs = _viewer->stats();
  if (!_printedEncoder && hs.frames_encoded > 0) {
    _printedEncoder = YES;
    std::printf("encoder: %s, decoder: %s\n", _host->encoder_name().c_str(),
                _viewer->decoder_name().c_str());
  }
  const uint64_t enc = hs.frames_encoded - _lastHost.frames_encoded;
  const double kbps = (hs.bytes_sent - _lastHost.bytes_sent) * 8.0 / 1000.0;
  const double enc_ms = enc ? (hs.encode_us_total - _lastHost.encode_us_total) / 1000.0 / enc : 0;
  const uint64_t dec = vs.frames_decoded - _lastViewer.frames_decoded;
  const double dec_ms = dec ? (vs.decode_us_total - _lastViewer.decode_us_total) / 1000.0 / dec : 0;
  const uint64_t n = _latencyCount.exchange(0);
  const double lat_ms = n ? _latencySumUs.exchange(0) / 1000.0 / n : 0;
  const uint64_t skipped = hs.frames_skipped - _lastHost.frames_skipped;
  _lastHost = hs;
  _lastViewer = vs;

  NSString* line = [NSString
      stringWithFormat:@"%llu fps encoded (%llu skipped) | %.0f kbps | enc %.1f ms | dec %.1f ms | "
                       @"capture-to-display %.1f ms",
                       enc, skipped, kbps, enc_ms, dec_ms, lat_ms];
  _window.title = line;
  std::printf("%s\n", line.UTF8String);
  std::fflush(stdout);
}

- (void)fail:(NSString*)message {
  std::fprintf(stderr, "error: %s\n", message.UTF8String);
  NSAlert* alert = [[NSAlert alloc] init];
  alert.messageText = @"rdaccel loopback failed";
  alert.informativeText = message;
  [alert runModal];
  [NSApp terminate:nil];
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)app {
  return YES;
}

- (void)applicationWillTerminate:(NSNotification*)note {
  [_timer invalidate];
  // Host first: it stops capture, and nothing feeds the viewer after that.
  _host.reset();
  _viewer.reset();
  if (_cursorImage) CGImageRelease(_cursorImage);
  _cursorImage = nullptr;
}

@end

int main() {
  @autoreleasepool {
    NSApplication* app = NSApplication.sharedApplication;
    app.activationPolicy = NSApplicationActivationPolicyRegular;
    AppDelegate* delegate = [[AppDelegate alloc] init];
    app.delegate = delegate;
    [app run];
  }
  return 0;
}
