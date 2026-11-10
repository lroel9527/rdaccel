#pragma once

// Platform-neutral HEVC encoder/decoder interfaces. Implementations:
//   * macOS: VideoToolbox (platform/mac/video_*_vt.mm)
//   * Linux: FFmpeg, hardware first (NVENC), x265 otherwise
//     (platform/linux/video_ffmpeg.cpp)

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "codec/hevc_bitstream.h"
#include "tile/tile_diff.h"

namespace rdacore::codec {

struct EncoderConfig {
  int32_t width = 0;
  int32_t height = 0;
  int32_t fps = 60;
  int64_t bitrate_bps = 8'000'000;
  // Lowest QP (finest quantizer) the encoder may use; 0 = encoder default.
  // Without a floor, low-latency rate control keeps spending its budget
  // sharpening static content far beyond what anyone can see.
  int32_t min_qp = 0;
};

class IHevcEncoder {
 public:
  virtual ~IHevcEncoder() = default;

  // Human-readable backend name, e.g. "VideoToolbox HEVC" or "libx265".
  virtual std::string name() const = 0;

  // Encodes one BGRA frame. Low-latency encoders return the picture right
  // away; nullopt means the encoder produced nothing for this input (e.g. it
  // dropped the frame) or failed, in which case last_error() is set.
  // last_error() describes the most recent Encode() call only: empty after a
  // successful call or a deliberate drop.
  virtual std::optional<HevcAccessUnit> Encode(const tile::ImageView& bgra, uint64_t pts_us,
                                               bool force_keyframe) = 0;

  // Changes the target bitrate without restarting the stream.
  virtual void SetBitrate(int64_t bps) = 0;

  // Lowers the QP floor (the quality ceiling) to `qp` for the pictures that
  // follow, or with 0 puts back the one the encoder was created with. For
  // pictures of a still screen, which may be coded finer than moving ones.
  // Never raises the floor. Encoders that cannot change it ignore the call.
  virtual void SetMinQp(int /*qp*/) {}

  virtual const std::string& last_error() const = 0;
};

// A decoded picture, always converted to BGRA so the rest of the pipeline
// does not care which decoder produced it. The view is valid only during the
// callback.
using DecodedFrameFn = std::function<void(const tile::ImageView& bgra, uint64_t pts_us)>;

class IHevcDecoder {
 public:
  virtual ~IHevcDecoder() = default;
  virtual std::string name() const = 0;

  // Decodes one access unit in wire layout. Calls `on_frame` zero or one
  // times. Returns false on a decode error (last_error() explains).
  virtual bool Decode(const std::vector<std::vector<uint8_t>>& parameter_sets,
                      const std::vector<uint8_t>& nal_data, uint64_t pts_us,
                      const DecodedFrameFn& on_frame) = 0;

  virtual const std::string& last_error() const = 0;
};

}  // namespace rdacore::codec
