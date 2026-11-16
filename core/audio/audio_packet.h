#pragma once

// One sound packet per transport datagram (Transport::SendDatagram).
// Little-endian:
//
//   [0]      kind   u8   1 = sound, version 1; anything else is dropped
//   [1]      flags  u8   bit 0 CLOCK: an 8-byte capture time follows
//                        bit 1 RED: a copy of the previous frame follows
//                        bit 2 END: the last packet before silence
//                        bits 3-7 must be 0, or the packet is dropped
//   [2]      spurt  u8   counts up each time sound resumes after silence
//   [3..4]   seq    u16  counts up with every packet
//   [5..8]   ts     u32  the main frame's first sample, in 48 kHz samples;
//                        runs on through silence (wraps after ~24.8 hours)
//   [CLOCK]  time   u64  the main frame's first sample's capture time, us,
//                        on the host's clock (as FrameBegin's capture_time_us)
//   [RED]    len    u16  then len bytes: the Opus packet of the frame right
//                        before the main one (its ts = ts - its samples)
//            main        the rest: one Opus packet (10 or 20 ms)

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace rdacore::audio {

inline constexpr uint8_t kPacketKind = 1;
inline constexpr uint8_t kFlagClock = 1;
inline constexpr uint8_t kFlagRed = 2;
inline constexpr uint8_t kFlagEnd = 4;
inline constexpr size_t kPacketHeader = 9;
// A whole packet fits one datagram (rdacore::kMaxDatagramPayload).
inline constexpr size_t kMaxPacket = 1100;
// Largest Opus packet of one frame the sender makes.
inline constexpr size_t kMaxOpusFrame = 500;

struct Packet {
  uint8_t flags = 0;
  uint8_t spurt = 0;
  uint16_t seq = 0;
  uint32_t ts = 0;
  uint64_t time_us = 0;  // with kFlagClock
  std::span<const uint8_t> red;   // with kFlagRed
  std::span<const uint8_t> main;  // never empty
  bool clock() const { return flags & kFlagClock; }
  bool end() const { return flags & kFlagEnd; }
};

// Appends one packet to `out` (cleared first). False if it would not fit
// kMaxPacket or the main frame is empty.
bool EncodePacket(const Packet& p, std::vector<uint8_t>& out);
// Spans point into `bytes`. Nullopt for anything malformed.
std::optional<Packet> ParsePacket(std::span<const uint8_t> bytes);

// Unwraps the 32-bit ts into a 64-bit position, as the nearest to the last
// one (the first one starts it).
class TsUnwrapper {
 public:
  int64_t Unwrap(uint32_t ts);
  void Reset() { have_ = false; }

 private:
  bool have_ = false;
  int64_t last_ = 0;
};

}  // namespace rdacore::audio
