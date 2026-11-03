#pragma once

// The monitors of an X screen, through RandR 1.5 monitors (what desktops
// show as displays); the whole screen as one display without RandR.

#include <string>
#include <vector>

#include "capture/capturer.h"

// After our headers: Xlib defines macros such as None and Status.
#include <X11/Xlib.h>

namespace rdacore::platform {

// Never empty for an open display: primary first, then left to right.
std::vector<capture::DisplayInfo> X11Displays(Display* dpy);

// The display `index` of `dpy`, or false with `error` set.
bool X11Display(Display* dpy, int32_t index, capture::DisplayInfo& out, std::string* error);

}  // namespace rdacore::platform
