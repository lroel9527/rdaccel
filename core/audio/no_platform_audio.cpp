// Where the platform has no sound capture or output device of its own
// (Linux so far): the codecs, the transport and the null player still work.

#include "audio/audio_source.h"
#include "audio/player.h"

namespace rdacore::audio {

std::unique_ptr<Source> CreateSystemSource() { return nullptr; }

std::unique_ptr<Player> CreateDevicePlayer(RenderFn, std::string* error) {
  if (error) *error = "this platform has no sound output yet (use the null device)";
  return nullptr;
}

uint64_t DevicePlayersOpened() { return 0; }

}  // namespace rdacore::audio
