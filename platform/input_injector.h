#pragma once

// Plays viewer input into the host OS (CGEvent).

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
  // Selects one of the enabled keyboard input sources (InputSourceInfo::id)
  // for the keys that follow. Thread-safe.
  virtual bool SelectInputSource(const std::string& id, std::string* error) {
    (void)id;
    if (error) *error = "selecting input sources is not supported on this platform";
    return false;
  }
  // Whether kText first steps away from an input method to an ASCII-capable
  // layout (macOS; true by default). Turned off, text goes to whatever source
  // is current, for one that passes keys through. Thread-safe.
  virtual void SetTextLeavesInputMethod(bool leaves) { (void)leaves; }
  // An input method that passes keys through (an application's own): while
  // it is the current source, kText goes into it as is; other input methods
  // are still stepped away from. Empty = none. Thread-safe. (3.4)
  virtual void SetTextPassthroughSource(const std::string& id) { (void)id; }
};

}  // namespace rdacore::platform
