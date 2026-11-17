#include <gtest/gtest.h>

#include "rdacore/keycodes.h"

using namespace rdacore;

TEST(KeyCodes, KnownKeys) {
  EXPECT_EQ(HidFromMacKeyCode(0x00), 0x04u);  // A (kVK_ANSI_A is 0)
  EXPECT_EQ(HidToMacKeyCode(0x04), 0x00u);
  EXPECT_EQ(HidFromMacKeyCode(0x24), 0x28u);  // Return
  EXPECT_EQ(HidFromMacKeyCode(0x37), 0xE3u);  // Command = left Meta
  EXPECT_EQ(HidToMacKeyCode(0x46), kNoKeyCode);  // Print Screen: no Mac key
  EXPECT_EQ(HidFromMacKeyCode(0x3F), 0u);     // Fn: no HID keyboard usage
  EXPECT_EQ(HidToMacKeyCode(0), kNoKeyCode);
  EXPECT_EQ(HidToMacKeyCode(0x1234), kNoKeyCode);
}

TEST(KeyCodes, RoundTrips) {
  int mac = 0;
  for (uint32_t hid = 1; hid < 0x100; ++hid) {
    if (hid == 0x32) continue;  // ISO "Non-US #" shares the backslash key
    if (const uint32_t m = HidToMacKeyCode(hid); m != kNoKeyCode) {
      EXPECT_EQ(HidFromMacKeyCode(m), hid) << std::hex << hid;
      ++mac;
    }
  }
  EXPECT_GT(mac, 110);
}

TEST(KeyCodes, NativeCodesAreUnique) {
  for (uint32_t code = 0; code < 256; ++code) {
    if (const uint32_t hid = HidFromMacKeyCode(code); hid && hid != 0x32) {
      EXPECT_EQ(HidToMacKeyCode(hid), code) << "mac " << code;
    }
  }
}
