#include "mac/capture_sck.h"

#include "capture/dirty_rects.h"
#include "mac/displays_mac.h"

#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>

#include <unistd.h>

#include <cmath>

using rdacore::capture::CapturedFrame;
using rdacore::capture::FrameFn;
using rdacore::capture::StoppedFn;

#include <atomic>

// Receives the pictures, and as the stream's delegate hears when the system
// stops it (Screen Recording permission revoked, display disconnected, ...).
@interface RDStreamOutput : NSObject <SCStreamOutput, SCStreamDelegate>
- (instancetype)initWithCallback:(FrameFn)fn stopped:(StoppedFn)stopped;
- (void)ignoreStops;
@end

@implementation RDStreamOutput {
  FrameFn _fn;
  StoppedFn _stopped;
  std::atomic<bool> _ignore_stops;
}

- (instancetype)initWithCallback:(FrameFn)fn stopped:(StoppedFn)stopped {
  if ((self = [super init])) {
    _fn = std::move(fn);
    _stopped = std::move(stopped);
    _ignore_stops = false;
  }
  return self;
}

- (void)ignoreStops {
  _ignore_stops = true;
}

- (void)stream:(SCStream*)stream didStopWithError:(NSError*)error {
  if (_ignore_stops.exchange(true) || !_stopped) return;
  std::string why = "screen capture stopped";
  if (error) {
    why += std::string(": ") + error.localizedDescription.UTF8String;
    // SCStreamErrorUserDeclined (-3801): the permission was taken away.
    if (error.code == -3801) why += " (Screen Recording permission)";
  }
  _stopped(why);
}

- (void)stream:(SCStream*)stream
    didOutputSampleBuffer:(CMSampleBufferRef)sample
                   ofType:(SCStreamOutputType)type {
  if (type != SCStreamOutputTypeScreen || !CMSampleBufferIsValid(sample)) return;

  CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(sample, false);
  if (!attachments || CFArrayGetCount(attachments) == 0) return;
  NSDictionary* info = (__bridge NSDictionary*)CFArrayGetValueAtIndex(attachments, 0);

  // SCK sends "idle" frames when nothing changed; they carry no image.
  NSNumber* status = info[SCStreamFrameInfoStatus];
  if (!status || status.integerValue != SCFrameStatusComplete) return;

  CVPixelBufferRef pb = CMSampleBufferGetImageBuffer(sample);
  if (!pb) return;
  CVPixelBufferLockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);

  CapturedFrame f;
  f.bgra = {static_cast<const uint8_t*>(CVPixelBufferGetBaseAddress(pb)),
            static_cast<int32_t>(CVPixelBufferGetWidth(pb)),
            static_cast<int32_t>(CVPixelBufferGetHeight(pb)),
            static_cast<int32_t>(CVPixelBufferGetBytesPerRow(pb))};
  f.capture_time_us = rdacore::capture::MonotonicMicros();

  // What changed, for the tile comparison. The content rect is in points;
  // the dirty rects have turned out to be in pixels of the picture, which
  // on a 2x display is not the same (taking them for points left the menu
  // bar and whatever was typed unrefreshed). AppendDirtyRect covers both
  // readings. If anything is missing or no rect has any area,
  // the list stays empty, which makes the tile differ check the whole frame
  // (correct, just slower).
  NSArray* dirty = info[SCStreamFrameInfoDirtyRects];
  NSDictionary* content_dict = info[SCStreamFrameInfoContentRect];
  CGRect content = CGRectZero;
  if (dirty && content_dict &&
      CGRectMakeWithDictionaryRepresentation((__bridge CFDictionaryRef)content_dict, &content) &&
      content.size.width > 0) {
    const double scale = f.bgra.width / content.size.width;
    bool understood = true;
    for (NSDictionary* d in dirty) {
      CGRect r;
      if (!CGRectMakeWithDictionaryRepresentation((__bridge CFDictionaryRef)d, &r)) {
        understood = false;
        break;
      }
      rdacore::capture::AppendDirtyRect(r.origin.x, r.origin.y, r.size.width, r.size.height, scale,
                                        content.origin.x, content.origin.y, f.bgra.width, f.bgra.height, f.dirty);
    }
    if (!understood) f.dirty.clear();
  }

  _fn(f);
  CVPixelBufferUnlockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
}

@end

