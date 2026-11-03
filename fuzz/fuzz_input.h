#pragma once

// Splits a fuzzer input into (channel, chunk) pieces: each piece is
// [channel byte][length byte][that many bytes]. Arbitrary splitting is the
// point: transports may cut and merge the byte streams anywhere.

#include <cstddef>
#include <cstdint>
#include <span>

namespace rdacore::fuzz {

class Pieces {
 public:
  Pieces(const uint8_t* data, size_t size) : p_(data), end_(data + size) {}
  // False when the input is used up.
  bool Next(uint8_t& channel, std::span<const uint8_t>& chunk) {
    if (end_ - p_ < 2) return false;
    channel = p_[0];
    size_t n = p_[1];
    p_ += 2;
    n = std::min<size_t>(n, static_cast<size_t>(end_ - p_));
    chunk = {p_, n};
    p_ += n;
    return true;
  }

 private:
  const uint8_t* p_;
  const uint8_t* end_;
};

}  // namespace rdacore::fuzz
