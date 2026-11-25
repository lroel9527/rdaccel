// rdaccel_clip: test clips of a screen (tools/clip/clip.h), and how the host
// does on them. A clip is played into a HostSession and a ViewerSession the
// way the Host runs them on a real screen, so two versions of rdaccel can be
// compared on exactly the same screen content. See docs/clips.md.
//
//   rdaccel_clip record OUT.clip [--seconds=60] [--display=0] [--fps=60]
//       Records the screen as the capturer delivers it (only frames that
//       changed), losslessly. Needs the Screen Recording permission.
//   rdaccel_clip render OUT.clip --scenario=NAME [--background=SCREEN.ppm] [--seconds=20]
//       One of the example scenes (examples/scenes.h: idle typing scroll drag
//       zoom video pause) as a clip, at 60 Hz.
//   rdaccel_clip eval CLIP [--text=0] [--bitrate=8000000] [--label=NAME] [--csv=FRAMES.csv] [--focus=x,y,w,h]
//                     [--refine=N] [--still-refine=N] [--split-cells=N] [--split-hot=1] [--text-refine=1] [--cells=1]
//       Sends the clip through a host and a viewer with the platform's video
//       encoder and decoder, as the Host does it: each frame when the screen
//       changed, and the last one every 100 ms while something is owed.
//       Prints one line: what was sent (video, lossless), the share of
//       pixels the viewer shows exactly and its PSNR (weighted by time), and
//       for the pauses (0.5 s or more without a change) how much is exact at
//       their end. --csv writes what each frame sent.
//       Further lines: the text of the screen apart from the rest
//       (text_cells.h), what the encoder's pictures were coded for and why
//       rects went to it; --cells=1 lists the text cells left unsharp.
//   rdaccel_clip frame CLIP --index=N --out=FRAME.ppm [--textmap=1]
//       Frame N of the clip as a PPM, to look at what was recorded;
//       --textmap=1 dims what is not counted as text.
//   rdaccel_clip decisions CLIP --out=FILE [--text=0] [--bitrate=8000000] [--fps=60] [--perturb=1]
//                          [--refine=N] [--still-refine=N] [--split-cells=N] [--split-hot=1] [--text-refine=1] [--linger-ms=N]
//       Plays the clip into a host as eval does, with a fake encoder whose
//       pictures depend only on its input and no viewer, and writes one line
//       per frame of what the host decided (what it sent on each channel, as
//       sizes and hashes; session::FrameTrace). Two builds that decide alike
//       write the same file: check_decisions.sh compares them on every clip.
//       --perturb=1 also requests keyframes, changes the bitrate, fails the
//       encoder once and switches text mode, at fixed points of the clip.
//
// eval works on the internals (session::HostSession), not the public API, so
// it sees every frame the host decides on; the numbers are what the Host
// sends on a fast path (no congestion, a fixed bitrate).

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "args.h"
#include "clip.h"
#include "platform.h"
#include "scenes.h"
#include "session/host_session.h"
#include "session/viewer_session.h"
#include "text_cells.h"
#include "transport/local_pipe.h"

using namespace rdacore;
using rdaccel::clip::ClipReader;
using rdaccel::clip::ClipWriter;
using rdaccel::clip::kTextCell;
using rdaccel::examples::Args;

namespace {

int Usage() {
  std::fprintf(stderr,
               "usage: rdaccel_clip record OUT.clip [--seconds=60] [--display=0] [--fps=60]\n"
               "       rdaccel_clip render OUT.clip --scenario=NAME [--background=SCREEN.ppm] [--seconds=20]\n"
               "       rdaccel_clip eval CLIP [--text=0] [--bitrate=8000000] [--label=NAME] [--csv=FRAMES.csv] [--focus=x,y,w,h]\n"
               "                    [--refine=N] [--still-refine=N] [--split-cells=N] [--split-hot=1] [--text-refine=1] [--cells=1]\n"
               "       rdaccel_clip frame CLIP --index=N --out=FRAME.ppm [--textmap=1]\n"
               "       rdaccel_clip decisions CLIP --out=FILE [--text=0] [--bitrate=8000000] [--fps=60] [--perturb=1]\n"
               "                    [--refine=N] [--still-refine=N] [--split-cells=N] [--split-hot=1] [--text-refine=1] [--linger-ms=N]\n");
  return 2;
}

int Record(const Args& args) {
  ClipWriter out(args.positional().c_str());
  if (!out.ok()) {
    std::fprintf(stderr, "cannot write %s\n", args.positional().c_str());
    return 1;
  }
  std::string err;
  auto cap = platform::CreateScreenCapturer(&err);
  if (!cap) {
    std::fprintf(stderr, "%s\n", err.c_str());
    return 1;
  }
  std::mutex mu;
  uint64_t frames = 0, bytes = 0;
  capture::CaptureConfig cfg;
  cfg.display_index = static_cast<int32_t>(args.num("display", 0));
  cfg.fps = static_cast<int32_t>(args.num("fps", 60));
  if (!cap->Start(cfg, [&](const capture::CapturedFrame& f) {
        std::lock_guard<std::mutex> lock(mu);
        bytes += out.Write(f.capture_time_us, f.bgra.data, f.bgra.width, f.bgra.height, f.bgra.stride, f.dirty);
        ++frames;
      })) {
    std::fprintf(stderr, "capture: %s\n", cap->last_error().c_str());
    return 1;
  }
  const long long seconds = args.num("seconds", 60);
  for (long long s = 1; s <= seconds; ++s) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    if (s % 15 == 0 && s < seconds) {
      std::lock_guard<std::mutex> lock(mu);
      std::fprintf(stderr, "%llds: %llu frames, %.1f MB\n", s, static_cast<unsigned long long>(frames), bytes / 1e6);
    }
  }
  cap->Stop();
  std::lock_guard<std::mutex> lock(mu);
  std::printf("recorded %llu frames, %.1f MB\n", static_cast<unsigned long long>(frames), bytes / 1e6);
  return 0;
}

int Render(const Args& args) {
  using namespace rdaccel::examples;
  Image bg;
  int w = 1920, h = 1080;
  const std::string bg_path = args.str("background");
  if (!bg_path.empty()) {
    if (!LoadPpm(bg_path, bg, w, h)) {
      std::fprintf(stderr, "cannot read %s\n", bg_path.c_str());
      return 1;
    }
  } else {
    rdaccel::RenderTestPattern(600, w, h, bg);
  }
  const Scene scene{&bg, w, h};
  const Scenario* sc = nullptr;
  for (const auto& s : kScenarios) {
    if (args.str("scenario") == s.name) sc = &s;
  }
  if (!sc) {
    std::fprintf(stderr, "unknown --scenario (idle typing scroll drag zoom video pause pointer terminal)\n");
    return 2;
  }
  ClipWriter out(args.positional().c_str());
  if (!out.ok()) return 1;
  const auto frames = static_cast<uint64_t>(args.num("seconds", 20) * 60);
  Image img, last;
  uint64_t written = 0;
  for (uint64_t i = 0; i < frames; ++i) {
    Render(scene, sc->fn, i, img);
    if (i > 0 && img == last) continue;  // a screen source sends nothing then
    last = img;
    out.Write(1'000'000 + i * 1'000'000 / 60, img.data(), w, h, w * 4, {});
    ++written;
  }
  std::printf("%s: %llu of %llu frames\n", sc->name, static_cast<unsigned long long>(written),
              static_cast<unsigned long long>(frames));
  return 0;
}

