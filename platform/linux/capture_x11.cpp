#include "linux/capture_x11.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>
#include <sys/ipc.h>
#include <sys/shm.h>

#include <chrono>
#include <vector>

extern "C" {
#include <libswscale/swscale.h>
}

namespace rdaccel::platform {

bool X11Capturer::Start(const capture::CaptureConfig& cfg, capture::FrameFn on_frame) {
  if (running_) {
    error_ = "already running";
    return false;
  }
  Display* dpy = XOpenDisplay(nullptr);
  if (!dpy) {
    error_ = "cannot open X display (is DISPLAY set?)";
    return false;
  }
  if (!XShmQueryExtension(dpy)) {
    error_ = "X server lacks the MIT-SHM extension";
    XCloseDisplay(dpy);
    return false;
  }
  const int screen = DefaultScreen(dpy);
  const Window root = RootWindow(dpy, screen);
  const int w = DisplayWidth(dpy, screen);
  const int h = DisplayHeight(dpy, screen);
  Visual* visual = DefaultVisual(dpy, screen);
  const int depth = DefaultDepth(dpy, screen);
  if (depth != 24 && depth != 32) {
    error_ = "unsupported X depth " + std::to_string(depth);
    XCloseDisplay(dpy);
    return false;
  }

  auto* shm = new XShmSegmentInfo{};
  XImage* img = XShmCreateImage(dpy, visual, static_cast<unsigned>(depth), ZPixmap, nullptr, shm,
                                static_cast<unsigned>(w), static_cast<unsigned>(h));
  if (!img || img->bits_per_pixel != 32) {
    error_ = "XShmCreateImage failed or not 32 bpp";
    if (img) XDestroyImage(img);
    delete shm;
    XCloseDisplay(dpy);
    return false;
  }
  shm->shmid = shmget(IPC_PRIVATE, static_cast<size_t>(img->bytes_per_line) * img->height,
                      IPC_CREAT | 0600);
  shm->shmaddr = img->data = static_cast<char*>(shmat(shm->shmid, nullptr, 0));
  shm->readOnly = False;
  if (shm->shmid < 0 || shm->shmaddr == reinterpret_cast<char*>(-1) || !XShmAttach(dpy, shm)) {
    error_ = "shared memory setup failed";
    XCloseDisplay(dpy);
    return false;
  }
  // Mark the segment for removal now; it lives until both sides detach.
  shmctl(shm->shmid, IPC_RMID, nullptr);

  // Scaled capture (CaptureConfig::width/height): area averaging, like a
  // display set to a lower resolution. Even sizes, as the encoder needs.
  int out_w = w, out_h = h;
  if (cfg.width > 0 && cfg.height > 0 && (cfg.width != w || cfg.height != h)) {
    out_w = cfg.width & ~1;
    out_h = cfg.height & ~1;
  }
  SwsContext* sws = nullptr;
  if (out_w != w || out_h != h) {
    sws = sws_getContext(w, h, AV_PIX_FMT_BGRA, out_w, out_h, AV_PIX_FMT_BGRA, SWS_AREA, nullptr,
                         nullptr, nullptr);
    if (!sws) {
      error_ = "cannot scale the capture to " + std::to_string(out_w) + "x" + std::to_string(out_h);
      XShmDetach(dpy, shm);
      shmdt(shm->shmaddr);
      img->data = nullptr;
      XDestroyImage(img);
      delete shm;
      XCloseDisplay(dpy);
      return false;
    }
  }

  running_ = true;
  const int fps = cfg.fps > 0 ? cfg.fps : 60;
  thread_ = std::thread([this, dpy, root, img, shm, w, h, out_w, out_h, sws, fps,
                         on_frame = std::move(on_frame)] {
    const auto period = std::chrono::microseconds(1'000'000 / fps);
    auto next = std::chrono::steady_clock::now();
    std::vector<uint8_t> scaled(sws ? static_cast<size_t>(out_w) * out_h * 4 : 0);
    while (running_) {
      if (XShmGetImage(dpy, root, img, 0, 0, AllPlanes)) {
        // 24/32-bit TrueColor ZPixmap on little-endian is B, G, R, X in
        // memory, which is the BGRA layout the pipeline uses (alpha ignored).
        capture::CapturedFrame f;
        f.bgra = {reinterpret_cast<const uint8_t*>(img->data), w, h, img->bytes_per_line};
        if (sws) {
          const uint8_t* src[1] = {f.bgra.data};
          const int src_stride[1] = {f.bgra.stride};
          uint8_t* dst[1] = {scaled.data()};
          const int dst_stride[1] = {out_w * 4};
          sws_scale(sws, src, src_stride, 0, h, dst, dst_stride);
          f.bgra = {scaled.data(), out_w, out_h, out_w * 4};
        }
        f.capture_time_us = capture::MonotonicMicros();
        on_frame(f);
      }
      next += period;
      std::this_thread::sleep_until(next);
    }
    sws_freeContext(sws);
    XShmDetach(dpy, shm);
    shmdt(shm->shmaddr);
    img->data = nullptr;  // shm memory, must not be free()d by XDestroyImage
    XDestroyImage(img);
    delete shm;
    XCloseDisplay(dpy);
  });
  return true;
}

void X11Capturer::Stop() {
  running_ = false;
  if (thread_.joinable()) thread_.join();
}

}  // namespace rdaccel::platform
