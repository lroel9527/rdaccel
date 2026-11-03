#pragma once

// Library-internal logging. Nothing is printed; messages go to the sink the
// application installs with rda_set_log() (or nowhere).

#include <string>

namespace rdacore::log {

enum class Level { kError = 0, kWarn = 1, kInfo = 2, kDebug = 3 };

using SinkFn = void (*)(void* user, int level, const char* message);

void SetSink(SinkFn fn, void* user, Level max_level);

// Cheap when no sink is installed or the level is filtered out.
bool Enabled(Level level);
void Write(Level level, const std::string& message);

inline void Error(const std::string& m) { Write(Level::kError, m); }
inline void Warn(const std::string& m) { Write(Level::kWarn, m); }
inline void Info(const std::string& m) { Write(Level::kInfo, m); }
inline void Debug(const std::string& m) { if (Enabled(Level::kDebug)) Write(Level::kDebug, m); }

}  // namespace rdacore::log