int Frame(const Args& args) {
  ClipReader clip(args.positional().c_str());
  const long long want = args.num("index", 0);
  long long i = -1;
  while (i < want && clip.Next()) ++i;
  if (i < 0) {
    std::fprintf(stderr, "no frames\n");
    return 1;
  }
  std::FILE* f = std::fopen(args.str("out", "frame.ppm").c_str(), "wb");
  if (!f) return 1;
  std::fprintf(f, "P6 %d %d 255\n", clip.width(), clip.height());
  const auto& px = clip.frame();
  // --textmap=1: the cells counted as text (text_cells.h) keep their colours,
  // the rest is dimmed, to check the labelling by eye.
  std::vector<uint8_t> text_cell;
  const bool textmap = args.num("textmap", 0) != 0;
  if (textmap) rdaccel::clip::LabelTextCells(px.data(), clip.width(), clip.height(), clip.width() * 4, text_cell);
  uint64_t n_text = 0;
  for (const uint8_t c : text_cell) n_text += c;
  for (size_t k = 0; k < px.size(); k += 4) {
    uint8_t rgb[3] = {px[k + 2], px[k + 1], px[k]};
    if (textmap) {
      const auto p = static_cast<int32_t>(k / 4);
      const int32_t cx = p % clip.width() / kTextCell, cy = p / clip.width() / kTextCell;
      if (!text_cell[static_cast<size_t>(cy) * rdaccel::clip::TextCellCols(clip.width()) + cx]) {
        for (auto& v : rgb) v = static_cast<uint8_t>(v / 4);
      }
    }
    std::fwrite(rgb, 1, 3, f);
  }
  if (textmap) std::printf("%llu of %zu cells are text\n", static_cast<unsigned long long>(n_text), text_cell.size());
  std::fclose(f);
  std::printf("frame %lld of the clip\n", i);
  return 0;
}

// The experiments eval and decisions take. How much the refinement and the
// still-screen polish cost and give (HostOptions::refine_frames,
// still_refine_frames). Text mode's decisions (HostOptions): --split-cells=0
// --text-refine=1 is text mode as it was up to rdaccel 3.0.1.
void SetExperiments(const Args& args, session::HostOptions& o) {
  o.split_cells = static_cast<int>(args.num("split-cells", o.split_cells));
  o.split_hot = args.num("split-hot", o.split_hot ? 1 : 0) != 0;
  o.text_refine = args.num("text-refine", o.text_refine ? 1 : 0) != 0;
  if (args.has("linger-ms")) o.text_linger_us = static_cast<int64_t>(args.num("linger-ms", 0) * 1000);
  if (args.has("refine")) o.refine_frames = static_cast<int>(args.num("refine", o.refine_frames));
  if (args.has("still-refine")) o.still_refine_frames = static_cast<int>(args.num("still-refine", o.still_refine_frames));
}

