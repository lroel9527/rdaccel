#pragma once

// The full-screen pictures, decoded on the host as the viewer decodes them.
//
// The host knows which rects the viewer takes from a picture, but not how
// close to the screen the encoder made them: a picture coded while most of
// the screen changed at once (an app switch, a window closing) at a modest
// bitrate came out a kilobyte or two for a hundred rects, plain-colour
// blocks beside sharp text. Decoding every picture as the viewer will (the
// same bitstream through the same kind of decoder: HEVC decoding is exact)
// tells it. Every picture of the stream has to be decoded, in order, as
// each refers to the ones before.

#include <cstdint>
#include <functional>
#include <memory>

#include "codec/video_codec.h"
#include "rdacore/image.h"

namespace rdacore::session {

class PictureMirror {
 public:
  explicit PictureMirror(std::unique_ptr<codec::IHevcDecoder> decoder) : decoder_(std::move(decoder)) {}

  // Whether it has decoded every picture since a keyframe, so that what it
  // decodes next is what the viewer shows.
  bool in_step() const { return in_step_; }
  // A picture was coded that it did not see (text mode was off): it is out
  // of step until the next keyframe.
  void Lose() { in_step_ = false; }

  // Decodes the next picture of the stream and calls `on_picture` with it
  // (BGRA, valid during the call). False if it is out of step or the decoder
  // failed (then it stays out of step until a keyframe).
  bool Decode(const codec::HevcAccessUnit& au, uint64_t pts_us, const std::function<void(const ImageView&)>& on_picture);

 private:
  std::unique_ptr<codec::IHevcDecoder> decoder_;
  bool in_step_ = false;
};

}  // namespace rdacore::session
