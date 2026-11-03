#pragma once

// HEVC bitstream helpers shared by all platforms.
//
// Two layouts exist for the same NAL units:
//   * Annex B: NAL units separated by 00 00 01 / 00 00 00 01 start codes
//     (what FFmpeg/x265/NVENC produce and consume).
//   * Length-prefixed ("HVCC"): every NAL preceded by its size as a 4-byte
//     big-endian integer (what VideoToolbox produces and consumes).
// The wire protocol always carries the length-prefixed layout, with parameter
// sets (VPS/SPS/PPS) split out, so both ends see the same format.

#include <cstdint>
#include <span>
#include <vector>

namespace rdacore::codec {

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

// Splits an Annex B byte stream into NAL units (start codes removed).
std::vector<std::span<const uint8_t>> SplitAnnexB(std::span<const uint8_t> stream);

// Splits a 4-byte length-prefixed buffer into NAL units. Returns false if a
// length runs past the end of the buffer.
bool SplitLengthPrefixed(std::span<const uint8_t> data,
                         std::vector<std::span<const uint8_t>>& out);

void AppendLengthPrefixed(std::span<const uint8_t> nal, std::vector<uint8_t>& out);
void AppendAnnexB(std::span<const uint8_t> nal, std::vector<uint8_t>& out);

// One encoded picture in the wire layout.
struct HevcAccessUnit {
  bool keyframe = false;
  std::vector<std::vector<uint8_t>> parameter_sets;  // VPS, SPS, PPS (keyframes)
  std::vector<uint8_t> nal_data;                     // length-prefixed slices + SEI
};

// Converts one Annex B access unit (encoder output) to the wire layout.
// Parameter sets are moved into `parameter_sets`; access unit delimiters are
// dropped; everything else is kept in order.
HevcAccessUnit AnnexBToAccessUnit(std::span<const uint8_t> annexb);

// Converts the wire layout back to Annex B for decoders that want it.
// Returns false if `nal_data` is malformed.
bool AccessUnitToAnnexB(const std::vector<std::vector<uint8_t>>& parameter_sets,
                        std::span<const uint8_t> nal_data, std::vector<uint8_t>& out);

}  // namespace rdacore::codec
