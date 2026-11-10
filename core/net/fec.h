#pragma once

// Forward error correction for the reliable UDP transport: one parity packet
// per group of data packets, the XOR of their bodies, so the receiver can
// rebuild any single lost packet of the group at once instead of waiting a
// round trip for its resend (which holds up every later byte of that
// channel: a picture behind a lost packet waits ~1 RTT).
//
// A body is a data datagram without its type byte and packet number:
// [channel u8][offset varint][bytes]. Bodies differ in length; each is
// padded with zeros to the longest one, and the XOR of the lengths travels
// along so the rebuilt body gets its own length back.
//
// Wire format of a parity datagram (after its type byte):
//   [packet number varint]            the parity's own, acknowledged like data
//   [count u8][first varint]          the protected data packet numbers:
//   [delta varint] x (count - 1)      first, first + d1, first + d1 + d2, ...
//   [length xor u16le][body xor bytes]

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <vector>

namespace rdacore::net::fec {

inline constexpr size_t kMaxGroup = 32;

struct Parity {
  std::vector<uint64_t> pns;  // protected data packets, ascending
  uint16_t length_xor = 0;
  std::vector<uint8_t> body_xor;
};

// Folds one more data packet into the parity. Packet numbers must ascend.
void Add(Parity& p, uint64_t pn, std::span<const uint8_t> body);

// The datagram payload after the type byte.
std::vector<uint8_t> Encode(uint64_t parity_pn, const Parity& p);
bool Decode(std::span<const uint8_t> payload, uint64_t* parity_pn, Parity* out);

// Rebuilds the body of `missing` (one of p.pns) from the parity and every
// other protected body; nullopt if one of those is not in `known`.
std::optional<std::vector<uint8_t>> Recover(const Parity& p, uint64_t missing,
                                            const std::map<uint64_t, std::vector<uint8_t>>& known);

}  // namespace rdacore::net::fec
