#include "rdacore/rdacore.h"

#include <algorithm>

#include "capture/capturer.h"
#include "capture/synthetic_capturer.h"
#include "io/stream_file.h"
#include "platform.h"

namespace rdacore {

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

}  // namespace rdacore

namespace rdacore {

void DrawCursor(const CursorImage& c, int32_t x, int32_t y, uint8_t* bgra, int32_t width,
                int32_t height, int32_t stride) {
  if (!bgra || c.bgra.size() != static_cast<size_t>(c.width) * c.height * 4) return;
  const int32_t left = x - c.hotspot_x, top = y - c.hotspot_y;
  for (int32_t row = std::max(0, -top); row < c.height && top + row < height; ++row) {
    const uint8_t* s = &c.bgra[static_cast<size_t>(row) * c.width * 4];
    uint8_t* d = bgra + static_cast<size_t>(top + row) * stride;
    for (int32_t col = std::max(0, -left); col < c.width && left + col < width; ++col) {
      const uint8_t* sp = s + static_cast<size_t>(col) * 4;
      uint8_t* dp = d + static_cast<size_t>(left + col) * 4;
      const int inv = 255 - sp[3];
      if (inv == 255) continue;
      // Premultiplied "over"; the destination stays opaque.
      for (int k = 0; k < 3; ++k) dp[k] = static_cast<uint8_t>(std::min(255, sp[k] + (dp[k] * inv + 127) / 255));
      dp[3] = 255;
    }
  }
}

}  // namespace rdacore
