#pragma once

#include <cstdint>

namespace rdacore {

enum class InputKind : uint8_t {
  kMouseMove = 1,
  kMouseDown = 2,
  kMouseUp = 3,
  kScroll = 4,
  kKeyDown = 5,
  kKeyUp = 6,
};

// A viewer-side input event, sent to the host. The library only carries
// these; injecting them into the host OS is up to the application for now.
struct InputEvent {
  InputKind kind = InputKind::kMouseMove;
  int32_t x = 0;  // host physical pixels (mouse events)
  int32_t y = 0;
  int32_t delta_x = 0;  // scroll
  int32_t delta_y = 0;
  uint32_t code = 0;  // mouse button or macOS virtual key code
  uint32_t modifiers = 0;
  friend bool operator==(const InputEvent&, const InputEvent&) = default;
};

}  // namespace rdacore
