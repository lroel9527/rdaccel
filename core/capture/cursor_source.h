#pragma once

// Reads the system mouse cursor (position and image) for the cursor channel.
// Implementations:
//   * macOS: CoreGraphics position + NSCursor image (platform/mac/cursor_mac.mm)
//   * Linux: XFixes (platform/linux/cursor_xfixes.cpp)

#include <cstdint>
#include <string>

#include "rdacore/cursor.h"

namespace rdacore::capture {

struct CursorSample {
  // Pointer position and the size of the screen it is measured on, in the
  // same units (points on macOS, pixels on X11). The host scales them to the
  // captured frame.
  double x = 0, y = 0;
  double screen_width = 0, screen_height = 0;
  // Image pixels per unit of the above (2 for a Retina cursor measured in
  // points). The host resizes the image to the captured frame's scale.
  double image_scale = 1;
  bool visible = true;
  bool image_changed = false;  // `image` holds a new image
  CursorImage image;
};

class ICursorSource {
 public:
  virtual ~ICursorSource() = default;
  // Called from one thread at a time, at the polling rate. Fills `image`
  // only when it changed since the previous call (and on the first call).
  virtual bool Poll(CursorSample& out) = 0;
};

}  // namespace rdacore::capture
