#pragma once

// The mouse cursor travels separately from the screen picture: the host sends
// the pointer position (a few bytes) and each cursor image once, and the
// viewer draws the cursor itself, on top of the picture. Moving the mouse then
// costs no video at all, the pointer stays sharp, and on a slow or congested
// link it keeps moving even while a large frame is still on its way, because
// the cursor channel is sent before the video.

#include <cstdint>
#include <vector>

namespace rdaccel {

struct CursorImage {
  int32_t width = 0;   // at most 256 x 256
  int32_t height = 0;
  int32_t hotspot_x = 0;  // the pixel of the image that sits at the pointer position
  int32_t hotspot_y = 0;
  // width * height * 4 bytes, rows packed. Byte order B, G, R, A with
  // premultiplied alpha (0 = transparent).
  std::vector<uint8_t> bgra;
  friend bool operator==(const CursorImage&, const CursorImage&) = default;
};

struct CursorState {
  int32_t x = 0;  // pointer position in host screen pixels (where the hotspot goes)
  int32_t y = 0;
  bool visible = true;
  uint64_t time_us = 0;  // host clock, like FrameInfo::capture_time_us
  // Identifies the image; it changes exactly when the image does. 0 = no
  // image received (draw a default arrow).
  uint64_t image_id = 0;
  bool image_changed = false;           // true when image_id differs from the last call
  const CursorImage* image = nullptr;  // valid during the call; null if image_id == 0
};

// Draws `c` into a BGRA picture (e.g. the frame from Viewer's on_frame) with
// alpha blending, for applications that have no hardware cursor layer.
void DrawCursor(const CursorImage& c, int32_t x, int32_t y, uint8_t* bgra, int32_t width,
                int32_t height, int32_t stride);

}  // namespace rdaccel
