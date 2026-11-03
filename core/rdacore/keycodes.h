#pragma once

// Key codes on the wire are USB HID usages (keyboard page 0x07), so a Mac
// viewer and a Linux host agree on what was pressed. These convert from and
// to the native codes applications see.

#include <cstdint>

namespace rdacore {

// No such key (native codes can be 0, so this is the "none" value for them).
inline constexpr uint32_t kNoKeyCode = 0xFFFFFFFFu;

// macOS virtual key code (kVK_*, NSEvent.keyCode) -> HID usage; 0 if unknown.
uint32_t HidFromMacKeyCode(uint32_t mac);
// HID usage -> macOS virtual key code; kNoKeyCode if there is none.
uint32_t HidToMacKeyCode(uint32_t hid);
// Linux evdev code (KEY_*, X11 keycode - 8) -> HID usage; 0 if unknown.
uint32_t HidFromEvdevKeyCode(uint32_t evdev);
// HID usage -> Linux evdev code; kNoKeyCode if there is none.
uint32_t HidToEvdevKeyCode(uint32_t hid);

}  // namespace rdacore
