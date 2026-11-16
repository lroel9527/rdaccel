#include "mac/capture_cgstream.h"

#include "capture/dirty_rects.h"
#include "mac/displays_mac.h"

#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#import <IOSurface/IOSurface.h>

#include <atomic>
#include <cmath>

// Deprecated in macOS 14; used only below it (platform_mac.mm).
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

namespace rdacore::platform {

struct CgStreamCapturer::Impl {
  CGDisplayStreamRef stream = nullptr;
  dispatch_queue_t queue = nullptr;
  dispatch_semaphore_t stopped = nullptr;  // signalled by the stream's "stopped" frame
  std::atomic<bool> stopping{false};
  std::atomic<bool> reported{false};
  capture::FrameFn on_frame;
  capture::StoppedFn on_stopped;
  double scale = 1;
  int32_t w = 0, h = 0;
};

CgStreamCapturer::CgStreamCapturer() : impl_(std::make_unique<Impl>()) {}

CgStreamCapturer::~CgStreamCapturer() { Stop(); }

bool CgStreamCapturer::Start(const capture::CaptureConfig& cfg, capture::FrameFn on_frame) {
  if (impl_->stream) {
    error_ = "already running";
    return false;
  }
  CGDirectDisplayID id = 0;
  if (!MacDisplayId(cfg.display_index, id, &error_)) return false;
  Impl* impl = impl_.get();
  impl->scale = MacDisplayScale(id);
  const CGRect b = CGDisplayBounds(id);
  impl->w = cfg.width > 0 ? cfg.width : static_cast<int32_t>(std::lround(b.size.width * impl->scale));
  impl->h = cfg.height > 0 ? cfg.height : static_cast<int32_t>(std::lround(b.size.height * impl->scale));
  impl->on_frame = std::move(on_frame);
  impl->on_stopped = on_stopped_;
  impl->stopping = false;
  impl->reported = false;
  impl->queue = dispatch_queue_create("rdaccel.capture.cgstream", DISPATCH_QUEUE_SERIAL);
  impl->stopped = dispatch_semaphore_create(0);
  NSDictionary* props = @{
    (__bridge NSString*)kCGDisplayStreamShowCursor : cfg.show_cursor ? @YES : @NO,
    (__bridge NSString*)kCGDisplayStreamMinimumFrameTime : @(1.0 / (cfg.fps > 0 ? cfg.fps : 60)),
    (__bridge NSString*)kCGDisplayStreamQueueDepth : @5,
  };
  impl->stream = CGDisplayStreamCreateWithDispatchQueue(
      id, static_cast<size_t>(impl->w), static_cast<size_t>(impl->h), 'BGRA', (__bridge CFDictionaryRef)props,
      impl->queue, ^(CGDisplayStreamFrameStatus status, uint64_t, IOSurfaceRef surface, CGDisplayStreamUpdateRef update) {
        if (status == kCGDisplayStreamFrameStatusStopped) {
          dispatch_semaphore_signal(impl->stopped);
          if (!impl->stopping && !impl->reported.exchange(true) && impl->on_stopped) {
            impl->on_stopped("screen capture stopped (CGDisplayStream)");
          }
          return;
        }
        if (status != kCGDisplayStreamFrameStatusFrameComplete || !surface || impl->stopping) return;
        IOSurfaceLock(surface, kIOSurfaceLockReadOnly, nullptr);
        capture::CapturedFrame f;
        f.bgra = {static_cast<const uint8_t*>(IOSurfaceGetBaseAddress(surface)),
                  static_cast<int32_t>(IOSurfaceGetWidth(surface)), static_cast<int32_t>(IOSurfaceGetHeight(surface)),
                  static_cast<int32_t>(IOSurfaceGetBytesPerRow(surface))};
        f.capture_time_us = capture::MonotonicMicros();
        // What changed. The unit is not documented (pixels of the output
        // or points of the display): both readings, as for ScreenCaptureKit.
        // No rects means the whole frame may have changed.
        if (update) {
          size_t n = 0;
          const CGRect* rects = CGDisplayStreamUpdateGetRects(update, kCGDisplayStreamUpdateDirtyRects, &n);
          for (size_t i = 0; rects && i < n; ++i) {
            const CGRect& r = rects[i];
            capture::AppendDirtyRect(r.origin.x, r.origin.y, r.size.width, r.size.height, impl->scale, 0, 0,
                                     f.bgra.width, f.bgra.height, f.dirty);
          }
          if (n > 0 && f.dirty.empty()) f.dirty.push_back({0, 0, f.bgra.width, f.bgra.height});
        }
        impl->on_frame(f);
        IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, nullptr);
      });
  if (!impl->stream) {
    error_ = "CGDisplayStreamCreate failed (grant Screen Recording permission to this app/terminal)";
    return false;
  }
  const CGError e = CGDisplayStreamStart(impl->stream);
  if (e != kCGErrorSuccess) {
    error_ = "CGDisplayStreamStart failed (CGError " + std::to_string(e) + ")";
    CFRelease(impl->stream);
    impl->stream = nullptr;
    return false;
  }
  return true;
}

void CgStreamCapturer::Stop() {
  if (!impl_ || !impl_->stream) return;
  impl_->stopping = true;  // stopping on purpose is not news
  CGDisplayStreamStop(impl_->stream);
  // The stream says it has stopped with one last call; then nothing more
  // is queued and it can go.
  dispatch_semaphore_wait(impl_->stopped, dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC));
  dispatch_sync(impl_->queue, ^{
                });
  CFRelease(impl_->stream);
  impl_->stream = nullptr;
}

}  // namespace rdacore::platform