int Eval(const Args& args) {
  ClipReader clip(args.positional().c_str());
  if (!clip.ok()) {
    std::fprintf(stderr, "cannot read %s\n", args.positional().c_str());
    return 1;
  }
  const bool text = args.num("text", 0) != 0;
  const int64_t bitrate = args.num("bitrate", 8'000'000);
  const std::string label = args.str("label", args.positional());
  constexpr uint64_t kResubmitUs = 100'000;  // as the Host's idle loop
  constexpr uint64_t kPauseUs = 500'000;

  auto [host_end, viewer_end] = transport::MakeLocalPipe();
  std::string err;
  auto decoder = platform::CreateVideoDecoder(&err);
  if (!decoder) {
    std::fprintf(stderr, "decoder: %s\n", err.c_str());
    return 1;
  }
  session::ViewerSession viewer(nullptr, std::move(decoder));
  // Video areas have their own streams and decoders, as in the real viewer
  // (src/viewer.cpp): without them every area that opened made the viewer
  // ask for a keyframe, which closed it again.
  viewer.SetDecoderFactory([] { return platform::CreateVideoDecoder(nullptr); });
  std::mutex mu;
  std::condition_variable cv;
  std::vector<uint8_t> shown;
  uint64_t shown_id = 0;
  viewer.SetFrameHandler([&](const tile::ImageView& img, const std::vector<Rect>&, uint64_t id, uint64_t) {
    std::lock_guard<std::mutex> lock(mu);
    shown.resize(static_cast<size_t>(img.width) * img.height * 4);
    for (int32_t y = 0; y < img.height; ++y) {
      std::memcpy(shown.data() + static_cast<size_t>(y) * img.width * 4, img.data + static_cast<size_t>(y) * img.stride,
                  static_cast<size_t>(img.width) * 4);
    }
    shown_id = id;
    cv.notify_all();
  });

  std::unique_ptr<session::HostSession> host;
  std::vector<uint8_t> screen;
  int32_t W = 0, H = 0;
  uint64_t first_t = 0, last_submit = 0, frames = 0, resent = 0, missed = 0;
  // Quality weighted by time: what the viewer shows from one frame to the next.
  double w_exact = 0, w_psnr = 0, w_time = 0, last_exact = 0, last_psnr = 0;
  uint64_t last_q_t = 0;
  // --focus=x,y,w,h: PSNR inside that area (a video window) and outside it,
  // also weighted by time.
  Rect focus{};
  if (args.has("focus")) std::sscanf(args.str("focus").c_str(), "%d,%d,%d,%d", &focus.x, &focus.y, &focus.w, &focus.h);
  double w_in = 0, w_out = 0, last_in = 0, last_out = 0;
  // Each clip frame opens a stretch until the next one; those of kPauseUs or
  // more are pauses. Samples: (time, exact %) within it.
  struct Stretch {
    uint64_t start;
    std::vector<std::pair<uint64_t, double>> samples;
  };
  std::vector<Stretch> stretches;

  // The text of the screen (text_cells.h), apart from the rest. A text cell
  // on the viewer is one of:
  //   exact    every pixel as on the screen;
  //   sharp    from the lossless layer and close to the screen: text mode's
  //            reduced colours (edges where they are, shades merged);
  //   before   still the content from before the cell's last change;
  //   soft     from the video encoder and close to the screen;
  //   wrong    none of these (a placeholder, or far from the screen).
  // Text that changed is timed until it is exact or sharp.
  constexpr double kSoftDb = 25.0;  // "close to the screen": at least this PSNR over the cell
  std::vector<uint8_t> prev_screen, before, text_cell, has_before;
  std::vector<uint64_t> changed_at;  // per cell: when its text changed, while the viewer has not shown it exactly
  int32_t cols = 0, rows = 0;
  double w_tshare = 0, w_tpx = 0, w_tcell = 0, w_sharp = 0, w_stale = 0, w_soft = 0, w_wrong = 0;
  double last_tshare = 0, last_tpx = 100, last_tcell = 100, last_sharp = 0, last_stale = 0, last_soft = 0, last_wrong = 0;
  double sharp_se = 0, sharp_n = 0;  // how far sharp cells are from the screen
  double soft_se = 0, soft_n = 0;    // ... and soft ones
  // Soft cells by the colours the cell itself has on the screen: up to 16,
  // up to 64, more. (Few colours in a soft cell: it went to the encoder for
  // what else is in its tile.)
  double w_soft16 = 0, w_soft64 = 0, last_soft16 = 0, last_soft64 = 0;
  // The same text cells by their pixels alone: how close the viewer's cell
  // is to the screen's, whatever the host believes it sent. "sharp" above
  // rests on the host's own bookkeeping (ShownQuality), which can be wrong;
  // this does not. Shares of cells at least 40 and 30 dB PSNR over the cell
  // (exact ones included), and the PSNR of the cells that are not exact,
  // capped at kCapDb so a nearly exact cell does not outweigh the rest,
  // averaged over cell-time (sum and count both weighted by time).
  constexpr double kCapDb = 60.0;
  double w_px40 = 0, w_px30 = 0, w_ne_db = 0, w_ne = 0;
  double last_px40 = 100, last_px30 = 100, last_ne_db = 0, last_ne = 0;
  auto cell_colours = [&](const Rect& r) {
    uint32_t seen[65];
    int n = 0;
    for (int32_t y = r.y; y < r.bottom(); ++y) {
      const uint8_t* p = screen.data() + (static_cast<size_t>(y) * W + r.x) * 4;
      for (int32_t x = 0; x < r.w; ++x, p += 4) {
        const uint32_t c = static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]) << 16;
        int k = 0;
        while (k < n && seen[k] != c) ++k;
        if (k < n) continue;
        if (n == 65) return 65;
        seen[n++] = c;
      }
    }
    return n;
  };
  std::vector<double> to_exact_ms;
  uint64_t superseded = 0;
  double superseded_ms = 0;
  auto cell_rect = [&](int32_t cx, int32_t cy) {
    Rect r{cx * kTextCell, cy * kTextCell, kTextCell, kTextCell};
    r.w = std::min(r.w, W - r.x);
    r.h = std::min(r.h, H - r.y);
    return r;
  };
  auto cell_equal = [&](const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, const Rect& r) {
    for (int32_t y = r.y; y < r.bottom(); ++y) {
      const size_t off = (static_cast<size_t>(y) * W + r.x) * 4;
      if (std::memcmp(a.data() + off, b.data() + off, static_cast<size_t>(r.w) * 4) != 0) return false;
    }
    return true;
  };
  // A new clip frame is in `screen`, the one before it in `prev_screen`.
  auto new_screen = [&](uint64_t t) {
    cols = rdaccel::clip::TextCellCols(W);
    rows = rdaccel::clip::TextCellRows(H);
    rdaccel::clip::LabelTextCells(screen.data(), W, H, W * 4, text_cell);
    const size_t n = static_cast<size_t>(cols) * rows;
    changed_at.resize(n, 0);
    has_before.resize(n, 0);
    before.resize(screen.size());
    const bool first = prev_screen.size() != screen.size();
    for (int32_t cy = 0; cy < rows; ++cy) {
      for (int32_t cx = 0; cx < cols; ++cx) {
        const size_t idx = static_cast<size_t>(cy) * cols + cx;
        const Rect r = cell_rect(cx, cy);
        const bool changed = first || !cell_equal(prev_screen, screen, r);
        if (changed && !first) {
          for (int32_t y = r.y; y < r.bottom(); ++y) {
            const size_t off = (static_cast<size_t>(y) * W + r.x) * 4;
            std::memcpy(before.data() + off, prev_screen.data() + off, static_cast<size_t>(r.w) * 4);
          }
          has_before[idx] = 1;
        }
        if (!text_cell[idx]) {
          changed_at[idx] = 0;
        } else if (changed) {
          if (changed_at[idx] != 0) {
            ++superseded;
            superseded_ms += static_cast<double>(t - changed_at[idx]) / 1e3;
          }
          changed_at[idx] = t;
        }
      }
    }
  };

  // What the full-screen encoder's pictures were coded for, and why rects
  // went to it (session::FrameTrace).
  struct {
    double b_key = 0, b_new = 0, b_refine = 0, b_polish = 0, b_other = 0;
    uint64_t pictures = 0, pictures_no_new = 0;
    uint64_t colours = 0, hot = 0, budget = 0, keyframe_first = 0, motion = 0, lossless = 0;
  } why;
  session::FrameTrace last_trace;

  std::FILE* csv = nullptr;
  if (args.has("csv")) {
    csv = std::fopen(args.str("csv").c_str(), "w");
    if (csv) std::fprintf(csv, "t_ms,resent,dirty_px,video_b,lossless_b,lossless_rects,moved_tiles,cached_tiles,owed_tiles,video_frame,exact_pct,text_exact_pct,text_sharp_pct,text_before_pct,text_soft_pct,text_wrong_pct,hot,hot_unmoved,moves,motion,video_rects,text_px40_pct,text_px30_pct\n");
  }

  // --audit=1: cells whose pixels on the viewer contradict what the host
  // believes it shows there (HostSession::ShownQuality): all from the
  // lossless layer yet far from the screen, or finished video very far from
  // it. Nothing is owed for such a cell, so it stays as it is until its
  // content changes.
  const bool audit = args.num("audit", 0) != 0;
  constexpr double kAuditLosslessDb = 30.0;
  const double kAuditFinishedDb = static_cast<double>(args.num("audit-db", 20));
  std::vector<uint64_t> bad_since;  // per cell: when it turned bad (0 = fine)
  uint64_t bad_events = 0, bad_printed = 0;
  auto cell_error = [&](const Rect& r, uint64_t& same) {
    double se = 0;
    same = 0;
    for (int32_t y = r.y; y < r.bottom(); ++y) {
      const size_t off = (static_cast<size_t>(y) * W + r.x) * 4;
      for (int32_t x = 0; x < r.w; ++x) {
        const size_t i = off + static_cast<size_t>(x) * 4;
        const int d0 = shown[i] - screen[i], d1 = shown[i + 1] - screen[i + 1], d2 = shown[i + 2] - screen[i + 2];
        same += (d0 | d1 | d2) == 0;
        se += d0 * d0 + d1 * d1 + d2 * d2;
      }
    }
    const double n = static_cast<double>(r.w) * r.h;
    return se == 0 ? 99.0 : 10 * std::log10(255.0 * 255.0 / (se / (n * 3)));
  };
  // 1 = lossless by the host's account but far, 2 = finished video but very far.
  auto audit_kind = [&](const Rect& r, double& db, int& q) {
    uint64_t same = 0;
    db = cell_error(r, same);
    q = host->ShownQuality(r);
    if (same == static_cast<uint64_t>(r.w) * r.h) return 0;
    if (q == 0 && db < kAuditLosslessDb) return 1;
    if (q == 1 && db < kAuditFinishedDb) return 2;
    return 0;
  };
  auto audit_cells = [&](uint64_t t) {
    bad_since.resize(static_cast<size_t>(cols) * rows, 0);
    for (int32_t cy = 0; cy < rows; ++cy) {
      for (int32_t cx = 0; cx < cols; ++cx) {
        const size_t idx = static_cast<size_t>(cy) * cols + cx;
        const Rect r = cell_rect(cx, cy);
        double db = 0;
        int q = 0;
        const int kind = audit_kind(r, db, q);
        if (kind == 0) {
          bad_since[idx] = 0;
          continue;
        }
        if (bad_since[idx] != 0) continue;
        bad_since[idx] = t;
        ++bad_events;
        if (bad_printed++ < 60) {
          std::printf("  audit %8.0f ms: cell %4d,%4d %s, PSNR %5.1f%s\n", (t - first_t) / 1e3, r.x, r.y,
                      kind == 1 ? "lossless by the host's account" : "finished video", db,
                      text_cell.size() > idx && text_cell[idx] ? "  (text)" : "");
        }
      }
    }
  };

  auto measure = [&](uint64_t t) {
    // Wait (briefly) until the viewer shows what the host sent. Some frames
    // never reach the screen as their own picture; then what the viewer
    // shows meanwhile is what is measured.
    const uint64_t want = host->next_frame_id() - 1;
    std::unique_lock<std::mutex> lock(mu);
    const bool caught_up = cv.wait_for(lock, std::chrono::milliseconds(400), [&] { return shown_id >= want; });
    if (!caught_up) ++missed;
    if (shown.size() != screen.size()) return;
    if (audit && caught_up) audit_cells(t);
    if (last_q_t) {
      const double dt = static_cast<double>(t - last_q_t);
      w_exact += last_exact * dt;
      w_psnr += last_psnr * dt;
      w_in += last_in * dt;
      w_out += last_out * dt;
      w_tshare += last_tshare * dt;
      w_tpx += last_tpx * dt;
      w_tcell += last_tcell * dt;
      w_sharp += last_sharp * dt;
      w_stale += last_stale * dt;
      w_soft += last_soft * dt;
      w_soft16 += last_soft16 * dt;
      w_soft64 += last_soft64 * dt;
      w_wrong += last_wrong * dt;
      w_px40 += last_px40 * dt;
      w_px30 += last_px30 * dt;
      w_ne_db += last_ne_db * dt;
      w_ne += last_ne * dt;
      w_time += dt;
    }
    uint64_t same = 0, n_in = 0;
    double se = 0, se_in = 0;
    for (size_t i = 0; i < screen.size(); i += 4) {
      const int d0 = shown[i] - screen[i], d1 = shown[i + 1] - screen[i + 1], d2 = shown[i + 2] - screen[i + 2];
      same += (d0 | d1 | d2) == 0;
      const double e = d0 * d0 + d1 * d1 + d2 * d2;
      se += e;
      if (focus.w > 0) {
        const auto p = static_cast<int32_t>(i / 4);
        const int32_t x = p % W, y = p / W;
        if (x >= focus.x && x < focus.right() && y >= focus.y && y < focus.bottom()) {
          se_in += e;
          ++n_in;
        }
      }
    }
    if (focus.w > 0) {
      auto db = [](double sq, double n) { return sq == 0 || n == 0 ? 99.0 : std::min(99.0, 10 * std::log10(255.0 * 255.0 / (sq / (n * 3)))); };
      last_in = db(se_in, static_cast<double>(n_in));
      last_out = db(se - se_in, static_cast<double>(screen.size() / 4 - n_in));
    }
    const double px = static_cast<double>(screen.size() / 4);
    last_exact = 100.0 * static_cast<double>(same) / px;
    const double mse = se / (px * 3);
    last_psnr = mse == 0 ? 99.0 : std::min(99.0, 10 * std::log10(255.0 * 255.0 / mse));
    last_q_t = t;
    if (!stretches.empty()) stretches.back().samples.push_back({t, last_exact});
    uint64_t t_px = 0, t_same = 0, cells = 0, c_exact = 0, c_sharp = 0, c_stale = 0, c_soft = 0, c_wrong = 0;
    uint64_t c_soft16 = 0, c_soft64 = 0;
    uint64_t c_px40 = 0, c_px30 = 0, c_ne = 0;
    double ne_db = 0;
    for (int32_t cy = 0; cy < rows; ++cy) {
      for (int32_t cx = 0; cx < cols; ++cx) {
        const size_t idx = static_cast<size_t>(cy) * cols + cx;
        if (!text_cell[idx]) continue;
        const Rect r = cell_rect(cx, cy);
        uint64_t c_same = 0;
        double c_se = 0;
        for (int32_t y = r.y; y < r.bottom(); ++y) {
          const size_t off = (static_cast<size_t>(y) * W + r.x) * 4;
          for (int32_t x = 0; x < r.w; ++x) {
            const size_t i = off + static_cast<size_t>(x) * 4;
            const int d0 = shown[i] - screen[i], d1 = shown[i + 1] - screen[i + 1], d2 = shown[i + 2] - screen[i + 2];
            c_same += (d0 | d1 | d2) == 0;
            c_se += d0 * d0 + d1 * d1 + d2 * d2;
          }
        }
        const uint64_t n = static_cast<uint64_t>(r.w) * r.h;
        ++cells;
        t_px += n;
        t_same += c_same;
        const bool close = c_same == n || 10 * std::log10(255.0 * 255.0 / (c_se / static_cast<double>(n * 3))) >= kSoftDb;
        // By pixels alone. An exact cell has no error (and an infinite PSNR):
        // it counts as within both and stays out of the mean.
        if (c_same == n) {
          ++c_px40;
          ++c_px30;
        } else {
          const double db = std::min(kCapDb, 10 * std::log10(255.0 * 255.0 / (c_se / static_cast<double>(n * 3))));
          c_px40 += db >= 40.0;
          c_px30 += db >= 30.0;
          ne_db += db;
          ++c_ne;
        }
        const bool stale = c_same != n && has_before[idx] && cell_equal(shown, before, r);
        const bool sharp = c_same != n && !stale && close && host->ShownQuality(r) == 0;
        if (c_same == n || sharp) {
          if (changed_at[idx] != 0) {
            to_exact_ms.push_back(static_cast<double>(t - changed_at[idx]) / 1e3);
            changed_at[idx] = 0;
          }
        }
        if (c_same == n) {
          ++c_exact;
        } else if (sharp) {
          ++c_sharp;
          sharp_se += c_se;
          sharp_n += static_cast<double>(n * 3);
        } else if (stale) {
          ++c_stale;
        } else if (close) {
          ++c_soft;
          soft_se += c_se;
          soft_n += static_cast<double>(n * 3);
          const int colours = cell_colours(r);
          c_soft16 += colours <= 16;
          c_soft64 += colours > 16 && colours <= 64;
        } else {
          ++c_wrong;
        }
      }
    }
    const double nc = static_cast<double>(std::max<uint64_t>(cells, 1));
    last_tshare = 100.0 * static_cast<double>(cells) / std::max<double>(1.0, static_cast<double>(cols) * rows);
    last_tpx = t_px ? 100.0 * static_cast<double>(t_same) / static_cast<double>(t_px) : 100.0;
    last_tcell = cells ? 100.0 * static_cast<double>(c_exact) / nc : 100.0;
    last_sharp = 100.0 * static_cast<double>(c_sharp) / nc;
    last_stale = 100.0 * static_cast<double>(c_stale) / nc;
    last_soft = 100.0 * static_cast<double>(c_soft) / nc;
    last_soft16 = 100.0 * static_cast<double>(c_soft16) / nc;
    last_soft64 = 100.0 * static_cast<double>(c_soft64) / nc;
    last_wrong = 100.0 * static_cast<double>(c_wrong) / nc;
    // No text cells: all of none is close, as for last_tcell.
    last_px40 = cells ? 100.0 * static_cast<double>(c_px40) / nc : 100.0;
    last_px30 = cells ? 100.0 * static_cast<double>(c_px30) / nc : 100.0;
    last_ne_db = ne_db;
    last_ne = static_cast<double>(c_ne);
  };
  auto submit = [&](uint64_t t, const std::vector<Rect>& dirty, bool resend) {
    const auto stats_before = host->stats();
    last_trace = {};
    host->OnFrame({{screen.data(), W, H, W * 4}, dirty, t});
    last_submit = t;
    measure(t);
    if (csv) {
      const auto a = host->stats();
      int64_t dpx = 0;
      for (const auto& r : dirty) dpx += static_cast<int64_t>(r.w) * r.h;
      std::fprintf(csv, "%.0f,%d,%lld,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%d,%d,%d,%d,%d,%.2f,%.2f\n", (t - first_t) / 1e3, resend ? 1 : 0,
                   static_cast<long long>(dpx), static_cast<unsigned long long>(a.video_bytes - stats_before.video_bytes),
                   static_cast<unsigned long long>(a.lossless_bytes - stats_before.lossless_bytes),
                   static_cast<unsigned long long>(a.lossless_tiles - stats_before.lossless_tiles),
                   static_cast<unsigned long long>(a.moved_tiles - stats_before.moved_tiles),
                   static_cast<unsigned long long>(a.cached_tiles - stats_before.cached_tiles),
                   static_cast<unsigned long long>(a.progressive_tiles + a.refreshed_tiles - stats_before.progressive_tiles -
                                                   stats_before.refreshed_tiles),
                   static_cast<unsigned long long>(a.video_frames - stats_before.video_frames), last_exact, last_tcell, last_sharp,
                   last_stale, last_soft, last_wrong, last_trace.hot, last_trace.hot_unmoved, last_trace.moved,
                   last_trace.mass_motion ? 1 : 0, last_trace.video_rects, last_px40, last_px30);
    }
  };

  bool attached = false;
  uint64_t clip_end = 0;
  while (clip.Next()) {
    if (!host) {
      W = clip.width();
      H = clip.height();
      first_t = clip.time_us();
      const auto fps = static_cast<int32_t>(args.num("fps", 60));
      codec::EncoderConfig ec{W, H, fps, bitrate, 24};
      auto enc = platform::CreateVideoEncoder(ec, &err);
      if (!enc) {
        std::fprintf(stderr, "encoder: %s\n", err.c_str());
        return 1;
      }
      session::HostOptions o;
      o.text_mode = text;
      o.context_tiles = text;
      o.context_tiles_64 = text;
      o.fps = fps;
      o.bitrate_bps = bitrate;
      SetExperiments(args, o);
      o.region_encoder_factory = [](const codec::EncoderConfig& c, std::string* e) {
        return platform::CreateVideoEncoder(c, e);
      };
      o.trace = [&](const session::FrameTrace& f) {
        last_trace = f;
        why.colours += f.video_colours;
        why.hot += f.video_hot;
        why.budget += f.video_budget;
        why.keyframe_first += f.video_keyframe_first;
        why.motion += f.video_motion;
        why.lossless += f.lossless_rects;
        if (f.picture_bytes == 0) return;
        ++why.pictures;
        const auto bytes = static_cast<double>(f.picture_bytes);
        if (f.key || f.sharpen) {
          why.b_key += bytes;
          return;
        }
        // A picture's bytes go to what it was coded for, by tiles.
        const double all = f.video_rects + f.refine_tiles + f.polish_tiles;
        if (all <= 0) {
          why.b_other += bytes;
          return;
        }
        why.b_new += bytes * f.video_rects / all;
        why.b_refine += bytes * f.refine_tiles / all;
        why.b_polish += bytes * f.polish_tiles / all;
        if (f.video_rects == 0) ++why.pictures_no_new;
      };
      host = std::make_unique<session::HostSession>(host_end.get(), std::move(enc), W, H, o);
    } else if (clip.width() != W || clip.height() != H) {
      std::fprintf(stderr, "the clip changes size at %.1f s: not supported, stopping there\n",
                   (clip.time_us() - first_t) / 1e6);
      break;
    }
    if (!attached) {
      viewer.Attach(viewer_end.get());
      attached = true;
    }
    // The Host's idle loop between changes.
    while (frames > 0 && clip.time_us() - last_submit >= kResubmitUs &&
           (host->keyframe_pending() || host->work_pending())) {
      submit(last_submit + kResubmitUs, {}, true);
      ++resent;
    }
    prev_screen.swap(screen);
    screen = clip.frame();
    new_screen(clip.time_us());
    stretches.push_back({clip.time_us(), {}});
    submit(clip.time_us(), clip.dirty(), false);
    ++frames;
    clip_end = clip.time_us();
  }
  if (!host) {
    std::fprintf(stderr, "no frames\n");
    return 1;
  }
  // What is still owed once the clip ends (up to 3 s).
  for (int k = 0; k < 30 && (host->keyframe_pending() || host->work_pending()); ++k) {
    submit(last_submit + kResubmitUs, {}, true);
    ++resent;
  }
  const uint64_t end_t = std::max(last_submit, clip_end) + kResubmitUs;
  if (last_q_t) {
    const double dt = static_cast<double>(end_t - last_q_t);
    w_exact += last_exact * dt;
    w_psnr += last_psnr * dt;
    w_in += last_in * dt;
    w_out += last_out * dt;
    w_tshare += last_tshare * dt;
    w_tpx += last_tpx * dt;
    w_tcell += last_tcell * dt;
    w_sharp += last_sharp * dt;
    w_stale += last_stale * dt;
    w_soft += last_soft * dt;
    w_soft16 += last_soft16 * dt;
    w_soft64 += last_soft64 * dt;
    w_wrong += last_wrong * dt;
    w_px40 += last_px40 * dt;
    w_px30 += last_px30 * dt;
    w_ne_db += last_ne_db * dt;
    w_ne += last_ne * dt;
    w_time += dt;
  }
  std::vector<double> pause_exact;
  for (size_t k = 0; k < stretches.size(); ++k) {
    const uint64_t next = k + 1 < stretches.size() ? stretches[k + 1].start : end_t;
    if (next - stretches[k].start < kPauseUs || stretches[k].samples.empty()) continue;
    pause_exact.push_back(stretches[k].samples.back().second);
  }
  double pe = 0;
  for (double v : pause_exact) pe += v;
  const auto s = host->stats();
  const double secs = static_cast<double>(end_t - first_t) / 1e6;
  std::printf("%-22s %-6s %6.1f s %5llu frames +%4llu re-sent | sent %8.0f kB %6.0f kbps (video %7.0f, lossless %7.0f kB) | "
              "exact %5.1f%% PSNR %5.1f | %zu pauses: exact %5.1f%%\n",
              label.c_str(), text ? "saver" : "normal", secs, static_cast<unsigned long long>(frames),
              static_cast<unsigned long long>(resent), s.bytes_sent / 1e3, s.bytes_sent * 8 / 1e3 / std::max(secs, 0.001),
              s.video_bytes / 1e3, s.lossless_bytes / 1e3, w_time > 0 ? w_exact / w_time : 0.0,
              w_time > 0 ? w_psnr / w_time : 0.0, pause_exact.size(), pause_exact.empty() ? 0.0 : pe / pause_exact.size());
  {
    std::sort(to_exact_ms.begin(), to_exact_ms.end());
    auto at = [&](double q) {
      return to_exact_ms.empty() ? 0.0 : to_exact_ms[static_cast<size_t>(q * static_cast<double>(to_exact_ms.size() - 1))];
    };
    uint64_t never = 0;
    for (const uint64_t c : changed_at) never += c != 0;
    const double wt = std::max(w_time, 1.0);
    std::printf("  text: %4.1f%% of the screen | cells, by time: exact %5.1f%%, sharp %5.1f%% (PSNR %.1f), as before the "
                "change %5.2f%%, soft %5.2f%% (PSNR %.1f; cells of up to 16 colours %.2f%%, up to 64 %.2f%%), wrong %5.2f%% | pixels exact "
                "%5.1f%%\n",
                w_tshare / wt, w_tcell / wt, w_sharp / wt,
                sharp_n > 0 && sharp_se > 0 ? 10 * std::log10(255.0 * 255.0 / (sharp_se / sharp_n)) : 99.0, w_stale / wt,
                w_soft / wt, soft_n > 0 && soft_se > 0 ? 10 * std::log10(255.0 * 255.0 / (soft_se / soft_n)) : 99.0, w_soft16 / wt,
                w_soft64 / wt, w_wrong / wt, w_tpx / wt);
    // The exact share is the one above (an exact cell is exact by pixels
    // alone), repeated so the line reads on its own. "-" for the mean when
    // every text cell was exact all the time.
    char ne_mean[16] = "-";
    if (w_ne > 0) std::snprintf(ne_mean, sizeof ne_mean, "%.1f", w_ne_db / w_ne);
    std::printf("  text by pixels alone: within 40 dB %5.1f%% (exact %5.1f%%), within 30 dB %5.1f%%, others' mean PSNR %s\n",
                w_px40 / wt, w_tcell / wt, w_px30 / wt, ne_mean);
    std::printf("  changed text cells until exact or sharp: %zu, median %.0f ms, 95%% %.0f ms, longest %.0f ms | changed again first: "
                "%llu (after %.0f ms on average) | not exact at the end: %llu\n",
                to_exact_ms.size(), at(0.5), at(0.95), at(1.0), static_cast<unsigned long long>(superseded),
                superseded ? superseded_ms / static_cast<double>(superseded) : 0.0, static_cast<unsigned long long>(never));
  }
  std::printf("  pictures: %llu (%llu with nothing new) | keyframes %.0f kB, new content %.0f, refinement %.0f, still polish "
              "%.0f, other %.0f kB\n",
              static_cast<unsigned long long>(why.pictures), static_cast<unsigned long long>(why.pictures_no_new),
              why.b_key / 1e3, why.b_new / 1e3, why.b_refine / 1e3, why.b_polish / 1e3, why.b_other / 1e3);
  std::printf("  rects: %llu lossless | to video: %llu too many colours, %llu too many for content that keeps changing, "
              "%llu over the lossless budget, %llu in motion (saver), %llu video first with a keyframe\n",
              static_cast<unsigned long long>(why.lossless), static_cast<unsigned long long>(why.colours),
              static_cast<unsigned long long>(why.hot), static_cast<unsigned long long>(why.budget),
              static_cast<unsigned long long>(why.motion), static_cast<unsigned long long>(why.keyframe_first));
  // --cells=1: the text cells that are not exact or sharp when the clip
  // ends, to find out why.
  if (args.num("cells", 0) != 0 && shown.size() == screen.size()) {
    for (int32_t cy = 0; cy < rows; ++cy) {
      for (int32_t cx = 0; cx < cols; ++cx) {
        if (!text_cell[static_cast<size_t>(cy) * cols + cx]) continue;
        const Rect r = cell_rect(cx, cy);
        double se = 0;
        uint64_t same = 0;
        for (int32_t y = r.y; y < r.bottom(); ++y) {
          for (int32_t x = r.x; x < r.right(); ++x) {
            const size_t i = (static_cast<size_t>(y) * W + x) * 4;
            const int d0 = shown[i] - screen[i], d1 = shown[i + 1] - screen[i + 1], d2 = shown[i + 2] - screen[i + 2];
            same += (d0 | d1 | d2) == 0;
            se += d0 * d0 + d1 * d1 + d2 * d2;
          }
        }
        const double n = static_cast<double>(r.w) * r.h;
        if (same == static_cast<uint64_t>(n)) continue;
        const double db = 10 * std::log10(255.0 * 255.0 / (se / (n * 3)));
        const int q = host->ShownQuality(r);
        if (q == 0 && db >= kSoftDb) continue;
        std::printf("  cell %4d,%4d  PSNR %5.1f  %s  %d colours%s\n", r.x, r.y, db,
                    q == 0 ? "lossless" : q == 1 ? "polished video" : "video", cell_colours(r), db < kSoftDb ? "  WRONG" : "");
      }
    }
  }
  if (audit && shown.size() == screen.size()) {
    // At the end: the cells still contradicting the host, and text left at
    // video quality with nothing owed for it.
    uint64_t still = 0, text_video = 0, text_video_finished = 0, below25 = 0, below20 = 0;
    for (int32_t cy = 0; cy < rows; ++cy) {
      for (int32_t cx = 0; cx < cols; ++cx) {
        const size_t idx = static_cast<size_t>(cy) * cols + cx;
        const Rect r = cell_rect(cx, cy);
        double db = 0;
        int q = 0;
        const int kind = audit_kind(r, db, q);
        if (kind != 0) {
          ++still;
          std::printf("  audit at the end: cell %4d,%4d %s, PSNR %5.1f, since %.0f ms%s\n", r.x, r.y,
                      kind == 1 ? "lossless by the host's account" : "finished video", db,
                      bad_since.size() > idx && bad_since[idx] ? (bad_since[idx] - first_t) / 1e3 : -1.0,
                      text_cell[idx] ? "  (text)" : "");
        }
        below25 += db < 25.0;
        below20 += db < 20.0;
        if (text_cell[idx] && q != 0 && db < 35.0) {
          ++text_video;
          text_video_finished += q == 1;
        }
      }
    }
    std::printf("  audit: %llu cells turned bad during the clip, %llu at the end | cells below 25 dB at the end: %llu (below 20: "
                "%llu), whatever the host's account | text cells at video quality at the end: %llu "
                "(%llu of them finished, nothing owed) | work pending at the end: %s\n",
                static_cast<unsigned long long>(bad_events), static_cast<unsigned long long>(still),
                static_cast<unsigned long long>(below25), static_cast<unsigned long long>(below20),
                static_cast<unsigned long long>(text_video), static_cast<unsigned long long>(text_video_finished),
                host->work_pending() || host->keyframe_pending() ? "yes" : "no");
    // --dump=PREFIX: the screen, what the viewer shows, and a map of the
    // host's account per cell (green lossless, blue finished video, red
    // other video, magenta contradicted), as PPM files. They show the real
    // screen: keep them on this machine (docs/clips.md, privacy).
    const std::string dump = args.str("dump");
    if (!dump.empty()) {
      auto write = [&](const std::string& path, const std::vector<uint8_t>& px) {
        std::FILE* f = std::fopen(path.c_str(), "wb");
        if (!f) return;
        std::fprintf(f, "P6 %d %d 255\n", W, H);
        for (size_t k = 0; k < px.size(); k += 4) {
          const uint8_t rgb[3] = {px[k + 2], px[k + 1], px[k]};
          std::fwrite(rgb, 1, 3, f);
        }
        std::fclose(f);
      };
      write(dump + "-screen.ppm", screen);
      write(dump + "-viewer.ppm", shown);
      std::vector<uint8_t> map(screen.size());
      for (int32_t cy = 0; cy < rows; ++cy) {
        for (int32_t cx = 0; cx < cols; ++cx) {
          const Rect r = cell_rect(cx, cy);
          double db = 0;
          int q = 0;
          const int kind = audit_kind(r, db, q);
          const uint8_t tint[4][3] = {{0, 160, 0}, {0, 0, 200}, {200, 0, 0}, {255, 0, 255}};
          const uint8_t* c = tint[kind != 0 ? 3 : q];
          for (int32_t y = r.y; y < r.bottom(); ++y) {
            for (int32_t x = r.x; x < r.right(); ++x) {
              const size_t i = (static_cast<size_t>(y) * W + x) * 4;
              // BGRA: half the screen's grey, half the tint.
              const int g = (77 * screen[i + 2] + 150 * screen[i + 1] + 29 * screen[i]) >> 9;
              map[i] = static_cast<uint8_t>(g + c[2] / 2);
              map[i + 1] = static_cast<uint8_t>(g + c[1] / 2);
              map[i + 2] = static_cast<uint8_t>(g + c[0] / 2);
              map[i + 3] = 255;
            }
          }
        }
      }
      write(dump + "-map.ppm", map);
    }
  }
  if (focus.w > 0 && w_time > 0) std::printf("  PSNR inside the focus %.1f, outside %.1f\n", w_in / w_time, w_out / w_time);
  std::printf("  video areas: %llu opened, %llu closed, %.0f kB in their streams\n",
              static_cast<unsigned long long>(s.regions_opened), static_cast<unsigned long long>(s.regions_closed),
              s.region_bytes / 1e3);
  if (missed > 0) {
    std::printf("  (%llu frames without a picture of their own on the viewer; measured on what it showed)\n",
                static_cast<unsigned long long>(missed));
  }
  if (csv) std::fclose(csv);
  viewer.Stop();
  return 0;
}

