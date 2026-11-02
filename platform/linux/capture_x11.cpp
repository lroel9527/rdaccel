#include "linux/capture_x11.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>
#include <sys/ipc.h>
#include <sys/shm.h>

#include <chrono>

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

  running_ = true;
  const int fps = cfg.fps > 0 ? cfg.fps : 60;
  thread_ = std::thread([this, dpy, root, img, shm, w, h, fps, on_frame = std::move(on_frame)] {
    const auto period = std::chrono::microseconds(1'000'000 / fps);
    auto next = std::chrono::steady_clock::now();
    while (running_) {
      if (XShmGetImage(dpy, root, img, 0, 0, AllPlanes)) {
        // 24/32-bit TrueColor ZPixmap on little-endian is B, G, R, X in
        // memory, which is the BGRA layout the pipeline uses (alpha ignored).
        capture::CapturedFrame f;
        f.bgra = {reinterpret_cast<const uint8_t*>(img->data), w, h, img->bytes_per_line};
        f.capture_time_us = capture::MonotonicMicros();
        on_frame(f);
      }
      next += period;
      std::this_thread::sleep_until(next);
    }
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
