#pragma once

// Plays viewer input into the host OS: XTest on Linux, CGEvent on macOS.

#include <cstdint>
#include <string>

#include "rdacore/input.h"

namespace rdacore::platform {

struct InjectorConfig {
  int32_t display_index = 0;
  // The size of the picture the viewer sees, which event coordinates refer
  // to (the capture size). 0 = the display's native size.
  int32_t source_width = 0;
  int32_t source_height = 0;
};

class IInputInjector {
 public:
  virtual ~IInputInjector() = default;
  // False with `error` for an event the OS cannot take (a key it does not
  // have, a character no keyboard mapping can make, ...). Thread-safe.
  virtual bool Inject(const InputEvent& ev, std::string* error) = 0;
};

}  // namespace rdacore::platform
