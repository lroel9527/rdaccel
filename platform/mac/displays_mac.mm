#include "mac/displays_mac.h"

#import <AppKit/AppKit.h>

#include <algorithm>
#include <cmath>

#include "platform.h"

namespace rdacore::platform {
namespace {

NSScreen* ScreenFor(CGDirectDisplayID id) {
  for (NSScreen* s in NSScreen.screens) {
    NSNumber* n = s.deviceDescription[@"NSScreenNumber"];
    if (n && n.unsignedIntValue == id) return s;
  }
  return nil;
}

}  // namespace

std::vector<CGDirectDisplayID> MacDisplayIds() {
  uint32_t n = 0;
  if (CGGetActiveDisplayList(0, nullptr, &n) != kCGErrorSuccess || n == 0) return {};
  std::vector<CGDirectDisplayID> ids(n);
  if (CGGetActiveDisplayList(n, ids.data(), &n) != kCGErrorSuccess) return {};
  ids.resize(n);
  // Mirrored displays show the same picture: keep one of each set.
  ids.erase(std::remove_if(ids.begin(), ids.end(),
                           [](CGDirectDisplayID id) { return CGDisplayMirrorsDisplay(id) != kCGNullDirectDisplay; }),
            ids.end());
  const CGDirectDisplayID main = CGMainDisplayID();
  std::stable_sort(ids.begin(), ids.end(), [main](CGDirectDisplayID a, CGDirectDisplayID b) {
    if ((a == main) != (b == main)) return a == main;
    const CGRect ra = CGDisplayBounds(a), rb = CGDisplayBounds(b);
    return ra.origin.x != rb.origin.x ? ra.origin.x < rb.origin.x : ra.origin.y < rb.origin.y;
  });
  return ids;
}

bool MacDisplayId(int32_t index, CGDirectDisplayID& out, std::string* error) {
  const auto ids = MacDisplayIds();
  if (index < 0 || static_cast<size_t>(index) >= ids.size()) {
    if (error) {
      *error = "no display " + std::to_string(index) + " (this Mac has " + std::to_string(ids.size()) + ")";
    }
    return false;
  }
  out = ids[static_cast<size_t>(index)];
  return true;
}

double MacDisplayScale(CGDirectDisplayID id) {
  // From the display's current mode, not NSScreen: NSScreen only follows a
  // mode change or a display that was just added once the main run loop has
  // handled the notification, and a command-line host may have none.
  if (CGDisplayModeRef mode = CGDisplayCopyDisplayMode(id)) {
    const size_t points = CGDisplayModeGetWidth(mode), pixels = CGDisplayModeGetPixelWidth(mode);
    CGDisplayModeRelease(mode);
    if (points > 0 && pixels > 0) return static_cast<double>(pixels) / static_cast<double>(points);
  }
  @autoreleasepool {
    NSScreen* s = ScreenFor(id);
    return s && s.backingScaleFactor > 0 ? s.backingScaleFactor : 1.0;
  }
}

bool ListDisplays(std::vector<capture::DisplayInfo>& out, std::string* error) {
  const auto ids = MacDisplayIds();
  if (ids.empty()) {
    if (error) *error = "no display";
    return false;
  }
  out.clear();
  @autoreleasepool {
    for (size_t i = 0; i < ids.size(); ++i) {
      capture::DisplayInfo d;
      d.index = static_cast<int32_t>(i);
      const CGRect b = CGDisplayBounds(ids[i]);  // points
      d.x = static_cast<int32_t>(b.origin.x);
      d.y = static_cast<int32_t>(b.origin.y);
      const double scale = MacDisplayScale(ids[i]);
      d.width = static_cast<int32_t>(std::lround(b.size.width * scale));
      d.height = static_cast<int32_t>(std::lround(b.size.height * scale));
      d.primary = i == 0;
      if (NSScreen* s = ScreenFor(ids[i])) {
        if (const char* name = s.localizedName.UTF8String) d.name = name;
      }
      out.push_back(std::move(d));
    }
  }
  return true;
}

}  // namespace rdacore::platform