// decisions: everything the host decides on a clip, as a file two builds can
// be compared by. The platform's encoder cannot be used for that: its
// pictures differ from run to run, and what the host does next depends on
// them (their sizes, the pictures it drops).

// 64-bit FNV-1a over 8-byte words, with splitmix64's finalizer: the same
// value on every run (no seed, no addresses), and fast enough for a
// full-screen picture per frame.
class Hash64 {
 public:
  void Word(uint64_t w) { h_ = (h_ ^ w) * 0x100000001b3ull; }
  // Whole words, then what is left in one word with zeros above it (callers
  // mix in the length where it is not fixed).
  void Bytes(const uint8_t* p, size_t n) {
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
      uint64_t w;
      std::memcpy(&w, p + i, 8);
      Word(w);
    }
    if (i < n) {
      uint64_t w = 0;
      std::memcpy(&w, p + i, n - i);
      Word(w);
    }
  }
  uint64_t Final() const {
    uint64_t z = h_;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
  }

 private:
  uint64_t h_ = 0xcbf29ce484222325ull;
};

// The encoder of `decisions`, full screen and video areas alike: a picture
// depends only on the input and the call (force_keyframe, the QP floor last
// asked for), never on time, addresses or the calls before, so a run repeats
// exactly. Its size varies with the content, as a real picture's does, and
// some pictures are dropped as VideoToolbox drops them at low rates, so the
// host's paths for those (the video tiles carried to the next picture) are
// taken too.
class DeterministicEncoder final : public codec::IHevcEncoder {
 public:
  explicit DeterministicEncoder(const codec::EncoderConfig& config) : config_(config) {}
  std::string name() const override { return "deterministic fake (rdaccel_clip decisions)"; }
  const std::string& last_error() const override { return err_; }
  void SetBitrate(int64_t bps) override { config_.bitrate_bps = bps; }  // kept, not used
  void SetMinQp(int qp) override { min_qp_ = qp; }
  // --perturb: the next picture fails, as on an encoder that broke.
  void FailNext() { fail_next_ = true; }

