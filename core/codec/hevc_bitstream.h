#pragma once

// HEVC and H.264 bitstream helpers shared by all platforms.
//
// HEVC is the stream's codec. H.264 is the fallback for hosts whose HEVC
// encoder is missing or too slow at the screen's size (an Intel Mac with
// hardware H.264 at 52 fps and HEVC at 15), for viewers that decode it
// (proto::kCapH264). Both use the same layouts below; only the NAL header
// differs (HEVC: type = (nal[0] >> 1) & 0x3f; H.264: type = nal[0] & 0x1f).
//
// Two layouts exist for the same NAL units:
//   * Annex B: NAL units separated by 00 00 01 / 00 00 00 01 start codes
//     (what FFmpeg/x265/NVENC produce and consume).
//   * Length-prefixed ("HVCC"): every NAL preceded by its size as a 4-byte
//     big-endian integer (what VideoToolbox produces and consumes).
// The wire protocol always carries the length-prefixed layout, with parameter
// sets (HEVC VPS/SPS/PPS, H.264 SPS/PPS) split out, so both ends see the same
// format.

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace rdacore::codec {

// The video codec of a stream. The values are those of proto::Hello::codec.
enum class VideoCodec : uint8_t { kH264 = 1, kHevc = 2 };

inline const char* VideoCodecName(VideoCodec c) { return c == VideoCodec::kH264 ? "H.264" : "HEVC"; }

enum HevcNalType : uint8_t {
  kNalIdrWRadl = 19,
  kNalIdrNLp = 20,
  kNalCra = 21,
  kNalVps = 32,
  kNalSps = 33,
  kNalPps = 34,
  kNalAud = 35,
  kNalPrefixSei = 39,
  kNalSuffixSei = 40,
};

inline uint8_t HevcNalTypeOf(std::span<const uint8_t> nal) {
  return nal.empty() ? 0xff : static_cast<uint8_t>((nal[0] >> 1) & 0x3f);
}

inline bool IsHevcIrap(uint8_t type) { return type >= 16 && type <= 23; }

inline bool IsHevcParameterSet(uint8_t type) {
  return type == kNalVps || type == kNalSps || type == kNalPps;
}

enum H264NalType : uint8_t {
  kH264NalSlice = 1,
  kH264NalIdr = 5,
  kH264NalSei = 6,
  kH264NalSps = 7,
  kH264NalPps = 8,
  kH264NalAud = 9,
};

inline uint8_t H264NalTypeOf(std::span<const uint8_t> nal) {
  return nal.empty() ? 0xff : static_cast<uint8_t>(nal[0] & 0x1f);
}

// The same questions for either codec.
inline uint8_t NalTypeOf(VideoCodec c, std::span<const uint8_t> nal) {
  return c == VideoCodec::kH264 ? H264NalTypeOf(nal) : HevcNalTypeOf(nal);
}
inline bool IsParameterSetNal(VideoCodec c, uint8_t type) {
  return c == VideoCodec::kH264 ? type == kH264NalSps || type == kH264NalPps : IsHevcParameterSet(type);
}
// A picture the decoder can start from (HEVC IRAP, H.264 IDR).
inline bool IsKeyframeNal(VideoCodec c, uint8_t type) {
  return c == VideoCodec::kH264 ? type == kH264NalIdr : IsHevcIrap(type);
}
inline bool IsAudNal(VideoCodec c, uint8_t type) {
  return c == VideoCodec::kH264 ? type == kH264NalAud : type == kNalAud;
}

// Which codec a keyframe's parameter sets belong to: HEVC when one of them is
// a VPS (first byte 0x40/0x41, which as H.264 would be an unspecified type
// 0), H.264 when there is none and one is an H.264 SPS. nullopt when neither
// (no parameter sets, or unknown data). Lets a viewer follow the stream even
// if a Hello naming the codec went missing or came late.
std::optional<VideoCodec> DetectCodec(const std::vector<std::vector<uint8_t>>& parameter_sets);

// Splits an Annex B byte stream into NAL units (start codes removed).
std::vector<std::span<const uint8_t>> SplitAnnexB(std::span<const uint8_t> stream);

// Splits a 4-byte length-prefixed buffer into NAL units. Returns false if a
// length runs past the end of the buffer.
bool SplitLengthPrefixed(std::span<const uint8_t> data,
                         std::vector<std::span<const uint8_t>>& out);

void AppendLengthPrefixed(std::span<const uint8_t> nal, std::vector<uint8_t>& out);
void AppendAnnexB(std::span<const uint8_t> nal, std::vector<uint8_t>& out);

// One encoded picture in the wire layout (either codec).
struct HevcAccessUnit {
  bool keyframe = false;
  std::vector<std::vector<uint8_t>> parameter_sets;  // (VPS,) SPS, PPS (keyframes)
  std::vector<uint8_t> nal_data;                     // length-prefixed slices + SEI
};

// Converts one Annex B access unit (encoder output) to the wire layout.
// Parameter sets are moved into `parameter_sets`; access unit delimiters are
// dropped; everything else is kept in order.
HevcAccessUnit AnnexBToAccessUnit(std::span<const uint8_t> annexb,
                                  VideoCodec codec = VideoCodec::kHevc);

// Converts the wire layout back to Annex B for decoders that want it.
// Returns false if `nal_data` is malformed.
bool AccessUnitToAnnexB(const std::vector<std::vector<uint8_t>>& parameter_sets,
                        std::span<const uint8_t> nal_data, std::vector<uint8_t>& out);

}  // namespace rdacore::codec
