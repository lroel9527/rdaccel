#include "rdaccel/host.h"

#include <atomic>
#include <mutex>

#include "capture/capturer.h"
#include "platform.h"
#include "session/host_session.h"

namespace rdaccel {

struct Host::Impl {
  HostConfig config;

  // Created on the first frame, when the resolution is known. Never replaced,
  // so readers only need the atomic pointer.
  std::unique_ptr<session::HostSession> session;
  std::atomic<session::HostSession*> session_ptr{nullptr};
  std::unique_ptr<capture::ICapturer> capturer;
  // Declared after `session`, so it is destroyed first: its destructor stops
  // the receive thread that calls into the session.
  std::unique_ptr<Transport> transport;

  std::mutex submit_mu;  // serializes frames (screen capture vs SubmitFrame)
  std::atomic<bool> failed{false};  // encoder creation failed; drop further frames
  bool size_error_reported = false;
  int32_t width = 0, height = 0;

  mutable std::mutex mu;  // guards the fields below
  std::string encoder_name;
  InputHandler on_input;
  ErrorHandler on_error;

  void ReportError(const std::string& msg) {
    ErrorHandler fn;
    {
      std::lock_guard<std::mutex> lock(mu);
      fn = on_error;
    }
    if (fn) fn(msg);
  }

  bool OnFrame(const capture::CapturedFrame& f, std::string* error) {
    std::lock_guard<std::mutex> lock(submit_mu);
    if (failed) {
      if (error) *error = "host stopped after an earlier encoder error";
      return false;
    }
    if (!session) {
      codec::EncoderConfig cfg{f.bgra.width, f.bgra.height, config.fps, config.bitrate_bps};
      std::string err;
      auto enc = platform::CreateHevcEncoder(cfg, config.encoder, &err);
      if (!enc) {
        failed = true;
        if (error) *error = err;
        ReportError(err);  // also reaches apps that only use screen capture
        return false;
      }
      {
        std::lock_guard<std::mutex> l(mu);
        encoder_name = enc->name();
      }
      width = f.bgra.width;
      height = f.bgra.height;
      session = std::make_unique<session::HostSession>(transport.get(), std::move(enc), width,
                                                       height);
      session->SetInputHandler([this](const InputEvent& ev) {
        InputHandler fn;
        {
          std::lock_guard<std::mutex> l(mu);
          fn = on_input;
        }
        if (fn) fn(ev);
      });
      session_ptr = session.get();
    } else if (f.bgra.width != width || f.bgra.height != height) {
      const std::string err = "frame size changed from " + std::to_string(width) + "x" +
                              std::to_string(height) + " to " + std::to_string(f.bgra.width) +
                              "x" + std::to_string(f.bgra.height) + " (not supported yet)";
      if (error) *error = err;
      if (!size_error_reported) {
        size_error_reported = true;
        ReportError(err);
      }
      return false;
    }
    session->OnFrame(f);
    return true;
  }
};

Host::Host(std::unique_ptr<Transport> transport, HostConfig config)
    : impl_(std::make_unique<Impl>()) {
  impl_->config = std::move(config);
  impl_->transport = std::move(transport);
}

Host::~Host() {
  StopScreenCapture();
  // Stop the receive thread before the session goes away.
  impl_->transport.reset();
}

void Host::SetInputHandler(InputHandler fn) {
  std::lock_guard<std::mutex> lock(impl_->mu);
  impl_->on_input = std::move(fn);
}

void Host::SetErrorHandler(ErrorHandler fn) {
  std::lock_guard<std::mutex> lock(impl_->mu);
  impl_->on_error = std::move(fn);
}

bool Host::StartScreenCapture(std::string* error) {
  if (impl_->capturer) {
    if (error) *error = "screen capture already running";
    return false;
  }
  std::string err;
  auto cap = platform::CreateScreenCapturer(&err);
  if (!cap) {
    if (error) *error = err;
    return false;
  }
  capture::CaptureConfig cc;
  cc.fps = impl_->config.fps;
  cc.width = impl_->config.capture_width;
  cc.height = impl_->config.capture_height;
  cc.show_cursor = impl_->config.capture_cursor;
  Impl* impl = impl_.get();
  if (!cap->Start(cc, [impl](const capture::CapturedFrame& f) {
        // OnFrame reports errors through the error handler itself.
        impl->OnFrame(f, nullptr);
      })) {
    if (error) *error = cap->last_error();
    return false;
  }
  impl_->capturer = std::move(cap);
  return true;
}

void Host::StopScreenCapture() {
  if (impl_->capturer) {
    impl_->capturer->Stop();
    impl_->capturer.reset();
  }
}

bool Host::SubmitFrame(const ImageView& bgra, uint64_t capture_time_us, std::span<const Rect> dirty,
                       std::string* error) {
  if (!bgra.data || bgra.width <= 0 || bgra.height <= 0 || bgra.stride < bgra.width * 4) {
    if (error) *error = "invalid image";
    return false;
  }
  capture::CapturedFrame f;
  f.bgra = bgra;
  f.dirty.assign(dirty.begin(), dirty.end());
  f.capture_time_us = capture_time_us;
  return impl_->OnFrame(f, error);
}

void Host::RequestKeyframe() {
  if (auto* s = impl_->session_ptr.load()) s->RequestKeyframe();
  // Before the first frame there is nothing to do: the first frame is always
  // a keyframe.
}

std::string Host::encoder_name() const {
  std::lock_guard<std::mutex> lock(impl_->mu);
  return impl_->encoder_name;
}

HostStats Host::stats() const {
  auto* s = impl_->session_ptr.load();
  return s ? s->stats() : HostStats{};
}

}  // namespace rdaccel
