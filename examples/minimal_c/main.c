/* The smallest complete rdaccel program in plain C: a host and a viewer in
 * one process, connected by an in-process pipe. The host is fed synthetic
 * frames; the viewer counts what it decodes. Across machines, replace
 * rda_pipe_create() with rda_udp_listen() on the host and rda_udp_connect()
 * on the viewer.
 *
 * Exits 0 when every frame arrived. */
#define _POSIX_C_SOURCE 199309L
#include <rdaccel/rdaccel.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { kWidth = 640, kHeight = 360, kFrames = 30 };

typedef struct {
  int frames;
  int width, height;
  uint64_t bytes_changed;
} viewer_state;

static void on_stream_info(void* user, int32_t width, int32_t height) {
  viewer_state* s = (viewer_state*)user;
  s->width = width;
  s->height = height;
}

static void on_frame(void* user, const rda_image* bgra, const rda_rect* changed, size_t n,
                     const rda_frame_info* info) {
  viewer_state* s = (viewer_state*)user;
  size_t i;
  (void)bgra;
  (void)info;
  for (i = 0; i < n; ++i) s->bytes_changed += (uint64_t)changed[i].w * changed[i].h * 4;
  s->frames++;
}

static void on_log(void* user, rda_log_level level, const char* message) {
  (void)user;
  if (level <= RDA_LOG_WARN) fprintf(stderr, "rdaccel: %s\n", message);
}

static int check(rda_status s, const char* what) {
  if (s == RDA_OK) return 1;
  fprintf(stderr, "%s failed: %s (%s)\n", what, rda_status_str(s), rda_last_error());
  return 0;
}

int main(void) {
  rda_transport *host_end = NULL, *viewer_end = NULL;
  rda_host_config hc;
  rda_viewer_config vc;
  rda_host* host = NULL;
  rda_viewer* viewer = NULL;
  viewer_state got;
  rda_host_stats hs;
  uint8_t* px;
  int i, ok = 0;
  char encoder[64];

  printf("rdaccel %s on %s\n", rda_version_string(), rda_platform_name());
  rda_set_log(on_log, NULL, RDA_LOG_WARN);
  memset(&got, 0, sizeof(got));

  if (!check(rda_pipe_create(&host_end, &viewer_end), "rda_pipe_create")) return 1;

  rda_viewer_config_default(&vc);
  vc.user = &got;
  vc.on_stream_info = on_stream_info;
  vc.on_frame = on_frame;
  if (!check(rda_viewer_create(viewer_end, &vc, &viewer), "rda_viewer_create")) {
    rda_transport_destroy(host_end);
    return 1;
  }

  rda_host_config_default(&hc);
  hc.fps = 30;
  hc.bitrate_bps = 2000000;
  if (!check(rda_host_create(host_end, &hc, &host), "rda_host_create")) goto done;

  px = (uint8_t*)malloc((size_t)kWidth * kHeight * 4);
  if (!px) goto done;
  for (i = 0; i < kFrames; ++i) {
    rda_rect dirty[16];
    size_t n = 0;
    rda_image img;
    if (!check(rda_test_pattern_render((uint64_t)i, kWidth, kHeight, px, i > 0, dirty, 16, &n),
               "rda_test_pattern_render")) {
      break;
    }
    img.data = px;
    img.width = kWidth;
    img.height = kHeight;
    img.stride = kWidth * 4;
    if (!check(rda_host_submit_frame(host, &img, rda_now_us(), dirty, n < 16 ? n : 0),
               "rda_host_submit_frame")) {
      break;
    }
  }
  free(px);

  hs.struct_size = sizeof(hs);
  rda_host_get_stats(host, &hs);
  rda_host_encoder_name(host, encoder, sizeof(encoder));
  printf("encoder %s: %llu frames, %llu bytes\n", encoder, (unsigned long long)hs.frames_encoded,
         (unsigned long long)hs.bytes_sent);

  /* Frames are decoded on the pipe's thread; wait for them (up to 5 s). */
  for (i = 0; i < 500; ++i) {
    rda_viewer_stats vs;
    struct timespec ts = {0, 10 * 1000 * 1000};
    vs.struct_size = sizeof(vs);
    rda_viewer_get_stats(viewer, &vs);
    if (vs.frames_decoded >= kFrames) break;
    nanosleep(&ts, NULL);
  }

done:
  /* After destroy no callback runs, so `got` can be read without locking. */
  rda_host_destroy(host);
  rda_viewer_destroy(viewer);
  printf("viewer: %dx%d, %d frames\n", got.width, got.height, got.frames);
  ok = got.frames == kFrames && got.width == kWidth && got.height == kHeight;
  printf("%s\n", ok ? "OK" : "FAIL");
  return ok ? 0 : 1;
}