  std::optional<codec::HevcAccessUnit> Encode(const tile::ImageView& img, uint64_t, bool force_keyframe) override {
    err_.clear();
    if (fail_next_) {
      fail_next_ = false;
      err_ = "the encoder failed (--perturb)";
      return std::nullopt;
    }
    Hash64 h;
    h.Word(static_cast<uint64_t>(img.width));
    h.Word(static_cast<uint64_t>(img.height));
    const size_t row = static_cast<size_t>(img.width) * 4;
    for (int32_t y = 0; y < img.height; ++y) h.Bytes(img.data + static_cast<size_t>(y) * img.stride, row);
    h.Word(force_keyframe ? 1 : 0);
    h.Word(static_cast<uint64_t>(static_cast<int64_t>(min_qp_)));
    const uint64_t hash = h.Final();
    if (!force_keyframe && hash % 29 == 0) return std::nullopt;  // dropped: no error
    codec::HevcAccessUnit au;
    au.keyframe = force_keyframe;
    if (force_keyframe) au.parameter_sets = {std::vector<uint8_t>{static_cast<uint8_t>(codec::kNalVps << 1), 1}};
    std::vector<uint8_t> nal = {static_cast<uint8_t>((force_keyframe ? codec::kNalIdrWRadl : 1) << 1), 1};
    for (int k = 0; k < 8; ++k) nal.push_back(static_cast<uint8_t>(hash >> (8 * k)));
    nal.resize(nal.size() + hash % 3000, 0x55);
    codec::AppendLengthPrefixed(nal, au.nal_data);
    return au;
  }

