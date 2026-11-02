#include "proto/wire.h"

#include <limits>

namespace rdaccel::proto {

void ByteWriter::u32le(uint32_t v) {
  for (int i = 0; i < 4; ++i) buf_.push_back(static_cast<uint8_t>(v >> (8 * i)));
}

void ByteWriter::u64le(uint64_t v) {
  for (int i = 0; i < 8; ++i) buf_.push_back(static_cast<uint8_t>(v >> (8 * i)));
}

void ByteWriter::varint(uint64_t v) {
  while (v >= 0x80) {
    buf_.push_back(static_cast<uint8_t>(v | 0x80));
    v >>= 7;
  }
  buf_.push_back(static_cast<uint8_t>(v));
}

void ByteWriter::svarint(int64_t v) {
  // Zigzag: 0 -> 0, -1 -> 1, 1 -> 2, -2 -> 3, ...
  const uint64_t zz = (static_cast<uint64_t>(v) << 1) ^ static_cast<uint64_t>(v >> 63);
  varint(zz);
}

void ByteWriter::bytes(std::span<const uint8_t> data) {
  buf_.insert(buf_.end(), data.begin(), data.end());
}

void ByteWriter::blob(std::span<const uint8_t> data) {
  varint(data.size());
  bytes(data);
}

bool ByteReader::u8(uint8_t& v) {
  if (!ok_ || remaining() < 1) return fail();
  v = data_[pos_++];
  return true;
}

bool ByteReader::u32le(uint32_t& v) {
  if (!ok_ || remaining() < 4) return fail();
  v = 0;
  for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(data_[pos_ + i]) << (8 * i);
  pos_ += 4;
  return true;
}

bool ByteReader::u64le(uint64_t& v) {
  if (!ok_ || remaining() < 8) return fail();
  v = 0;
  for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(data_[pos_ + i]) << (8 * i);
  pos_ += 8;
  return true;
}

bool ByteReader::varint(uint64_t& v) {
  if (!ok_) return false;
  v = 0;
  for (int shift = 0; shift < 64; shift += 7) {
    if (remaining() < 1) return fail();
    const uint8_t b = data_[pos_++];
    // The 10th byte may only carry the top bit of a 64-bit value.
    if (shift == 63 && b > 1) return fail();
    v |= static_cast<uint64_t>(b & 0x7f) << shift;
    if ((b & 0x80) == 0) return true;
  }
  return fail();
}

bool ByteReader::svarint(int64_t& v) {
  uint64_t zz = 0;
  if (!varint(zz)) return false;
  v = static_cast<int64_t>(zz >> 1) ^ -static_cast<int64_t>(zz & 1);
  return true;
}

bool ByteReader::bytes(size_t n, std::span<const uint8_t>& out) {
  if (!ok_ || remaining() < n) return fail();
  out = data_.subspan(pos_, n);
  pos_ += n;
  return true;
}

bool ByteReader::blob(std::span<const uint8_t>& out) {
  uint64_t n = 0;
  if (!varint(n)) return false;
  if (n > remaining()) return fail();
  return bytes(static_cast<size_t>(n), out);
}

bool ByteReader::varint32(uint32_t& v) {
  uint64_t x = 0;
  if (!varint(x)) return false;
  if (x > std::numeric_limits<uint32_t>::max()) return fail();
  v = static_cast<uint32_t>(x);
  return true;
}

bool ByteReader::svarint32(int32_t& v) {
  int64_t x = 0;
  if (!svarint(x)) return false;
  if (x < std::numeric_limits<int32_t>::min() || x > std::numeric_limits<int32_t>::max()) {
    return fail();
  }
  v = static_cast<int32_t>(x);
  return true;
}

}  // namespace rdaccel::proto
