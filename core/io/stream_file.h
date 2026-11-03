#pragma once

// .rdstream files: the exact bytes the host sends on the video and control
// channels (and the cursor channel), behind an 8-byte magic. A file recorded on one platform can be
// played back on another, which is how Mac <-> Linux interop is tested
// without a live connection.

#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#include "transport/transport.h"

namespace rdaccel::io {

inline constexpr char kStreamMagic[8] = {'R', 'D', 'S', 'T', 'R', 'M', '0', '1'};

// A transport that writes everything it is asked to send into a file and
// never receives anything. Plugs into HostSession unchanged.
class StreamFileWriter final : public transport::ITransport {
 public:
  // Returns false and sets `error` if the file cannot be created.
  bool Open(const std::string& path, std::string* error);
  ~StreamFileWriter() override { Close(); }

  bool Send(transport::Channel ch, std::span<const uint8_t> bytes) override;
  void SetReceiver(ReceiveFn) override {}
  void Close() override;

  uint64_t bytes_written() const {
    std::lock_guard<std::mutex> lock(mu_);
    return bytes_;
  }

 private:
  // The cursor is sent from another thread than the screen.
  mutable std::mutex mu_;
  std::FILE* f_ = nullptr;
  uint64_t bytes_ = 0;
};

// Reads a whole .rdstream file. Returns false with `error` set if the file is
// missing or has the wrong magic.
bool ReadStreamFile(const std::string& path, std::vector<uint8_t>& out, std::string* error);

}  // namespace rdaccel::io
