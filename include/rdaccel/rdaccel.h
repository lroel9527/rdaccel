#pragma once

// rdaccel: low-bandwidth remote desktop streaming.
//
// Include this header and link rdaccel::rdaccel. Everything public lives in
// include/rdaccel/; nothing else is part of the API.
//
//   Host   (host.h)    capture or receive screen frames, encode, send
//   Viewer (viewer.h)  receive, decode, hand pictures to the application
//   Transport (transport.h)  in-process pipe, TCP, UDP, or your own
//   Cursor (cursor.h)  the mouse cursor, sent apart from the picture

#include <cstdint>

#include "rdaccel/cursor.h"
#include "rdaccel/geometry.h"
#include "rdaccel/host.h"
#include "rdaccel/image.h"
#include "rdaccel/input.h"
#include "rdaccel/recording.h"
#include "rdaccel/test_pattern.h"
#include "rdaccel/transport.h"
#include "rdaccel/viewer.h"

namespace rdaccel {

// Library version, e.g. "0.1.0".
const char* Version();

// "macOS" or "Linux".
const char* PlatformName();

// Monotonic clock in microseconds, the same one the library uses.
uint64_t NowMicros();

}  // namespace rdaccel
