// The smallest complete integration: a Host and a Viewer in one process,
// connected by an in-process pipe, streaming the built-in test pattern.
// Replace SubmitFrame with StartScreenCapture() to stream the real screen,
// and the pipe with ListenTcp / ConnectTcp (or your own Transport) to go
// across machines.

#include <rdaccel/rdaccel.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

int main() {
  auto [host_end, viewer_end] = rdaccel::CreateLocalPipe();

  std::atomic<int> frames{0};
  rdaccel::ViewerHandlers handlers;
  handlers.on_frame = [&](const rdaccel::ImageView& img, std::span<const rdaccel::Rect> changed,
                          const rdaccel::FrameInfo& info) {
    std::printf("frame %llu: %dx%d, %zu changed areas\n",
                static_cast<unsigned long long>(info.frame_id), img.width, img.height,
                changed.size());
    ++frames;
  };
  std::string err;
  auto viewer = rdaccel::Viewer::Create(std::move(viewer_end), std::move(handlers), &err);
  if (!viewer) {
    std::fprintf(stderr, "viewer: %s\n", err.c_str());
    return 1;
  }

  rdaccel::Host host(std::move(host_end));
  const int w = 640, h = 360, n = 10;
  std::vector<uint8_t> px;
  for (int i = 0; i < n; ++i) {
    const auto dirty = rdaccel::RenderTestPattern(i, w, h, px);
    if (!host.SubmitFrame({px.data(), w, h, w * 4}, rdaccel::NowMicros(), dirty, &err)) {
      std::fprintf(stderr, "host: %s\n", err.c_str());
      return 1;
    }
  }

  // Frames are decoded on the pipe's thread; wait for them.
  for (int i = 0; i < 200 && frames < n; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  std::printf("rdaccel %s on %s, encoder %s, decoder %s: %d/%d frames\n", rdaccel::Version(),
              rdaccel::PlatformName(), host.encoder_name().c_str(),
              viewer->decoder_name().c_str(), frames.load(), n);
  return frames == n ? 0 : 1;
}
