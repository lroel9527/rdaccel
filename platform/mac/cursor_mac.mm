// The macOS cursor: CGEventGetLocation for the position (global points) and
// [NSCursor currentSystemCursor] for the image, rendered at the
// captured display's backing scale. The image is re-read every few polls and only
// reported when its pixels differ.

#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>

#include <algorithm>
#include <cmath>
#include <memory>

#include "platform.h"
#include "mac/displays_mac.h"

namespace rdacore::platform {
namespace {

constexpr int kImageEvery = 4;  // polls between image reads (~30 Hz at 120 Hz polling)

bool RenderCursor(CursorImage& out, CGFloat scale) {
  @autoreleasepool {
    NSCursor* cursor = [NSCursor currentSystemCursor];
    if (!cursor) return false;
    NSImage* image = cursor.image;
    const NSSize size = image.size;  // points
    if (size.width <= 0 || size.height <= 0) return false;
    const int w = std::min(256, static_cast<int>(std::ceil(size.width * scale)));
    const int h = std::min(256, static_cast<int>(std::ceil(size.height * scale)));
    out.width = w;
    out.height = h;
    out.bgra.assign(static_cast<size_t>(w) * h * 4, 0);
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    // BGRA in memory, premultiplied; row 0 of the buffer is the top row.
    CGContextRef ctx = CGBitmapContextCreate(out.bgra.data(), w, h, 8, static_cast<size_t>(w) * 4, cs,
                                             kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
    CGColorSpaceRelease(cs);
    if (!ctx) return false;
    NSGraphicsContext* g = [NSGraphicsContext graphicsContextWithCGContext:ctx flipped:NO];
    [NSGraphicsContext saveGraphicsState];
    [NSGraphicsContext setCurrentContext:g];
    [image drawInRect:NSMakeRect(0, 0, w, h)
             fromRect:NSZeroRect
            operation:NSCompositingOperationCopy
             fraction:1.0];
    [NSGraphicsContext restoreGraphicsState];
    CGContextRelease(ctx);
    const NSPoint hot = cursor.hotSpot;  // points from the top-left corner
    out.hotspot_x = std::clamp(static_cast<int>(std::lround(hot.x * scale)), 0, w - 1);
    out.hotspot_y = std::clamp(static_cast<int>(std::lround(hot.y * scale)), 0, h - 1);
    return true;
  }
}

class MacCursorSource final : public capture::ICursorSource {
 public:
  explicit MacCursorSource(CGDirectDisplayID display) : display_(display) {}

  bool Poll(capture::CursorSample& out) override {
    CGEventRef ev = CGEventCreate(nullptr);
    if (!ev) return false;
    const CGPoint p = CGEventGetLocation(ev);
    CFRelease(ev);
    // Relative to the captured display; hidden while on another one.
    const CGRect b = CGDisplayBounds(display_);
    out.x = p.x - b.origin.x;
    out.y = p.y - b.origin.y;
    out.screen_width = b.size.width;
    out.screen_height = b.size.height;
    // RenderCursor draws at this scale.
    if (polls_ % kImageEvery == 0) scale_ = MacDisplayScale(display_);
    out.image_scale = scale_;
    out.visible = out.x >= 0 && out.y >= 0 && out.x < b.size.width && out.y < b.size.height;
    out.image_changed = false;
    if (polls_++ % kImageEvery == 0) {
      CursorImage img;
      if (RenderCursor(img, scale_) && !(img == last_)) {
        last_ = img;
        out.image = std::move(img);
        out.image_changed = true;
      }
    }
    return true;
  }

 private:
  CGDirectDisplayID display_;
  double scale_ = 1;
  uint64_t polls_ = 0;
  CursorImage last_;
};

}  // namespace

std::unique_ptr<capture::ICursorSource> CreateCursorSource(int32_t display_index, std::string* error) {
  CGDirectDisplayID id = 0;
  if (!MacDisplayId(display_index, id, error)) return nullptr;
  return std::make_unique<MacCursorSource>(id);
}

}  // namespace rdacore::platform
