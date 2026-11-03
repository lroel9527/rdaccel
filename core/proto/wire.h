#pragma once

// Little-endian binary encoding helpers used by the wire protocol.
// Unsigned integers use LEB128 varints; signed ones are zigzag-encoded first.

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace rdacore::proto {

class ByteWriter {
 public:
  void u8(uint8_t v) { buf_.push_back(v); }
  void u32le(uint32_t v);
  void u64le(uint64_t v);
  void varint(uint64_t v);
  void svarint(int64_t v);
  void bytes(std::span<const uint8_t> data);
  // Length-prefixed (varint) byte string.
  void blob(std::span<const uint8_t> data);

  const std::vector<uint8_t>& data() const { return buf_; }
  std::vector<uint8_t> take() { return std::move(buf_); }
  size_t size() const { return buf_.size(); }

 private:
  std::vector<uint8_t> buf_;
};

// Reads from a borrowed buffer. Every read returns false on truncated or
// malformed input and leaves the reader in a failed state; callers can do a
// chain of reads and check ok() once at the end.
class ByteReader {
 public:
  explicit ByteReader(std::span<const uint8_t> data) : data_(data) {}

  bool u8(uint8_t& v);
  bool u32le(uint32_t& v);
  bool u64le(uint64_t& v);
  bool varint(uint64_t& v);
  bool svarint(int64_t& v);
  // Reads exactly n bytes as a view into the underlying buffer.
  bool bytes(size_t n, std::span<const uint8_t>& out);
  bool blob(std::span<const uint8_t>& out);

  // Convenience: varint that must fit in 32 bits.
  bool varint32(uint32_t& v);
  bool svarint32(int32_t& v);

  bool ok() const { return ok_; }
  size_t remaining() const { return data_.size() - pos_; }
  bool at_end() const { return pos_ == data_.size(); }

 private:
  bool fail() {
    ok_ = false;
    return false;
  }

  std::span<const uint8_t> data_;
  size_t pos_ = 0;
  bool ok_ = true;
};

}  // namespace rdacore::proto
