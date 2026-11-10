#include "net/fec.h"

#include <algorithm>

#include "proto/wire.h"

namespace rdacore::net::fec {

void Add(Parity& p, uint64_t pn, std::span<const uint8_t> body) {
  p.pns.push_back(pn);
  p.length_xor ^= static_cast<uint16_t>(body.size());
  if (p.body_xor.size() < body.size()) p.body_xor.resize(body.size(), 0);
  for (size_t i = 0; i < body.size(); ++i) p.body_xor[i] ^= body[i];
}

std::vector<uint8_t> Encode(uint64_t parity_pn, const Parity& p) {
  proto::ByteWriter w;
  w.varint(parity_pn);
  w.u8(static_cast<uint8_t>(p.pns.size()));
  for (size_t i = 0; i < p.pns.size(); ++i) w.varint(i == 0 ? p.pns[0] : p.pns[i] - p.pns[i - 1]);
  w.u8(static_cast<uint8_t>(p.length_xor & 0xff));
  w.u8(static_cast<uint8_t>(p.length_xor >> 8));
  w.bytes(p.body_xor);
  return w.take();
}

bool Decode(std::span<const uint8_t> payload, uint64_t* parity_pn, Parity* out) {
  proto::ByteReader r(payload);
  uint8_t count = 0;
  if (!r.varint(*parity_pn) || !r.u8(count) || count == 0 || count > kMaxGroup) return false;
  out->pns.clear();
  uint64_t pn = 0;
  for (uint8_t i = 0; i < count; ++i) {
    uint64_t v = 0;
    if (!r.varint(v)) return false;
    if (i > 0 && v == 0) return false;  // ascending, no repeats
    if (i > 0 && pn > UINT64_MAX - v) return false;
    pn = i == 0 ? v : pn + v;
    out->pns.push_back(pn);
  }
  uint8_t lo = 0, hi = 0;
  std::span<const uint8_t> rest;
  if (!r.u8(lo) || !r.u8(hi) || !r.bytes(r.remaining(), rest)) return false;
  out->length_xor = static_cast<uint16_t>(lo | (hi << 8));
  out->body_xor.assign(rest.begin(), rest.end());
  return true;
}

std::optional<std::vector<uint8_t>> Recover(const Parity& p, uint64_t missing,
                                            const std::map<uint64_t, std::vector<uint8_t>>& known) {
  if (std::find(p.pns.begin(), p.pns.end(), missing) == p.pns.end()) return std::nullopt;
  std::vector<uint8_t> body = p.body_xor;
  uint16_t length = p.length_xor;
  for (uint64_t pn : p.pns) {
    if (pn == missing) continue;
    auto it = known.find(pn);
    if (it == known.end()) return std::nullopt;
    const auto& b = it->second;
    if (b.size() > body.size()) return std::nullopt;  // not the body the parity was made from
    for (size_t i = 0; i < b.size(); ++i) body[i] ^= b[i];
    length ^= static_cast<uint16_t>(b.size());
  }
  if (length > body.size()) return std::nullopt;
  body.resize(length);
  return body;
}

}  // namespace rdacore::net::fec
