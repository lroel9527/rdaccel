#pragma once

// Screen capture on Wayland: the xdg-desktop-portal ScreenCast interface
// (over D-Bus) asks the compositor, and on most desktops the user, for one
// monitor, then the picture arrives as a PipeWire video stream. This is the
// only way to capture a Wayland desktop (GNOME, KDE, sway, Hyprland, ...);
// X11 capture there sees only XWayland windows, or nothing.
//
// The portal decides which monitor is shared (CaptureConfig::display_index
// must be 0); most desktops show a dialog the first time. The permission is
// remembered for the life of the process, and across runs when
// RDACCEL_PORTAL_TOKEN_FILE names a file to keep the portal's restore token
// in (portal version 4 and later).
//
// Frames arrive when the screen changes (no repeats for a still screen), with
// the compositor's damage rectangles when it sends them. The cursor comes as
// stream metadata when the portal offers it (GNOME, KDE): then
// CreatePipeWireCursorSource() feeds the cursor channel. Otherwise (wlroots)
// the cursor is drawn into the picture, so the viewer still sees it.

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "capture/capturer.h"
#include "capture/cursor_source.h"

namespace rdacore::platform {

struct PipeWireCursor;  // cursor metadata shared with the cursor source

class PipeWireCapturer final : public capture::ICapturer {
 public:
  PipeWireCapturer();
  // Reads PipeWire node `node_id` of the default PipeWire daemon directly,
  // without the portal (tests).
  explicit PipeWireCapturer(uint32_t node_id);
  ~PipeWireCapturer() override;

  bool Start(const capture::CaptureConfig& cfg, capture::FrameFn on_frame) override;
  void Stop() override;
  const std::string& last_error() const override { return error_; }

  struct Impl;

 private:
  std::optional<uint32_t> node_;
  std::unique_ptr<Impl> impl_;
  std::string error_;
};

// True when screen capture should go through the portal: a Wayland session
// (XDG_SESSION_TYPE=wayland, or WAYLAND_DISPLAY without DISPLAY).
// RDACCEL_CAPTURE=x11 or =pipewire overrides the guess.
bool UseWaylandCapture();

// The cursor of the running PipeWire capture, if it streams cursor metadata;
// null with `error` set otherwise, or with `error` empty when the capture
// draws the cursor into the picture instead (nothing is missing then).
std::unique_ptr<capture::ICursorSource> CreatePipeWireCursorSource(std::string* error);

// The size of the monitor the portal last shared in this process (0 x 0
// before the first capture).
void LastPipeWireSize(int32_t& width, int32_t& height);

namespace pw_detail {

// Byte positions of blue, green and red within a 4-byte pixel of a PipeWire
// (SPA) video format, or false for a format the capturer does not take.
bool ChannelOrder(uint32_t spa_format, int& b, int& g, int& r);

// Copies a w x h picture of 4-byte pixels with channels at b/g/r into packed
// BGRA (alpha 255).
void ToBgra(const uint8_t* src, int32_t src_stride, int32_t w, int32_t h, int b, int g, int r,
            uint8_t* dst, int32_t dst_stride);

}  // namespace pw_detail

}  // namespace rdacore::platform
