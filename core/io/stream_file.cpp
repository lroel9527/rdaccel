#include "io/stream_file.h"

#include <cstring>

namespace rdaccel::io {

bool StreamFileWriter::Open(const std::string& path, std::string* error) {
  Close();
  f_ = std::fopen(path.c_str(), "wb");
  if (!f_) {
    if (error) *error = "cannot create " + path + ": " + std::strerror(errno);
    return false;
  }
  std::fwrite(kStreamMagic, 1, sizeof(kStreamMagic), f_);
  return true;
}

bool StreamFileWriter::Send(transport::Channel ch, std::span<const uint8_t> bytes) {
  if (!f_) return false;
  // Only screen data belongs in the file; input and feedback are live-only.
  if (ch != transport::Channel::kVideo && ch != transport::Channel::kControl) return true;
  if (std::fwrite(bytes.data(), 1, bytes.size(), f_) != bytes.size()) return false;
  bytes_ += bytes.size();
  return true;
}

void StreamFileWriter::Close() {
  if (f_) {
    std::fclose(f_);
    f_ = nullptr;
  }
}

bool ReadStreamFile(const std::string& path, std::vector<uint8_t>& out, std::string* error) {
  std::FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) {
    if (error) *error = "cannot open " + path + ": " + std::strerror(errno);
    return false;
  }
  char magic[sizeof(kStreamMagic)] = {};
  const bool magic_ok = std::fread(magic, 1, sizeof(magic), f) == sizeof(magic) &&
                        std::memcmp(magic, kStreamMagic, sizeof(magic)) == 0;
  if (!magic_ok) {
    std::fclose(f);
    if (error) *error = path + " is not an .rdstream file";
    return false;
  }
  out.clear();
  uint8_t buf[1 << 16];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.insert(out.end(), buf, buf + n);
  std::fclose(f);
  return true;
}

}  // namespace rdaccel::io
