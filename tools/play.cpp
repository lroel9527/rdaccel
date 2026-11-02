// rdaccel_play: decode an .rdstream file and report what came out.
//
//   rdaccel_play IN.rdstream [--check-synthetic] [--dump=DIR] [--dump-every=60]
//
// --check-synthetic regenerates the synthetic source frame for every decoded
// frame and prints the PSNR, which proves a stream recorded on one platform
// decodes correctly on the other. --dump writes decoded frames as .ppm files.
// Exit code is non-zero if any frame failed to decode or PSNR is too low.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "args.h"
#include "capture/synthetic_capturer.h"
#include "io/stream_file.h"
#include "platform.h"
#include "session/viewer_session.h"

using namespace rdaccel;

namespace {

// PSNR over the RGB channels (alpha ignored).
double Psnr(const tile::ImageView& a, const std::vector<uint8_t>& b) {
  double se = 0;
  for (int32_t y = 0; y < a.height; ++y) {
    const uint8_t* pa = a.data + static_cast<size_t>(y) * a.stride;
    const uint8_t* pb = b.data() + static_cast<size_t>(y) * a.width * 4;
    for (int32_t x = 0; x < a.width * 4; ++x) {
      if ((x & 3) == 3) continue;
      const double d = static_cast<double>(pa[x]) - pb[x];
      se += d * d;
    }
  }
  const double mse = se / (static_cast<double>(a.width) * a.height * 3);
  return mse == 0 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
}

void WritePpm(const std::string& path, const tile::ImageView& img) {
  std::FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return;
  std::fprintf(f, "P6\n%d %d\n255\n", img.width, img.height);
  std::vector<uint8_t> row(static_cast<size_t>(img.width) * 3);
  for (int32_t y = 0; y < img.height; ++y) {
    const uint8_t* p = img.data + static_cast<size_t>(y) * img.stride;
    for (int32_t x = 0; x < img.width; ++x) {
      row[x * 3 + 0] = p[x * 4 + 2];
      row[x * 3 + 1] = p[x * 4 + 1];
      row[x * 3 + 2] = p[x * 4 + 0];
    }
    std::fwrite(row.data(), 1, row.size(), f);
  }
  std::fclose(f);
}

}  // namespace

int main(int argc, char** argv) {
  tools::Args args(argc, argv);
  std::string in = args.str("in", args.positional());
  if (in.empty()) {
    std::fprintf(stderr, "usage: rdaccel_play FILE [--check-synthetic] [--dump=DIR]\n");
    return 2;
  }
  const bool check = args.has("check-synthetic");
  const std::string dump = args.str("dump");
  const auto dump_every = static_cast<uint64_t>(std::max(1LL, args.num("dump-every", 60)));
  const double min_psnr = static_cast<double>(args.num("min-psnr", 30));

  std::vector<uint8_t> bytes;
  std::string err;
  if (!io::ReadStreamFile(in, bytes, &err)) {
    std::fprintf(stderr, "%s\n", err.c_str());
    return 1;
  }
  auto dec = platform::CreateHevcDecoder(&err);
  if (!dec) {
    std::fprintf(stderr, "%s\n", err.c_str());
    return 1;
  }
  const std::string dec_name = dec->name();

  session::ViewerSession viewer(nullptr, std::move(dec));
  proto::Hello hello;
  viewer.SetHelloHandler([&](const proto::Hello& h) { hello = h; });

  std::vector<uint8_t> ref;
  double psnr_sum = 0, psnr_min = 1e9;
  uint64_t frames = 0, keyframes_seen = 0;
  std::vector<uint64_t> region_counts;
  viewer.SetFrameHandler([&](const tile::ImageView& img, const std::vector<Rect>& regions,
                             uint64_t frame_id, uint64_t) {
    ++frames;
    if (regions.empty()) ++keyframes_seen;
    region_counts.push_back(regions.size());
    if (check) {
      // Synthetic recordings never skip frames: frame_id N is source frame N-1.
      // An empty buffer forces a full, from-scratch render of that frame.
      ref.clear();
      capture::RenderSyntheticFrame(frame_id - 1, img.width, img.height, ref);
      const double p = Psnr(img, ref);
      psnr_sum += p;
      psnr_min = std::min(psnr_min, p);
    }
    if (!dump.empty() && (frames == 1 || frames % dump_every == 0)) {
      char name[64];
      std::snprintf(name, sizeof(name), "/frame_%06llu.ppm",
                    static_cast<unsigned long long>(frame_id));
      WritePpm(dump + name, img);
    }
  });
  viewer.FeedVideoBytes(bytes);

  const auto st = viewer.stats();
  std::printf("platform        : %s\n", platform::PlatformName());
  std::printf("decoder         : %s\n", dec_name.c_str());
  std::printf("stream          : %ux%u, %zu bytes\n", hello.width, hello.height, bytes.size());
  std::printf("frames decoded  : %llu (%llu full-screen)\n", static_cast<unsigned long long>(frames),
              static_cast<unsigned long long>(keyframes_seen));
  std::printf("decode errors   : %llu\n", static_cast<unsigned long long>(st.decode_errors));
  if (st.frames_decoded > 0) {
    std::printf("decode time     : %.2f ms/frame avg\n",
                st.decode_us_total / 1000.0 / static_cast<double>(st.frames_decoded));
  }
  bool ok = st.decode_errors == 0 && frames > 0;
  if (check && frames > 0) {
    std::printf("PSNR vs source  : avg %.2f dB, min %.2f dB\n", psnr_sum / frames, psnr_min);
    ok = ok && psnr_min >= min_psnr;
  }
  std::printf("result          : %s\n", ok ? "OK" : "FAIL");
  return ok ? 0 : 1;
}
