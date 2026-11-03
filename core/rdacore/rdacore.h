#pragma once

// Internal C++ API behind the public C ABI (include/rdaccel/rdaccel.h).
// The library itself and its unit tests use these; applications do not.
//
//   Host   (host.h)    capture or receive screen frames, encode, send
//   Viewer (viewer.h)  receive, decode, hand pictures to the application
//   Transport (transport.h)  in-process pipe, TCP, UDP, or your own
//   Cursor (cursor.h)  the mouse cursor, sent apart from the picture

#include <cstdint>
#include <string>
#include <vector>

#include "rdacore/connection.h"
#include "rdacore/cursor.h"
#include "rdacore/geometry.h"
#include "rdacore/host.h"
#include "rdacore/image.h"
#include "rdacore/input.h"
#include "rdacore/keycodes.h"
#include "rdacore/recording.h"
#include "rdacore/test_pattern.h"
#include "rdacore/transport.h"
#include "rdacore/viewer.h"

namespace rdacore {

// One display (monitor). Origin in desktop units (points on macOS, pixels
// on X11), size in pixels.
struct DisplayInfo {
  int32_t index = 0;
  int32_t x = 0, y = 0;
  int32_t width = 0, height = 0;
  bool primary = false;
  std::string name;
};

// Library version, e.g. "0.1.0".
const char* Version();

// "macOS" or "Linux".
const char* PlatformName();

// Monotonic clock in microseconds, the same one the library uses.
uint64_t NowMicros();

// The displays StartScreenCapture() can capture, primary first (index 0).
// False with `error` set when there is none (no X server, ...).
bool ListDisplays(std::vector<DisplayInfo>& out, std::string* error = nullptr);

}  // namespace rdacore