 private:
  codec::EncoderConfig config_;
  int min_qp_ = 0;
  bool fail_next_ = false;
  std::string err_;
};

// The transport of `decisions`: keeps what the host sends on each channel
// until it is taken (once per call). No viewer, so nothing comes back: no
// keyframe requests, no input.
class RecordingTransport final : public transport::ITransport {
 public:
  bool Send(transport::Channel ch, std::span<const uint8_t> bytes) override {
    std::lock_guard<std::mutex> lock(mu_);
    Sent& s = sent_[static_cast<size_t>(ch) % sent_.size()];
    s.used = true;
    s.bytes.insert(s.bytes.end(), bytes.begin(), bytes.end());
    return true;
  }
  void SetReceiver(ReceiveFn) override {}
  void Close() override {}

  // " video=1234:0123456789abcdef" for each channel sent on since the last
  // Take: how many bytes, and their hash in the order sent.
  std::string Take() {
    static const char* const kNames[] = {"control", "input", "surface", "video", "cursor", "message", "bulk"};
    static_assert(static_cast<int>(std::size(kNames)) == kChannelCount);
    std::lock_guard<std::mutex> lock(mu_);
    std::string out;
    for (size_t c = 0; c < sent_.size(); ++c) {
      Sent& s = sent_[c];
      if (!s.used) continue;
      Hash64 h;
      h.Bytes(s.bytes.data(), s.bytes.size());
      h.Word(s.bytes.size());
      char buf[80];
      std::snprintf(buf, sizeof buf, " %s=%zu:%016llx", kNames[c], s.bytes.size(),
                    static_cast<unsigned long long>(h.Final()));
      out += buf;
      s.used = false;
      s.bytes.clear();
    }
    return out;
  }

