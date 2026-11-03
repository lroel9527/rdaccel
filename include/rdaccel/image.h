#pragma once

#include <cstdint>

namespace rdaccel {

// A read-only view of a 32-bit-per-pixel image. Pixels are BGRA (byte order
// B, G, R, A in memory) on every platform; alpha is ignored.
struct ImageView {
  const uint8_t* data = nullptr;
  int32_t width = 0;
  int32_t height = 0;
  int32_t stride = 0;  // bytes per row
};

}  // namespace rdaccel
