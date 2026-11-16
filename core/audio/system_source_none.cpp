// No system sound capture on this platform (Linux so far).

#include "audio/audio_source.h"

namespace rdacore::audio {

std::unique_ptr<Source> CreateSystemSource() { return nullptr; }

}  // namespace rdacore::audio
