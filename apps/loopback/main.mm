// rdaccel_loopback: host and viewer in one macOS process.
//
//   ScreenCaptureKit -> tile diff -> VideoToolbox HEVC encode -> local pipe
//   -> VideoToolbox HEVC decode -> window
//
// The window shows the decoded stream of the main display (this app's own
// window is excluded from capture). The title bar and stdout show, once per
// second: encoded fps, bitrate, encode/decode time and capture-to-display
// latency (both ends share one clock, so the latency is exact).
//
//   open rdaccel_loopback.app --args --fps=60 --bitrate=8000000

#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>

#include <atomic>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>

#include "capture/capturer.h"
#include "platform.h"
#include "session/host_session.h"
#include "session/viewer_session.h"
#include "transport/local_pipe.h"

using namespace rdaccel;

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
  NSTimer* _timer;

  std::unique_ptr<transport::LocalPipeEndpoint> _hostEnd, _viewerEnd;
  std::unique_ptr<capture::ICapturer> _capturer;
  std::unique_ptr<session::HostSession> _host;
  std::unique_ptr<session::ViewerSession> _viewer;
  std::mutex _hostMu;
  std::string _encoderName;

  std::atomic<bool> _presentPending;
  std::atomic<bool> _failed;
  std::atomic<uint64_t> _latencySumUs, _latencyCount, _presented;
  session::HostStats _lastHost;
  session::ViewerStats _lastViewer;
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
  [_window makeKeyAndOrderFront:nil];
  [NSApp activateIgnoringOtherApps:YES];

  auto [h, v] = transport::MakeLocalPipe();
  _hostEnd = std::move(h);
  _viewerEnd = std::move(v);

  std::string err;
  auto decoder = platform::CreateHevcDecoder(&err);
  _viewer = std::make_unique<session::ViewerSession>(_viewerEnd.get(), std::move(decoder));
  __weak AppDelegate* weakSelf = self;
  _viewer->SetFrameHandler([weakSelf](const tile::ImageView& img, const std::vector<Rect>&,
                                      uint64_t, uint64_t capture_time_us) {
    [weakSelf presentFrame:img captureTime:capture_time_us];
  });

  const int fps = static_cast<int>(ArgNum("fps", 60));
  const long bitrate = ArgNum("bitrate", 8'000'000);
  _capturer = platform::CreateScreenCapturer(&err);
  capture::CaptureConfig cc;
  cc.fps = fps;
  const bool ok = _capturer->Start(cc, [weakSelf, fps, bitrate](const capture::CapturedFrame& f) {
    [weakSelf onCaptured:f fps:fps bitrate:bitrate];
  });
  if (!ok) {
    [self fail:[NSString stringWithUTF8String:_capturer->last_error().c_str()]];
    return;
  }
  _timer = [NSTimer scheduledTimerWithTimeInterval:1.0
                                            target:self
                                          selector:@selector(tick)
                                          userInfo:nil
                                           repeats:YES];
}

- (void)onCaptured:(const capture::CapturedFrame&)f fps:(int)fps bitrate:(long)bitrate {
  if (_failed) return;
  std::lock_guard<std::mutex> lock(_hostMu);
  if (!_host) {
    codec::EncoderConfig cfg{f.bgra.width, f.bgra.height, fps, bitrate};
    std::string err;
    auto enc = platform::CreateHevcEncoder(cfg, "", &err);
    if (!enc) {
      NSString* msg = [NSString stringWithUTF8String:err.c_str()];
      // Don't stop the capturer from inside its own callback queue (that
      // would deadlock); just ignore further frames and quit.
      _failed = true;
      dispatch_async(dispatch_get_main_queue(), ^{
        [self fail:msg];
      });
      return;
    }
    _encoderName = enc->name();
    std::printf("capture %dx%d, encoder: %s\n", f.bgra.width, f.bgra.height, _encoderName.c_str());
    _host = std::make_unique<session::HostSession>(_hostEnd.get(), std::move(enc), f.bgra.width,
                                                   f.bgra.height);
  }
  _host->OnFrame(f);
}

// Runs on the viewer's transport thread. Copies the picture (the view is only
// valid during the callback) and hands it to the main thread, dropping frames
// if the main thread is still busy with the previous one.
- (void)presentFrame:(const tile::ImageView&)img captureTime:(uint64_t)captureTime {
  if (_presentPending.exchange(true)) return;
  const size_t bytes = static_cast<size_t>(img.stride) * img.height;
  CFDataRef data = CFDataCreate(kCFAllocatorDefault, img.data, static_cast<CFIndex>(bytes));
  CGDataProviderRef provider = CGDataProviderCreateWithCFData(data);
  CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
  CGImageRef image = CGImageCreate(img.width, img.height, 8, 32, img.stride, cs,
                                   static_cast<CGBitmapInfo>(kCGBitmapByteOrder32Little | kCGImageAlphaNoneSkipFirst), provider,
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
    const uint64_t now = capture::MonotonicMicros();
    self->_latencySumUs += now - captureTime;
    ++self->_latencyCount;
    ++self->_presented;
    self->_presentPending = false;
  });
}

- (void)tick {
  session::HostStats hs;
  {
    std::lock_guard<std::mutex> lock(_hostMu);
    if (!_host) return;
    hs = _host->stats();
  }
  const auto vs = _viewer->stats();
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
  if (_capturer) _capturer->Stop();
}

@end

int main(int argc, const char* argv[]) {
  @autoreleasepool {
    NSApplication* app = NSApplication.sharedApplication;
    app.activationPolicy = NSApplicationActivationPolicyRegular;
    AppDelegate* delegate = [[AppDelegate alloc] init];
    app.delegate = delegate;
    [app run];
  }
  return 0;
}