 private:
  struct Sent {
    bool used = false;
    std::vector<uint8_t> bytes;
  };
  std::mutex mu_;
  std::array<Sent, kChannelCount> sent_;
};

int Decisions(const Args& args) {
  ClipReader clip(args.positional().c_str());
  if (!clip.ok()) {
    std::fprintf(stderr, "cannot read %s\n", args.positional().c_str());
    return 1;
  }
  const std::string out_path = args.str("out");
  if (out_path.empty()) {
    std::fprintf(stderr, "decisions needs --out=FILE\n");
    return 2;
  }
  std::FILE* out = std::fopen(out_path.c_str(), "w");
  if (!out) {
    std::fprintf(stderr, "cannot write %s\n", out_path.c_str());
    return 1;
  }
  const bool text = args.num("text", 0) != 0;
  const int64_t bitrate = args.num("bitrate", 8'000'000);
  const auto fps = static_cast<int32_t>(args.num("fps", 60));
  const bool perturb = args.num("perturb", 0) != 0;
  constexpr uint64_t kResubmitUs = 100'000;  // as the Host's idle loop (and eval)

  // (Before the host, which keeps references to them.)
  RecordingTransport transport;
  std::vector<session::FrameTrace> traces;  // what the call in progress reported
  DeterministicEncoder* encoder = nullptr;  // the full-screen one; the host owns it
  std::unique_ptr<session::HostSession> host;
  std::vector<uint8_t> screen;
  int32_t W = 0, H = 0;
  uint64_t first_t = 0, last_submit = 0, frames = 0, calls = 0, resent = 0;

  // --perturb=1: calls the replay alone never makes (the Host makes them on
  // a real screen), at fixed points of the clip's own time from its first
  // frame. Each goes just before the first frame submitted at or after its
  // point (a re-submit too); points due together go in time order, and in
  // the order of this table at the same time.
  enum Kind { kKeyframe, kBitrate, kEncoderFails, kTextModeOffOn };
  struct Point {
    Kind kind;
    uint64_t first_us, every_us;
    int times;  // -1 = for as long as the clip lasts
    int done = 0;
    uint64_t next_us() const { return first_us + static_cast<uint64_t>(done) * every_us; }
  };
  std::vector<Point> points;
  if (perturb) {
    points = {{kKeyframe, 5'000'000, 10'000'000, -1},
              {kBitrate, 3'000'000, 7'000'000, -1},  // bitrate, then /2, /4, and again
              {kEncoderFails, 15'000'000, 0, 1},
              {kTextModeOffOn, 20'000'000, 2'000'000, text ? 2 : 0}};
  }
  auto apply_points = [&](uint64_t rel) {
    for (;;) {
      Point* due = nullptr;
      for (Point& p : points) {
        if ((p.times < 0 || p.done < p.times) && p.next_us() <= rel && (!due || p.next_us() < due->next_us())) due = &p;
      }
      if (!due) return;
      std::fprintf(out, "perturb at=%llus call=%llu t_us=%llu", static_cast<unsigned long long>(due->next_us() / 1'000'000),
                   static_cast<unsigned long long>(calls), static_cast<unsigned long long>(rel));
      switch (due->kind) {
        case kKeyframe:
          host->RequestKeyframe();
          std::fprintf(out, " keyframe\n");
          break;
        case kBitrate: {
          const int64_t cycle[3] = {bitrate, bitrate / 2, bitrate / 4};
          host->SetBitrate(cycle[due->done % 3]);
          std::fprintf(out, " bitrate=%lld\n", static_cast<long long>(cycle[due->done % 3]));
          break;
        }
        case kEncoderFails:
          encoder->FailNext();
          std::fprintf(out, " encoder_fails\n");
          break;
        case kTextModeOffOn:
          host->SetTextMode(false);
          host->SetTextMode(true);
          std::fprintf(out, " text_mode=off,on\n");
          break;
      }
      ++due->done;
    }
  };

  auto submit = [&](uint64_t t, const std::vector<Rect>& dirty, bool resend) {
    const uint64_t rel = t - first_t;
    apply_points(rel);
    // Nothing is sent between calls (the Hello went with "init"); should
    // anything be, it is not lost.
    const std::string between = transport.Take();
    if (!between.empty()) std::fprintf(out, "between%s\n", between.c_str());
    traces.clear();
    const bool ok = host->OnFrame({{screen.data(), W, H, W * 4}, dirty, t});
    last_submit = t;
    std::fprintf(out, "call=%llu t_us=%llu resent=%d ok=%d%s", static_cast<unsigned long long>(calls),
                 static_cast<unsigned long long>(rel), resend ? 1 : 0, ok ? 1 : 0, transport.Take().c_str());
    if (traces.empty()) std::fprintf(out, " trace=none");
    for (const auto& f : traces) {
      std::fprintf(out,
                   " trace frame_no=%llu key=%d still=%d sharpen=%d picture_bytes=%zu video_rects=%d video_colours=%d "
                   "video_hot=%d video_budget=%d video_keyframe_first=%d video_motion=%d refine_tiles=%d polish_tiles=%d "
                   "lossless_rects=%d hot=%d hot_unmoved=%d moved=%d mass_motion=%d",
                   static_cast<unsigned long long>(f.frame_no), f.key ? 1 : 0, f.still ? 1 : 0, f.sharpen ? 1 : 0,
                   f.picture_bytes, f.video_rects, f.video_colours, f.video_hot, f.video_budget, f.video_keyframe_first,
                   f.video_motion, f.refine_tiles, f.polish_tiles, f.lossless_rects, f.hot, f.hot_unmoved, f.moved,
                   f.mass_motion ? 1 : 0);
    }
    std::fprintf(out, " work_pending=%d keyframe_pending=%d\n", host->work_pending() ? 1 : 0,
                 host->keyframe_pending() ? 1 : 0);
    ++calls;
  };

  // As eval plays the clip, without the viewer.
  while (clip.Next()) {
    if (!host) {
      W = clip.width();
      H = clip.height();
      first_t = clip.time_us();
      session::HostOptions o;
      o.text_mode = text;
      o.context_tiles = text;
      o.context_tiles_64 = text;
      o.fps = fps;
      o.bitrate_bps = bitrate;
      SetExperiments(args, o);
      o.region_encoder_factory = [](const codec::EncoderConfig& c, std::string*) -> std::unique_ptr<codec::IHevcEncoder> {
        return std::make_unique<DeterministicEncoder>(c);
      };
      // What each frame went through, as eval has it. Tracing costs the host
      // one more palette search per rect it codes as video, which must not
      // change what it decides.
      o.trace = [&](const session::FrameTrace& f) { traces.push_back(f); };
      // format: raised whenever the lines change, so that a log recorded by
      // an older rdaccel_clip differs on its first line.
      std::fprintf(out,
                   "decisions format=1 size=%dx%d text=%d bitrate=%lld fps=%d perturb=%d split_cells=%d split_hot=%d "
                   "text_refine=%d text_linger_us=%lld refine_frames=%d still_refine_frames=%d\n",
                   W, H, text ? 1 : 0, static_cast<long long>(bitrate), fps, perturb ? 1 : 0, o.split_cells,
                   o.split_hot ? 1 : 0, o.text_refine ? 1 : 0, static_cast<long long>(o.text_linger_us), o.refine_frames,
                   o.still_refine_frames);
      auto enc = std::make_unique<DeterministicEncoder>(codec::EncoderConfig{W, H, fps, bitrate, 24});
      encoder = enc.get();
      host = std::make_unique<session::HostSession>(&transport, std::move(enc), W, H, o);
      std::fprintf(out, "init%s\n", transport.Take().c_str());
    } else if (clip.width() != W || clip.height() != H) {
      std::fprintf(stderr, "the clip changes size at %.1f s: not supported, stopping there\n",
                   (clip.time_us() - first_t) / 1e6);
      std::fprintf(out, "stopped t_us=%llu: the clip changes size\n", static_cast<unsigned long long>(clip.time_us() - first_t));
      break;
    }
    // The Host's idle loop between changes.
    while (frames > 0 && clip.time_us() - last_submit >= kResubmitUs &&
           (host->keyframe_pending() || host->work_pending())) {
      submit(last_submit + kResubmitUs, {}, true);
      ++resent;
    }
    screen = clip.frame();
    submit(clip.time_us(), clip.dirty(), false);
    ++frames;
  }
  if (!host) {
    std::fprintf(stderr, "no frames\n");
    std::fclose(out);
    return 1;
  }
  // What is still owed once the clip ends (up to 3 s).
  for (int k = 0; k < 30 && (host->keyframe_pending() || host->work_pending()); ++k) {
    submit(last_submit + kResubmitUs, {}, true);
    ++resent;
  }
  // The counters HostSession keeps, without its three timings (*_us_total).
  // The rest of HostStats is the Host's (re-submits on a wall-clock timer,
  // the transport's measurements and the bitrate that follows them, the
  // cursor, sound) and stays 0 with a HostSession alone.
  const auto s = host->stats();
  const std::pair<const char*, uint64_t> counters[] = {
      {"frames_captured", s.frames_captured},   {"frames_skipped", s.frames_skipped},
      {"frames_encoded", s.frames_encoded},     {"video_frames", s.video_frames},
      {"keyframes", s.keyframes},               {"keyframe_requests", s.keyframe_requests},
      {"encode_errors", s.encode_errors},       {"encoder_drops", s.encoder_drops},
      {"bytes_sent", s.bytes_sent},             {"video_bytes", s.video_bytes},
      {"lossless_bytes", s.lossless_bytes},     {"lossless_tiles", s.lossless_tiles},
      {"moved_tiles", s.moved_tiles},           {"cached_tiles", s.cached_tiles},
      {"refreshed_tiles", s.refreshed_tiles},   {"progressive_tiles", s.progressive_tiles},
      {"region_pictures", s.region_pictures},   {"region_bytes", s.region_bytes},
      {"regions_opened", s.regions_opened},     {"regions_closed", s.regions_closed},
  };
  std::fprintf(out, "stats");
  for (const auto& [name, v] : counters) std::fprintf(out, " %s=%llu", name, static_cast<unsigned long long>(v));
  std::fprintf(out, "\n");
  const bool written = std::ferror(out) == 0;
  if (std::fclose(out) != 0 || !written) {
    std::fprintf(stderr, "cannot write %s\n", out_path.c_str());
    return 1;
  }
  std::printf("%llu calls, %llu of them re-sent\n", static_cast<unsigned long long>(calls),
              static_cast<unsigned long long>(resent));
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) return Usage();
  const std::string cmd = argv[1];
  const Args args(argc - 1, argv + 1);
  if (args.positional().empty()) return Usage();
  if (cmd == "record") return Record(args);
  if (cmd == "render") return Render(args);
  if (cmd == "eval") return Eval(args);
  if (cmd == "frame") return Frame(args);
  if (cmd == "decisions") return Decisions(args);
  return Usage();
}
