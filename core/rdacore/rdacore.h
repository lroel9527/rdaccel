#pragma once

// Internal C++ API behind the public C ABI (include/rdaccel/rdaccel.h).
// The library itself and its unit tests use these; applications do not.
//
//   Host   (host.h)    capture or receive screen frames, encode, send
//   Viewer (viewer.h)  receive, decode, hand pictures to the application
//   Transport (transport.h)  in-process pipe, TCP, UDP, or your own
//   Cursor (cursor.h)  the mouse cursor, sent apart from the picture

#include <cstdint>

#include "rdacore/cursor.h"
#include "rdacore/geometry.h"
#include "rdacore/host.h"
#include "rdacore/image.h"
#include "rdacore/input.h"
#include "rdacore/recording.h"
#include "rdacore/test_pattern.h"
#include "rdacore/transport.h"
#include "rdacore/viewer.h"

namespace rdacore {

// Library version, e.g. "0.1.0".
const char* Version();

// "macOS" or "Linux".
const char* PlatformName();

// Monotonic clock in microseconds, the same one the library uses.
uint64_t NowMicros();

}  // namespace rdacore
