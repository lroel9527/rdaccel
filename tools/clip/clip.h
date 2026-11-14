#pragma once

// Clip files: a screen as a capture source delivers it, losslessly, so the
// same screen content can be sent through the host again and again (and
// through two versions of it). Only the frames in which the screen changed
// are stored, each deflated as its XOR with the one before.
//
// Per frame, little-endian:
//   u64 capture time (microseconds)   i32 width   i32 height
//   u32 number of dirty rects, then that many Rects (x, y, w, h as i32)
//   u32 length, then that many bytes: deflate of (frame XOR previous frame)
// A frame of a new size is XORed with zeros.
//
// Clips of a real desktop show whatever was on it: keep them on the machine
// that recorded them (see docs/clips.md). *.clip is in .gitignore.

#include <zlib.h>

#include <cstdint>
#include <cstdio>
#include <vector>

#include "rdacore/geometry.h"

namespace rdaccel::clip {

class ClipWriter {
 public:
  explicit ClipWriter(const char* path) : f_(std::fopen(path, "wb")) {}
  ~ClipWriter() {
    if (f_) std::fclose(f_);
  }
  ClipWriter(const ClipWriter&) = delete;
  ClipWriter& operator=(const ClipWriter&) = delete;
  bool ok() const { return f_ != nullptr; }

  // Returns the compressed size of the frame.
  size_t Write(uint64_t time_us, const uint8_t* bgra, int32_t w, int32_t h, int32_t stride,
               const std::vector<rdacore::Rect>& dirty) {
    const size_t n = static_cast<size_t>(w) * h * 4;
    if (prev_.size() != n) prev_.assign(n, 0);
    x_.resize(n);
    for (int32_t y = 0; y < h; ++y) {
      const uint8_t* s = bgra + static_cast<size_t>(y) * stride;
      uint8_t* p = prev_.data() + static_cast<size_t>(y) * w * 4;
      uint8_t* d = x_.data() + static_cast<size_t>(y) * w * 4;
      for (int32_t i = 0; i < w * 4; ++i) {
        d[i] = s[i] ^ p[i];
        p[i] = s[i];
      }
    }
    uLongf len = compressBound(static_cast<uLong>(n));
    c_.resize(len);
    compress2(c_.data(), &len, x_.data(), static_cast<uLong>(n), 1);
    const uint32_t nd = static_cast<uint32_t>(dirty.size()), cl = static_cast<uint32_t>(len);
    std::fwrite(&time_us, 8, 1, f_);
    std::fwrite(&w, 4, 1, f_);
    std::fwrite(&h, 4, 1, f_);
    std::fwrite(&nd, 4, 1, f_);
    for (const auto& r : dirty) std::fwrite(&r, sizeof r, 1, f_);
    std::fwrite(&cl, 4, 1, f_);
    std::fwrite(c_.data(), 1, len, f_);
    return len;
  }

 private:
  std::FILE* f_;
  std::vector<uint8_t> prev_, x_, c_;
};

class ClipReader {
 public:
  explicit ClipReader(const char* path) : f_(std::fopen(path, "rb")) {}
  ~ClipReader() {
    if (f_) std::fclose(f_);
  }
  ClipReader(const ClipReader&) = delete;
  ClipReader& operator=(const ClipReader&) = delete;
  bool ok() const { return f_ != nullptr; }

  // The next frame into frame(); false at the end (or on a damaged file).
  bool Next() {
    uint32_t nd = 0, cl = 0;
    int32_t nw = 0, nh = 0;
    if (!f_ || std::fread(&time_us_, 8, 1, f_) != 1) return false;
    if (std::fread(&nw, 4, 1, f_) != 1 || std::fread(&nh, 4, 1, f_) != 1 || std::fread(&nd, 4, 1, f_) != 1) return false;
    if (nw <= 0 || nh <= 0 || nw > 16384 || nh > 16384 || nd > 1u << 20) return false;
    dirty_.resize(nd);
    if (nd && std::fread(dirty_.data(), sizeof(rdacore::Rect), nd, f_) != nd) return false;
    if (std::fread(&cl, 4, 1, f_) != 1) return false;
    c_.resize(cl);
    if (cl && std::fread(c_.data(), 1, cl, f_) != cl) return false;
    const size_t n = static_cast<size_t>(nw) * nh * 4;
    if (nw != w_ || nh != h_) {
      w_ = nw;
      h_ = nh;
      cur_.assign(n, 0);
    }
    x_.resize(n);
    uLongf len = static_cast<uLongf>(n);
    if (uncompress(x_.data(), &len, c_.data(), cl) != Z_OK || len != n) return false;
    for (size_t i = 0; i < n; ++i) cur_[i] ^= x_[i];
    return true;
  }

  const std::vector<uint8_t>& frame() const { return cur_; }  // BGRA, rows of width * 4 bytes
  int32_t width() const { return w_; }
  int32_t height() const { return h_; }
  uint64_t time_us() const { return time_us_; }
  const std::vector<rdacore::Rect>& dirty() const { return dirty_; }

 private:
  std::FILE* f_;
  std::vector<uint8_t> cur_, x_, c_;
  std::vector<rdacore::Rect> dirty_;
  int32_t w_ = 0, h_ = 0;
  uint64_t time_us_ = 0;
};

}  // namespace rdaccel::clip
