// rdaccel_view: the viewer side across a real network, without a window.
// Connects to rdaccel_serve, decodes the stream and prints one line of
// statistics per second; --dump writes the last picture (with the cursor
// drawn on it) as a PPM file at the end.
//
//   rdaccel_view HOST [--udp=PORT | --tcp=PORT] [--seconds=10] [--dump=last.ppm]
//
// The protocol of both ends must match: --udp with --udp, --tcp with --tcp.
// Uses only the public API; a GUI application would draw the picture from
// on_frame and the cursor from on_cursor instead of counting them.

#include <rdaccel/rdaccel.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "args.h"

int main(int argc, char** argv) {
  rdaccel::examples::Args args(argc, argv);
  const std::string host_name = args.positional().empty() ? "127.0.0.1" : args.positional();
  const bool tcp = args.has("tcp");
  const auto port = static_cast<uint16_t>(args.num(tcp ? "tcp" : "udp", 7300));
  const double seconds = args.real("seconds", 10);
  const std::string dump = args.str("dump");

  std::string err;
  auto transport = tcp ? rdaccel::ConnectTcp(host_name, port, &err) : rdaccel::ConnectUdp(host_name, port, &err);
  if (!transport) {
    std::fprintf(stderr, "%s\n", err.c_str());
    return 1;
  }

  std::mutex mu;  // guards everything the handlers touch
  int32_t width = 0, height = 0;
  uint64_t frames = 0, cursor_updates = 0;
  std::vector<uint8_t> picture;  // the last frame, tightly packed
  rdaccel::CursorState cursor;
  rdaccel::CursorImage cursor_image;

  rdaccel::ViewerHandlers handlers;
  handlers.on_stream_info = [&](int32_t w, int32_t h) {
    std::lock_guard<std::mutex> lock(mu);
    width = w;
    height = h;
    std::printf("stream %dx%d\n", w, h);
  };
  handlers.on_frame = [&](const rdaccel::ImageView& img, std::span<const rdaccel::Rect>,
                          const rdaccel::FrameInfo&) {
    std::lock_guard<std::mutex> lock(mu);
    ++frames;
    if (dump.empty()) return;
    picture.resize(static_cast<size_t>(img.width) * img.height * 4);
    for (int y = 0; y < img.height; ++y) {
      std::memcpy(picture.data() + static_cast<size_t>(y) * img.width * 4,
                  img.data + static_cast<size_t>(y) * img.stride, static_cast<size_t>(img.width) * 4);
    }
  };
  handlers.on_cursor = [&](const rdaccel::CursorState& c) {
    std::lock_guard<std::mutex> lock(mu);
    ++cursor_updates;
    cursor = c;
    if (c.image_changed && c.image) {
      cursor_image = *c.image;
      std::printf("cursor image %dx%d, hotspot %d,%d\n", cursor_image.width, cursor_image.height,
                  cursor_image.hotspot_x, cursor_image.hotspot_y);
    }
    cursor.image = nullptr;  // only valid during the call
  };
  auto viewer = rdaccel::Viewer::Create(std::move(transport), std::move(handlers), &err);
  if (!viewer) {
    std::fprintf(stderr, "%s\n", err.c_str());
    return 1;
  }

  const auto t0 = std::chrono::steady_clock::now();
  uint64_t last_frames = 0, last_cursor = 0;
  rdaccel::ViewerStats last;
  for (int s = 1; s <= static_cast<int>(seconds + 0.5); ++s) {
    std::this_thread::sleep_until(t0 + std::chrono::seconds(s));
    const auto st = viewer->stats();
    std::lock_guard<std::mutex> lock(mu);
    const uint64_t dec = st.frames_decoded - last.frames_decoded;
    std::printf("%3ds  %3llu fps  %7.0f kbps  decode %5.2f ms  cursor %llu  errors %llu\n", s,
                static_cast<unsigned long long>(frames - last_frames),
                (st.bytes_received - last.bytes_received) * 8 / 1000.0,
                dec ? (st.decode_us_total - last.decode_us_total) / 1000.0 / dec : 0.0,
                static_cast<unsigned long long>(cursor_updates - last_cursor),
                static_cast<unsigned long long>(st.decode_errors));
    std::fflush(stdout);
    last = st;
    last_frames = frames;
    last_cursor = cursor_updates;
  }

  std::lock_guard<std::mutex> lock(mu);
  if (!dump.empty() && !picture.empty()) {
    if (cursor.visible && !cursor_image.bgra.empty()) {
      rdaccel::DrawCursor(cursor_image, cursor.x, cursor.y, picture.data(), width, height, width * 4);
    }
    std::FILE* f = std::fopen(dump.c_str(), "wb");
    if (f) {
      std::fprintf(f, "P6 %d %d 255\n", width, height);
      for (size_t i = 0; i < picture.size(); i += 4) {
        const uint8_t rgb[3] = {picture[i + 2], picture[i + 1], picture[i]};
        std::fwrite(rgb, 1, 3, f);
      }
      std::fclose(f);
      std::printf("wrote %s\n", dump.c_str());
    }
  }
  std::printf("decoder %s, %llu frames\n", viewer->decoder_name().c_str(), static_cast<unsigned long long>(frames));
  return frames > 0 ? 0 : 1;
}
