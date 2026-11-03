#include "log/log.h"

#include <atomic>
#include <mutex>
#include <shared_mutex>

namespace rdacore::log {
namespace {

struct Sink {
  std::shared_mutex mu;  // writers: SetSink; readers: Write
  SinkFn fn = nullptr;
  void* user = nullptr;
  std::atomic<int> max_level{-1};  // -1 = no sink
};

Sink& TheSink() {
  static Sink s;  // never destroyed before static users finish (function-local)
  return s;
}

}  // namespace

void SetSink(SinkFn fn, void* user, Level max_level) {
  Sink& s = TheSink();
  std::unique_lock lock(s.mu);
  s.fn = fn;
  s.user = user;
  s.max_level = fn ? static_cast<int>(max_level) : -1;
}

bool Enabled(Level level) { return static_cast<int>(level) <= TheSink().max_level.load(std::memory_order_relaxed); }

void Write(Level level, const std::string& message) {
  if (!Enabled(level)) return;
  Sink& s = TheSink();
  std::shared_lock lock(s.mu);
  if (s.fn && static_cast<int>(level) <= s.max_level) s.fn(s.user, static_cast<int>(level), message.c_str());
}

}  // namespace rdacore::log
