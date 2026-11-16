#pragma once

// A capturer that uses the preferred (newer) capture API and falls back to an
// older one only when the newer one does not work here: Start() fails, or
// no picture arrives within a while. The newer one is tried again on every
// Start(); nothing about a failure is remembered.
//
// macOS 12 and 13 use it for ScreenCaptureKit with CGDisplayStream behind
// it: on some Intel Macs with macOS 12 ScreenCaptureKit starts but never
// delivers a frame, while CGDisplayStream works (platform/mac/platform_mac.mm).

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include "capture/capturer.h"
#include "log/log.h"

namespace rdacore::capture {

class FallbackCapturer final : public ICapturer {
 public:
  using Factory = std::function<std::unique_ptr<ICapturer>()>;

  // `first_frame_wait`: how long the preferred capturer has to deliver its
  // first picture after a successful Start().
  FallbackCapturer(Factory preferred, std::string preferred_name, Factory fallback, std::string fallback_name,
                   std::chrono::milliseconds first_frame_wait)
      : preferred_(std::move(preferred)),
        fallback_(std::move(fallback)),
        preferred_name_(std::move(preferred_name)),
        fallback_name_(std::move(fallback_name)),
        wait_(first_frame_wait) {}

  ~FallbackCapturer() override { Stop(); }

  bool Start(const CaptureConfig& cfg, FrameFn on_frame) override {
    if (active_) {
      error_ = "already running";
      return false;
    }
    std::string why;
    {
      auto p = preferred_();
      p->SetStoppedHandler(on_stopped_);
      auto state = std::make_shared<State>();
      // Frames go to the caller from the start; the first one also says
      // that the preferred capturer works.
      FrameFn counted = [state, on_frame](const CapturedFrame& f) {
        if (!state->got.exchange(true)) {
          std::lock_guard<std::mutex> l(state->mu);
          state->cv.notify_all();
        }
        on_frame(f);
      };
      if (!p->Start(cfg, counted)) {
        why = preferred_name_ + " did not start: " + p->last_error();
      } else {
        std::unique_lock<std::mutex> l(state->mu);
        if (state->cv.wait_for(l, wait_, [&] { return state->got.load(); })) {
          active_ = std::move(p);
          using_fallback_ = false;
          return true;
        }
        l.unlock();
        p->Stop();
        why = preferred_name_ + " gave no picture in " + std::to_string(wait_.count()) + " ms";
      }
    }
    auto f = fallback_();
    f->SetStoppedHandler(on_stopped_);
    if (!f->Start(cfg, std::move(on_frame))) {
      error_ = why + "; " + fallback_name_ + " did not start either: " + f->last_error();
      return false;
    }
    log::Warn("screen capture: " + why + "; using " + fallback_name_);
    active_ = std::move(f);
    using_fallback_ = true;
    return true;
  }

  void Stop() override {
    if (active_) active_->Stop();
    active_.reset();
  }

  const std::string& last_error() const override { return active_ ? active_->last_error() : error_; }

  // Whether the running capture is the fallback one.
  bool using_fallback() const { return using_fallback_; }

 private:
  struct State {
    std::atomic<bool> got{false};
    std::mutex mu;
    std::condition_variable cv;
  };

  Factory preferred_, fallback_;
  std::string preferred_name_, fallback_name_;
  std::chrono::milliseconds wait_;
  std::unique_ptr<ICapturer> active_;
  bool using_fallback_ = false;
  std::string error_;
};

}  // namespace rdacore::capture
