#include "rdacore/keycodes.h"

namespace rdacore {
namespace {

constexpr int kNone = -1;

struct Key {
  uint16_t hid;
  int16_t mac;    // kVK_*
  int16_t evdev;  // KEY_*
};

// One row per HID usage. Where several native codes map to one usage the
// first row wins for the reverse direction. Follows the USB HID Usage
// Tables and the mappings browsers use (UI Events KeyboardEvent.code).
constexpr Key kKeys[] = {
    {0x04, 0x00, 30},  // A
    {0x05, 0x0B, 48},  // B
    {0x06, 0x08, 46},  // C
    {0x07, 0x02, 32},  // D
    {0x08, 0x0E, 18},  // E
    {0x09, 0x03, 33},  // F
    {0x0A, 0x05, 34},  // G
    {0x0B, 0x04, 35},  // H
    {0x0C, 0x22, 23},  // I
    {0x0D, 0x26, 36},  // J
    {0x0E, 0x28, 37},  // K
    {0x0F, 0x25, 38},  // L
    {0x10, 0x2E, 50},  // M
    {0x11, 0x2D, 49},  // N
    {0x12, 0x1F, 24},  // O
    {0x13, 0x23, 25},  // P
    {0x14, 0x0C, 16},  // Q
    {0x15, 0x0F, 19},  // R
    {0x16, 0x01, 31},  // S
    {0x17, 0x11, 20},  // T
    {0x18, 0x20, 22},  // U
    {0x19, 0x09, 47},  // V
    {0x1A, 0x0D, 17},  // W
    {0x1B, 0x07, 45},  // X
    {0x1C, 0x10, 21},  // Y
    {0x1D, 0x06, 44},  // Z
    {0x1E, 0x12, 2},   // 1
    {0x1F, 0x13, 3},   // 2
    {0x20, 0x14, 4},   // 3
    {0x21, 0x15, 5},   // 4
    {0x22, 0x17, 6},   // 5
    {0x23, 0x16, 7},   // 6
    {0x24, 0x1A, 8},   // 7
    {0x25, 0x1C, 9},   // 8
    {0x26, 0x19, 10},  // 9
    {0x27, 0x1D, 11},  // 0
    {0x28, 0x24, 28},  // Enter
    {0x29, 0x35, 1},   // Escape
    {0x2A, 0x33, 14},  // Backspace (Mac "Delete")
    {0x2B, 0x30, 15},  // Tab
    {0x2C, 0x31, 57},  // Space
    {0x2D, 0x1B, 12},  // - _
    {0x2E, 0x18, 13},  // = +
    {0x2F, 0x21, 26},  // [ {
    {0x30, 0x1E, 27},  // ] }
    {0x31, 0x2A, 43},  // \ |
    {0x32, 0x2A, 43},  // Non-US # ~ (ISO; same key position as \ |)
    {0x33, 0x29, 39},  // ; :
    {0x34, 0x27, 40},  // ' "
    {0x35, 0x32, 41},  // ` ~
    {0x36, 0x2B, 51},  // , <
    {0x37, 0x2F, 52},  // . >
    {0x38, 0x2C, 53},  // / ?
    {0x39, 0x39, 58},  // Caps Lock
    {0x3A, 0x7A, 59},  // F1
    {0x3B, 0x78, 60},  // F2
    {0x3C, 0x63, 61},  // F3
    {0x3D, 0x76, 62},  // F4
    {0x3E, 0x60, 63},  // F5
    {0x3F, 0x61, 64},  // F6
    {0x40, 0x62, 65},  // F7
    {0x41, 0x64, 66},  // F8
    {0x42, 0x65, 67},  // F9
    {0x43, 0x6D, 68},  // F10
    {0x44, 0x67, 87},  // F11
    {0x45, 0x6F, 88},  // F12
    {0x46, kNone, 99},   // Print Screen
    {0x47, kNone, 70},   // Scroll Lock
    {0x48, kNone, 119},  // Pause
    {0x49, 0x72, 110},   // Insert (Mac "Help")
    {0x4A, 0x73, 102},   // Home
    {0x4B, 0x74, 104},   // Page Up
    {0x4C, 0x75, 111},   // Delete forward
    {0x4D, 0x77, 107},   // End
    {0x4E, 0x79, 109},   // Page Down
    {0x4F, 0x7C, 106},   // Right
    {0x50, 0x7B, 105},   // Left
    {0x51, 0x7D, 108},   // Down
    {0x52, 0x7E, 103},   // Up
    {0x53, 0x47, 69},    // Num Lock (Mac keypad "Clear")
    {0x54, 0x4B, 98},    // Keypad /
    {0x55, 0x43, 55},    // Keypad *
    {0x56, 0x4E, 74},    // Keypad -
    {0x57, 0x45, 78},    // Keypad +
    {0x58, 0x4C, 96},    // Keypad Enter
    {0x59, 0x53, 79},    // Keypad 1
    {0x5A, 0x54, 80},    // Keypad 2
    {0x5B, 0x55, 81},    // Keypad 3
    {0x5C, 0x56, 75},    // Keypad 4
    {0x5D, 0x57, 76},    // Keypad 5
    {0x5E, 0x58, 77},    // Keypad 6
    {0x5F, 0x59, 71},    // Keypad 7
    {0x60, 0x5B, 72},    // Keypad 8
    {0x61, 0x5C, 73},    // Keypad 9
    {0x62, 0x52, 82},    // Keypad 0
    {0x63, 0x41, 83},    // Keypad .
    {0x64, 0x0A, 86},    // Non-US \ | (ISO key next to left Shift; Mac "Section")
    {0x65, 0x6E, 127},   // Application / context menu
    {0x66, kNone, 116},  // Power
    {0x67, 0x51, 117},   // Keypad =
    {0x68, 0x69, 183},   // F13
    {0x69, 0x6B, 184},   // F14
    {0x6A, 0x71, 185},   // F15
    {0x6B, 0x6A, 186},   // F16
    {0x6C, 0x40, 187},   // F17
    {0x6D, 0x4F, 188},   // F18
    {0x6E, 0x50, 189},   // F19
    {0x6F, 0x5A, 190},   // F20
    {0x70, kNone, 191},  // F21
    {0x71, kNone, 192},  // F22
    {0x72, kNone, 193},  // F23
    {0x73, kNone, 194},  // F24
    {0x85, 0x5F, 121},   // Keypad , (Brazil, JIS)
    {0x87, 0x5E, 89},    // International 1: JIS Ro / underscore
    {0x88, kNone, 93},   // International 2: Katakana/Hiragana
    {0x89, 0x5D, 124},   // International 3: Yen
    {0x8A, kNone, 92},   // International 4: Henkan
    {0x8B, kNone, 94},   // International 5: Muhenkan
    {0x8C, kNone, 95},   // International 6: keypad JP comma
    {0x90, 0x68, 122},   // Lang 1: Hangul / Mac Kana
    {0x91, 0x66, 123},   // Lang 2: Hanja / Mac Eisu
    {0x92, kNone, 90},   // Lang 3: Katakana
    {0x93, kNone, 91},   // Lang 4: Hiragana
    {0x94, kNone, 85},   // Lang 5: Zenkaku/Hankaku
    {0xE0, 0x3B, 29},    // Left Control
    {0xE1, 0x38, 42},    // Left Shift
    {0xE2, 0x3A, 56},    // Left Alt / Option
    {0xE3, 0x37, 125},   // Left Meta / Command
    {0xE4, 0x3E, 97},    // Right Control
    {0xE5, 0x3C, 54},    // Right Shift
    {0xE6, 0x3D, 100},   // Right Alt / Option
    {0xE7, 0x36, 126},   // Right Meta / Command
};

}  // namespace

uint32_t HidFromMacKeyCode(uint32_t mac) {
  for (const Key& k : kKeys) {
    if (k.mac != kNone && static_cast<uint32_t>(k.mac) == mac) return k.hid;
  }
  return 0;
}

uint32_t HidToMacKeyCode(uint32_t hid) {
  for (const Key& k : kKeys) {
    if (k.hid == hid) return k.mac == kNone ? kNoKeyCode : static_cast<uint32_t>(k.mac);
  }
  return kNoKeyCode;
}

uint32_t HidFromEvdevKeyCode(uint32_t evdev) {
  for (const Key& k : kKeys) {
    if (k.evdev != kNone && static_cast<uint32_t>(k.evdev) == evdev) return k.hid;
  }
  return 0;
}

uint32_t HidToEvdevKeyCode(uint32_t hid) {
  for (const Key& k : kKeys) {
    if (k.hid == hid) return k.evdev == kNone ? kNoKeyCode : static_cast<uint32_t>(k.evdev);
  }
  return kNoKeyCode;
}

}  // namespace rdacore
