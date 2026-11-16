#include "audio/player.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "capture/capturer.h"

namespace rdacore::audio {
namespace {

class NullPlayer final : public Player {
 public:
  explicit NullPlayer(RenderFn render) : render_(std::move(render)) {
    thread_ = std::thread([this] {
      std::vector<float> buf(240 * 2);
      auto next = std::chrono::steady_clock::now();
      while (!stop_) {
        render_(buf.data(), 240, capture::MonotonicMicros());
        frames_ += 240;
        next += std::chrono::microseconds(5'000);
        std::this_thread::sleep_until(next);
      }
    });
  }
  ~NullPlayer() override {
    stop_ = true;
    thread_.join();
  }
  uint64_t frames() const override { return frames_; }

 private:
  RenderFn render_;
  std::atomic<bool> stop_{false};
  std::atomic<uint64_t> frames_{0};
  std::thread thread_;
};

}  // namespace

bool NullOutputForced() {
  const char* v = std::getenv("RDACCEL_AUDIO_NULL_OUTPUT");
  return v && std::strcmp(v, "0") != 0 && v[0] != '\0';
}

std::unique_ptr<Player> CreateNullPlayer(RenderFn render) { return std::make_unique<NullPlayer>(std::move(render)); }

}  // namespace rdacore::audio
