#include <algorithm>

#include "platform.h"
#include "linux/displays_x11.h"

#include <X11/extensions/Xrandr.h>

namespace rdacore::platform {

std::vector<capture::DisplayInfo> X11Displays(Display* dpy) {
  std::vector<capture::DisplayInfo> out;
  const int screen = DefaultScreen(dpy);
  const Window root = RootWindow(dpy, screen);
  const int sw = DisplayWidth(dpy, screen);
  const int sh = DisplayHeight(dpy, screen);
  int ev = 0, err = 0, major = 0, minor = 0;
  if (XRRQueryExtension(dpy, &ev, &err) && XRRQueryVersion(dpy, &major, &minor) &&
      (major > 1 || (major == 1 && minor >= 5))) {
    int n = 0;
    if (XRRMonitorInfo* m = XRRGetMonitors(dpy, root, True, &n)) {
      for (int i = 0; i < n; ++i) {
        capture::DisplayInfo d;
        // Clipped to the screen, so a capture never reads outside the root.
        const int x0 = std::clamp(m[i].x, 0, sw), y0 = std::clamp(m[i].y, 0, sh);
        const int x1 = std::clamp(m[i].x + m[i].width, 0, sw);
        const int y1 = std::clamp(m[i].y + m[i].height, 0, sh);
        if (x1 - x0 < 2 || y1 - y0 < 2) continue;
        d.x = x0;
        d.y = y0;
        d.width = x1 - x0;
        d.height = y1 - y0;
        d.primary = m[i].primary != 0;
        if (char* name = XGetAtomName(dpy, m[i].name)) {
          d.name = name;
          XFree(name);
        }
        out.push_back(std::move(d));
      }
      XRRFreeMonitors(m);
    }
  }
  if (out.empty()) {
    capture::DisplayInfo d;
    d.width = sw;
    d.height = sh;
    d.primary = true;
    d.name = "screen";
    out.push_back(std::move(d));
  }
  std::stable_sort(out.begin(), out.end(), [](const capture::DisplayInfo& a, const capture::DisplayInfo& b) {
    if (a.primary != b.primary) return a.primary;
    return a.x != b.x ? a.x < b.x : a.y < b.y;
  });
  out[0].primary = true;  // RandR may flag none
  for (size_t i = 0; i < out.size(); ++i) {
    out[i].index = static_cast<int32_t>(i);
    if (i > 0) out[i].primary = false;
  }
  return out;
}

bool X11Display(Display* dpy, int32_t index, capture::DisplayInfo& out, std::string* error) {
  const auto all = X11Displays(dpy);
  if (index < 0 || static_cast<size_t>(index) >= all.size()) {
    if (error) {
      *error = "no display " + std::to_string(index) + " (this desktop has " +
               std::to_string(all.size()) + ")";
    }
    return false;
  }
  out = all[static_cast<size_t>(index)];
  return true;
}

bool ListDisplays(std::vector<capture::DisplayInfo>& out, std::string* error) {
  Display* dpy = XOpenDisplay(nullptr);
  if (!dpy) {
    if (error) *error = "cannot open X display (is DISPLAY set?)";
    return false;
  }
  out = X11Displays(dpy);
  XCloseDisplay(dpy);
  return true;
}

}  // namespace rdacore::platform
