#pragma once

// The active displays, main display first, then left to right.

#import <CoreGraphics/CoreGraphics.h>

#include <cstdint>
#include <string>
#include <vector>

namespace rdacore::platform {

std::vector<CGDirectDisplayID> MacDisplayIds();

// The display `index`, or false with `error` set.
bool MacDisplayId(int32_t index, CGDirectDisplayID& out, std::string* error);

// Backing scale (pixels per point) of a display; 1 if unknown.
double MacDisplayScale(CGDirectDisplayID id);

}  // namespace rdacore::platform
