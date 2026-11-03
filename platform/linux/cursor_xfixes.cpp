// The X11 cursor through XFixes: XQueryPointer for the position, and
// XFixesGetCursorImage only when XFixes reports that the image changed.

#include <algorithm>
#include <memory>

#include "platform.h"
#include "linux/displays_x11.h"

// After our headers: Xlib defines macros such as None and Status.
#include <X11/Xlib.h>
#include <X11/extensions/Xfixes.h>

namespace rdacore::platform {
namespace {

class XFixesCursorSource final : public capture::ICursorSource {
 public:
  XFixesCursorSource(Display* dpy, int event_base, capture::DisplayInfo display)
      : dpy_(dpy), event_base_(event_base), display_(std::move(display)) {
    root_ = DefaultRootWindow(dpy_);
    XFixesSelectCursorInput(dpy_, root_, XFixesDisplayCursorNotifyMask);
  }
  ~XFixesCursorSource() override { XCloseDisplay(dpy_); }

  bool Poll(capture::CursorSample& out) override {
    bool changed = first_;
    first_ = false;
    while (XPending(dpy_) > 0) {
      XEvent ev;
      XNextEvent(dpy_, &ev);
      if (ev.type == event_base_ + XFixesCursorNotify) changed = true;
    }
    Window root_ret, child;
    int rx = 0, ry = 0, wx = 0, wy = 0;
    unsigned int mask = 0;
    if (!XQueryPointer(dpy_, root_, &root_ret, &child, &rx, &ry, &wx, &wy, &mask)) return false;
    // Relative to the captured display; hidden while on another one. (X has
    // no portable "hidden" flag otherwise: hidden cursors are transparent.)
    out.x = rx - display_.x;
    out.y = ry - display_.y;
    out.screen_width = display_.width;
    out.screen_height = display_.height;
    out.visible = out.x >= 0 && out.y >= 0 && out.x < display_.width && out.y < display_.height;
    out.image_changed = false;
    if (changed) {
      XFixesCursorImage* ci = XFixesGetCursorImage(dpy_);
      if (ci) {
        auto& img = out.image;
        img.width = std::min<int32_t>(ci->width, 256);
        img.height = std::min<int32_t>(ci->height, 256);
        img.hotspot_x = std::min<int32_t>(ci->xhot, img.width - 1);
        img.hotspot_y = std::min<int32_t>(ci->yhot, img.height - 1);
        img.bgra.resize(static_cast<size_t>(img.width) * img.height * 4);
        // `pixels` holds premultiplied ARGB in unsigned longs (64-bit on LP64).
        for (int32_t y = 0; y < img.height; ++y) {
          for (int32_t x = 0; x < img.width; ++x) {
            const auto p = static_cast<uint32_t>(ci->pixels[static_cast<size_t>(y) * ci->width + x]);
            uint8_t* d = &img.bgra[(static_cast<size_t>(y) * img.width + x) * 4];
            d[0] = p & 0xff;
            d[1] = (p >> 8) & 0xff;
            d[2] = (p >> 16) & 0xff;
            d[3] = (p >> 24) & 0xff;
          }
        }
        out.image_changed = img.width > 0 && img.height > 0;
        XFree(ci);
      }
    }
    return true;
  }

 private:
  Display* dpy_;
  int event_base_;
  Window root_;
  capture::DisplayInfo display_;
  bool first_ = true;
};

}  // namespace

std::unique_ptr<capture::ICursorSource> CreateCursorSource(int32_t display_index, std::string* error) {
  Display* dpy = XOpenDisplay(nullptr);
  if (!dpy) {
    if (error) *error = "cannot open X display";
    return nullptr;
  }
  int event_base = 0, error_base = 0;
  if (!XFixesQueryExtension(dpy, &event_base, &error_base)) {
    XCloseDisplay(dpy);
    if (error) *error = "X server has no XFixes extension";
    return nullptr;
  }
  capture::DisplayInfo display;
  if (!X11Display(dpy, display_index, display, error)) {
    XCloseDisplay(dpy);
    return nullptr;
  }
  return std::make_unique<XFixesCursorSource>(dpy, event_base, std::move(display));
}

}  // namespace rdacore::platform
