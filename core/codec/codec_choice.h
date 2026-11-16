#pragma once

// Which video codec a host sends: HEVC whenever it can be used; H.264 only
// when the viewer decodes it (proto::kCapH264) and HEVC is not usable at the
// screen's size: no hardware HEVC encoder, or one that is too slow. An Intel
// MacBook Pro 13" 2016 (Iris 550, macOS 12) encodes 2880 x 1800 with
// hardware HEVC at about 15 fps and with hardware H.264 at about 52.
//
// Platform-neutral, so the decision can be tested with fake measurements;
// the host plugs in the platform's encoders (src/host.cpp).

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

#include "codec/video_codec.h"

namespace rdacore::codec {

// HostConfig::video_codec.
enum class CodecPreference : uint8_t { kAuto = 0, kHevc = 1, kH264 = 2 };

// Hardware HEVC below this many frames per second at the screen's size is
// not usable (if hardware H.264 is faster).
inline constexpr double kMinUsableFps = 30.0;

struct CodecChoice {
  VideoCodec codec = VideoCodec::kHevc;
  bool measured = false;  // hardware encoders were measured (auto only)
  double hevc_fps = 0;    // as measured; 0 = no hardware HEVC encoder (or not measured)
  double h264_fps = 0;    // 0 = no hardware H.264 encoder (or not measured)
  // Worth a warning: H.264 instead of HEVC, or a preference that could not
  // be followed.
  bool warn = false;
  std::string reason;     // for the log
};

// Frames per second a hardware encoder of `codec` reaches at w x h, or 0 when
// there is none (or it fails).
using HardwareFpsFn = std::function<double(VideoCodec codec, int32_t w, int32_t h)>;

// The decision. `measure` is only called for kAuto with a viewer that
// decodes H.264: HEVC first; H.264 only when HEVC is missing or below
// kMinUsableFps. H.264 wins only when it is faster than HEVC.
CodecChoice ChooseVideoCodec(CodecPreference pref, bool viewer_decodes_h264, int32_t w, int32_t h,
                             const HardwareFpsFn& measure);

// Encodes a short burst of changing w x h pictures (one keyframe to warm up,
// then up to `frames` timed ones) and returns the frames per second. Gives up
// early, returning what it has measured, once the burst can no longer reach
// `stop_below_fps` (so a slow encoder costs at most frames / stop_below_fps
// seconds). 0 if the encoder fails.
double MeasureEncodeFps(IHevcEncoder& enc, int32_t w, int32_t h, int frames = 8,
                        double stop_below_fps = kMinUsableFps);

// Hardware encoding speed measured in this process, per codec and size
// (0: no hardware encoder). What the machine can do does not change between
// sessions, and an application may make a host per session (rdmachine
// does): measured once per process, not once per host. Thread-safe.
std::optional<double> CachedHardwareFps(VideoCodec c, int32_t w, int32_t h);
void CacheHardwareFps(VideoCodec c, int32_t w, int32_t h, double fps);
void ClearHardwareFpsCacheForTests();

}  // namespace rdacore::codec
