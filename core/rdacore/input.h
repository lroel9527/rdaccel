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
  // Typed text, one Unicode scalar value in `code` (no surrogates). For
  // text the viewer's keyboard layout produced, or an IME; the host types
  // it as-is, whatever its own layout.
  kText = 7,
};

inline constexpr uint32_t kButtonLeft = 1;
inline constexpr uint32_t kButtonRight = 2;
inline constexpr uint32_t kButtonMiddle = 3;
inline constexpr uint32_t kButtonBack = 4;
inline constexpr uint32_t kButtonForward = 5;

inline constexpr uint32_t kModShift = 1u << 0;
inline constexpr uint32_t kModCtrl = 1u << 1;
inline constexpr uint32_t kModAlt = 1u << 2;   // Option on macOS
inline constexpr uint32_t kModMeta = 1u << 3;  // Command on macOS, Windows/Super key
inline constexpr uint32_t kModCaps = 1u << 4;  // Caps Lock is on

// A viewer-side input event, sent to the host. The library only carries
// these; injecting them into the host OS is up to the application for now.
struct InputEvent {
  InputKind kind = InputKind::kMouseMove;
  int32_t x = 0;  // host physical pixels (mouse events)
  int32_t y = 0;
  int32_t delta_x = 0;  // scroll
  int32_t delta_y = 0;
  // Mouse events: a button (kButton*). Key events: a USB HID usage from the
  // keyboard page 0x07 (0x04 = A, 0x28 = Enter, 0xE0 = left Control, ...),
  // the same on every OS; see keycodes.h to convert from and to native codes.
  // Text events: a Unicode scalar value (1..0x10FFFF, not 0xD800..0xDFFF).
  uint32_t code = 0;
  uint32_t modifiers = 0;  // kMod* bits held during the event
  friend bool operator==(const InputEvent&, const InputEvent&) = default;
};

// A Unicode scalar value: what kText carries.
inline constexpr bool IsUnicodeScalar(uint32_t c) {
  return c >= 1 && c <= 0x10FFFF && (c < 0xD800 || c > 0xDFFF);
}

}  // namespace rdacore
