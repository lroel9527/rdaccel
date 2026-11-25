#include "session/picture_mirror.h"

namespace rdacore::session {

bool PictureMirror::Decode(const codec::HevcAccessUnit& au, uint64_t pts_us,
                           const std::function<void(const ImageView&)>& on_picture) {
  if (au.keyframe) in_step_ = true;
  if (!in_step_ || !decoder_) return false;
  bool got = false;
  const bool ok = decoder_->Decode(au.parameter_sets, au.nal_data, pts_us, [&](const ImageView& picture, uint64_t) {
    got = true;
    on_picture(picture);
  });
  // A low-latency stream has no reordering: each picture comes out at once.
  // One that does not (or fails) leaves the mirror behind the viewer.
  if (!ok || !got) in_step_ = false;
  return ok && got;
}

}  // namespace rdacore::session
