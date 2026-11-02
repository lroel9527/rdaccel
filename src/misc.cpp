#include "rdaccel/rdaccel.h"

#include "capture/capturer.h"
#include "capture/synthetic_capturer.h"
#include "io/stream_file.h"
#include "platform.h"

namespace rdaccel {

const char* Version() { return RDACCEL_VERSION_STRING; }

const char* PlatformName() { return platform::PlatformName(); }

uint64_t NowMicros() { return capture::MonotonicMicros(); }

std::vector<Rect> RenderTestPattern(uint64_t index, int32_t w, int32_t h,
                                    std::vector<uint8_t>& bgra) {
  return capture::RenderSyntheticFrame(index, w, h, bgra);
}

std::unique_ptr<Transport> CreateRecordingTransport(const std::string& path, std::string* error) {
  auto w = std::make_unique<io::StreamFileWriter>();
  if (!w->Open(path, error)) return nullptr;
  return w;
}

bool ReadRecording(const std::string& path, std::vector<uint8_t>& out, std::string* error) {
  return io::ReadStreamFile(path, out, error);
}

}  // namespace rdaccel
