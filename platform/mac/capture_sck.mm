#include "mac/capture_sck.h"

#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>

#include <unistd.h>

#include <cmath>

using rdacore::capture::CapturedFrame;
using rdacore::capture::FrameFn;

@interface RDStreamOutput : NSObject <SCStreamOutput>
- (instancetype)initWithCallback:(FrameFn)fn;
@end

@implementation RDStreamOutput {
  FrameFn _fn;
}

- (instancetype)initWithCallback:(FrameFn)fn {
  if ((self = [super init])) _fn = std::move(fn);
  return self;
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

  // Dirty rects are in points relative to the content rect; convert to
  // buffer pixels. If anything is missing, leave the list empty, which makes
  // the tile differ check the whole frame (correct, just slower).
  NSArray* dirty = info[SCStreamFrameInfoDirtyRects];
  NSDictionary* content_dict = info[SCStreamFrameInfoContentRect];
  CGRect content = CGRectZero;
  if (dirty && content_dict &&
      CGRectMakeWithDictionaryRepresentation((__bridge CFDictionaryRef)content_dict, &content) &&
      content.size.width > 0) {
    const double scale = f.bgra.width / content.size.width;
    for (NSDictionary* d in dirty) {
      CGRect r;
      if (!CGRectMakeWithDictionaryRepresentation((__bridge CFDictionaryRef)d, &r)) continue;
      const auto x0 = static_cast<int32_t>(std::floor((r.origin.x - content.origin.x) * scale));
      const auto y0 = static_cast<int32_t>(std::floor((r.origin.y - content.origin.y) * scale));
      const auto x1 = static_cast<int32_t>(std::ceil((CGRectGetMaxX(r) - content.origin.x) * scale));
      const auto y1 = static_cast<int32_t>(std::ceil((CGRectGetMaxY(r) - content.origin.y) * scale));
      // Qualified: MacTypes.h also declares a global Rect.
      f.dirty.push_back(rdacore::Rect{x0, y0, x1 - x0, y1 - y0});
    }
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

  SCDisplay* display = content.displays.firstObject;
  NSMutableArray<SCRunningApplication*>* excluded = [NSMutableArray array];
  if (exclude_self_) {
    const pid_t me = getpid();
    for (SCRunningApplication* app in content.applications) {
      if (app.processID == me) [excluded addObject:app];
    }
  }
  SCContentFilter* filter = [[SCContentFilter alloc] initWithDisplay:display
                                               excludingApplications:excluded
                                                    exceptingWindows:@[]];

  SCStreamConfiguration* conf = [[SCStreamConfiguration alloc] init];
  const double scale = filter.pointPixelScale > 0 ? filter.pointPixelScale : 1.0;
  conf.width = cfg.width > 0 ? static_cast<size_t>(cfg.width)
                             : static_cast<size_t>(std::lround(display.width * scale));
  conf.height = cfg.height > 0 ? static_cast<size_t>(cfg.height)
                               : static_cast<size_t>(std::lround(display.height * scale));
  conf.pixelFormat = kCVPixelFormatType_32BGRA;
  conf.minimumFrameInterval = CMTimeMake(1, cfg.fps > 0 ? cfg.fps : 60);
  conf.showsCursor = cfg.show_cursor ? YES : NO;
  conf.queueDepth = 5;
  // Video only: rdaccel never captures or plays audio.
  conf.capturesAudio = NO;

  impl_->queue = dispatch_queue_create("rdaccel.capture", DISPATCH_QUEUE_SERIAL);
  impl_->output = [[RDStreamOutput alloc] initWithCallback:std::move(on_frame)];
  impl_->stream = [[SCStream alloc] initWithFilter:filter configuration:conf delegate:nil];
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