namespace rdacore::platform {

struct SckCapturer::Impl {
  SCStream* stream = nil;
  RDStreamOutput* output = nil;
  dispatch_queue_t queue = nullptr;
};

SckCapturer::SckCapturer(bool exclude_self) : impl_(std::make_unique<Impl>()), exclude_self_(exclude_self) {}

SckCapturer::~SckCapturer() { Stop(); }

bool SckCapturer::Start(const capture::CaptureConfig& cfg, capture::FrameFn on_frame) {
  if (impl_->stream) {
    error_ = "already running";
    return false;
  }

  // Shareable content is delivered asynchronously; wait for it.
  __block SCShareableContent* content = nil;
  __block NSError* content_error = nil;
  dispatch_semaphore_t sem = dispatch_semaphore_create(0);
  [SCShareableContent getShareableContentWithCompletionHandler:^(SCShareableContent* c, NSError* e) {
    content = c;
    content_error = e;
    dispatch_semaphore_signal(sem);
  }];
  dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);
  if (!content || content.displays.count == 0) {
    error_ = "no display available";
    if (content_error) {
      error_ += std::string(": ") + content_error.localizedDescription.UTF8String +
                " (grant Screen Recording permission to this app/terminal)";
    }
    return false;
  }

  CGDirectDisplayID want = 0;
  if (!MacDisplayId(cfg.display_index, want, &error_)) return false;
  SCDisplay* display = nil;
  for (SCDisplay* d in content.displays) {
    if (d.displayID == want) display = d;
  }
  if (!display) {
    error_ = "display " + std::to_string(cfg.display_index) + " is not available for capture";
    return false;
  }
  NSMutableArray<SCRunningApplication*>* excluded = [NSMutableArray array];
  if (exclude_self_ && !cfg.include_own_windows) {
    const pid_t me = getpid();
    for (SCRunningApplication* app in content.applications) {
      if (app.processID == me) [excluded addObject:app];
    }
  }
  SCContentFilter* filter = [[SCContentFilter alloc] initWithDisplay:display
                                               excludingApplications:excluded
                                                    exceptingWindows:@[]];

  SCStreamConfiguration* conf = [[SCStreamConfiguration alloc] init];
  double scale = 0;
  if (@available(macOS 14.0, *)) scale = filter.pointPixelScale;
  if (scale <= 0) scale = rdacore::platform::MacDisplayScale(display.displayID);
  conf.width = cfg.width > 0 ? static_cast<size_t>(cfg.width)
                             : static_cast<size_t>(std::lround(display.width * scale));
  conf.height = cfg.height > 0 ? static_cast<size_t>(cfg.height)
                               : static_cast<size_t>(std::lround(display.height * scale));
  conf.pixelFormat = kCVPixelFormatType_32BGRA;
  conf.minimumFrameInterval = CMTimeMake(1, cfg.fps > 0 ? cfg.fps : 60);
  conf.showsCursor = cfg.show_cursor ? YES : NO;
  conf.queueDepth = 5;
  // Video only: rdaccel never captures or plays audio. The property exists
  // from macOS 13 (and is NO by default); 12.x has no audio capture at all.
  if (@available(macOS 13.0, *)) conf.capturesAudio = NO;

  impl_->queue = dispatch_queue_create("rdaccel.capture", DISPATCH_QUEUE_SERIAL);
  impl_->output = [[RDStreamOutput alloc] initWithCallback:std::move(on_frame) stopped:on_stopped_];
  impl_->stream = [[SCStream alloc] initWithFilter:filter configuration:conf delegate:impl_->output];
  NSError* err = nil;
  if (![impl_->stream addStreamOutput:impl_->output
                                 type:SCStreamOutputTypeScreen
                   sampleHandlerQueue:impl_->queue
                                error:&err]) {
    error_ = std::string("addStreamOutput: ") + err.localizedDescription.UTF8String;
    impl_->stream = nil;
    return false;
  }

  __block NSError* start_error = nil;
  [impl_->stream startCaptureWithCompletionHandler:^(NSError* e) {
    start_error = e;
    dispatch_semaphore_signal(sem);
  }];
  dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);
  if (start_error) {
    error_ = std::string("startCapture: ") + start_error.localizedDescription.UTF8String +
             " (grant Screen Recording permission to this app/terminal)";
    impl_->stream = nil;
    return false;
  }
  return true;
}

void SckCapturer::Stop() {
  if (!impl_ || !impl_->stream) return;
  [impl_->output ignoreStops];  // stopping on purpose is not news
  dispatch_semaphore_t sem = dispatch_semaphore_create(0);
  [impl_->stream stopCaptureWithCompletionHandler:^(NSError*) {
    dispatch_semaphore_signal(sem);
  }];
  dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC));
  // Drain callbacks that were already queued before dropping the output.
  dispatch_sync(impl_->queue, ^{
                });
  impl_->stream = nil;
  impl_->output = nil;
}

}  // namespace rdacore::platform
