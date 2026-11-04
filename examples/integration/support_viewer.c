/* support_viewer: the viewer half of the remote-support example (see
 * support_host.c). Plain C on <rdaccel/rdaccel.h>, built against the
 * installed library.
 *
 *   - connects over UDP with the shared key and the pairing code, and
 *     retries with a fresh viewer while the host is not there yet;
 *   - keeps its own copy of the remote screen, updated from the changed
 *     areas of each picture, with the cursor drawn on top at the end;
 *   - sends some input (pointer moves, a click, typed text), the clipboard
 *     and application pings, and reads the host's session info;
 *   - after --seconds, writes the last picture to --snapshot (PPM) and
 *     prints what it saw.
 *
 *   support_viewer --key-file=PATH [--host=127.0.0.1] [--port=7300]
 *                  [--pair-code=CODE] [--seconds=5] [--snapshot=out.ppm]
 *                  [--retries=10] [--no-input]
 *
 * Exits 0 when it was connected and decoded at least one picture.
 */
#include "common.h"

typedef struct {
  session_events events;
  rda_viewer* viewer;

  /* Written on the decode thread, read by main after the viewer is gone. */
  uint8_t* screen;
  int32_t width, height;
  unsigned long frames, changed_rects;
  uint64_t first_frame_us;

  /* Cursor: transport thread. */
  pthread_mutex_t cursor_mu;
  int32_t cx, cy, cvisible;
  uint8_t* cursor_bgra;
  rda_cursor_image cursor;
  unsigned long cursor_updates;

  /* Messages: transport thread. */
  pthread_mutex_t msg_mu;
  char session_info[256];
  uint64_t ping_rtt_us;
} viewer_app;

static void on_stream_info(void* user, int32_t width, int32_t height) {
  viewer_app* app = (viewer_app*)user;
  free(app->screen);
  app->screen = (uint8_t*)calloc((size_t)width * height, 4);
  app->width = width;
  app->height = height;
  printf("stream %dx%d\n", width, height);
  fflush(stdout);
}

static void on_frame(void* user, const rda_image* img, const rda_rect* changed, size_t n,
                     const rda_frame_info* info) {
  viewer_app* app = (viewer_app*)user;
  rda_rect all;
  size_t i;
  int32_t y;
  (void)info;
  if (!app->screen || img->width != app->width || img->height != app->height) return;
  if (n == 0) {
    all.x = all.y = 0;
    all.w = img->width;
    all.h = img->height;
    changed = &all;
    n = 1;
  }
  /* What a texture upload would do: copy only what changed. */
  for (i = 0; i < n; ++i) {
    const rda_rect* r = &changed[i];
    for (y = r->y; y < r->y + r->h; ++y) {
      memcpy(app->screen + ((size_t)y * app->width + r->x) * 4,
             img->data + (size_t)y * img->stride + (size_t)r->x * 4, (size_t)r->w * 4);
    }
  }
  if (app->frames++ == 0) app->first_frame_us = rda_now_us();
  app->changed_rects += n;
}

static void on_cursor(void* user, const rda_cursor_state* c) {
  viewer_app* app = (viewer_app*)user;
  pthread_mutex_lock(&app->cursor_mu);
  app->cx = c->x;
  app->cy = c->y;
  app->cvisible = c->visible;
  if (c->image_changed && c->image) {
    size_t bytes = (size_t)c->image->width * c->image->height * 4;
    free(app->cursor_bgra);
    app->cursor_bgra = (uint8_t*)malloc(bytes);
    memcpy(app->cursor_bgra, c->image->bgra, bytes);
    app->cursor = *c->image;
    app->cursor.bgra = app->cursor_bgra;
  }
  app->cursor_updates++;
  pthread_mutex_unlock(&app->cursor_mu);
}

static void on_state(void* user, rda_state state, rda_status reason) {
  viewer_app* app = (viewer_app*)user;
  if (state == RDA_STATE_CONNECTED) printf("connected\n");
  if (state == RDA_STATE_DISCONNECTED) printf("disconnected: %s\n", rda_status_str(reason));
  fflush(stdout);
  events_set_state(&app->events, state, reason);
}

static void on_message(void* user, uint32_t type, const uint8_t* payload, size_t n) {
  viewer_app* app = (viewer_app*)user;
  pthread_mutex_lock(&app->msg_mu);
  if (type == APP_MSG_SESSION_INFO) {
    size_t k = n < sizeof app->session_info - 1 ? n : sizeof app->session_info - 1;
    memcpy(app->session_info, payload, k);
    app->session_info[k] = 0;
  } else if (type == APP_MSG_PONG && n == 8) {
    uint64_t sent;
    memcpy(&sent, payload, 8);
    app->ping_rtt_us = rda_now_us() - sent;
  }
  pthread_mutex_unlock(&app->msg_mu);
}

static void send_event(rda_viewer* v, rda_input_kind kind, int32_t x, int32_t y, uint32_t code) {
  rda_input_event ev;
  rda_status s;
  rda_input_event_default(&ev);
  ev.kind = kind;
  ev.x = x;
  ev.y = y;
  ev.code = code;
  s = rda_viewer_send_input(v, &ev);
  if (s != RDA_OK) fprintf(stderr, "input: %s (%s)\n", rda_status_str(s), rda_last_error());
}

/* Types UTF-8 text as RDA_INPUT_TEXT events, one per character. */
static void send_text(rda_viewer* v, const char* s) {
  const unsigned char* p = (const unsigned char*)s;
  while (*p) {
    uint32_t cp;
    int extra;
    if (*p < 0x80) { cp = *p; extra = 0; }
    else if ((*p & 0xe0) == 0xc0) { cp = *p & 0x1f; extra = 1; }
    else if ((*p & 0xf0) == 0xe0) { cp = *p & 0x0f; extra = 2; }
    else { cp = *p & 0x07; extra = 3; }
    ++p;
    while (extra-- > 0 && (*p & 0xc0) == 0x80) cp = (cp << 6) | (*p++ & 0x3f);
    send_event(v, RDA_INPUT_TEXT, 0, 0, cp);
  }
}

static void write_ppm(const char* path, const uint8_t* bgra, int32_t w, int32_t h) {
  FILE* f = fopen(path, "wb");
  int32_t i;
  if (!f) {
    perror(path);
    return;
  }
  fprintf(f, "P6\n%d %d\n255\n", w, h);
  for (i = 0; i < w * h; ++i) {
    unsigned char rgb[3] = {bgra[i * 4 + 2], bgra[i * 4 + 1], bgra[i * 4 + 0]};
    fwrite(rgb, 1, 3, f);
  }
  fclose(f);
}

static void sleep_ms(int ms) {
  struct timespec ts;
  ts.tv_sec = ms / 1000;
  ts.tv_nsec = (long)(ms % 1000) * 1000000L;
  nanosleep(&ts, NULL);
}

int main(int argc, char** argv) {
  viewer_app app;
  uint8_t key[32];
  const char* key_file = arg_value(argc, argv, "--key-file", NULL);
  const char* host = arg_value(argc, argv, "--host", "127.0.0.1");
  uint16_t port = (uint16_t)atoi(arg_value(argc, argv, "--port", "7300"));
  const char* pair_code = arg_value(argc, argv, "--pair-code", "");
  double seconds = atof(arg_value(argc, argv, "--seconds", "5"));
  const char* snapshot = arg_value(argc, argv, "--snapshot", NULL);
  int retries = atoi(arg_value(argc, argv, "--retries", "10"));
  int with_input = !arg_value(argc, argv, "--no-input", NULL);
  int connected = 0, attempt;
  rda_viewer_stats vs;
  char decoder[64] = "";

  if (!check_library_version()) return 1;
  rda_set_log(log_to_stderr, NULL, getenv("SUPPORT_LOG") ? RDA_LOG_INFO : RDA_LOG_WARN);
  if (!key_file) {
    fprintf(stderr, "usage: support_viewer --key-file=PATH [--host=127.0.0.1] [--port=7300] "
                    "[--pair-code=CODE] [--seconds=5] [--snapshot=out.ppm] [--retries=10]\n");
    return 2;
  }
  if (!load_key(key_file, key)) return 2;
  memset(&app, 0, sizeof app);
  pthread_mutex_init(&app.cursor_mu, NULL);
  pthread_mutex_init(&app.msg_mu, NULL);

  for (attempt = 0; attempt <= retries && !connected; ++attempt) {
    rda_transport* t = NULL;
    rda_viewer_config cfg;
    rda_status s;
    uint64_t start;

    events_init(&app.events);
    s = rda_udp_connect(host, port, &t);
    if (s != RDA_OK) {
      fprintf(stderr, "connect %s:%u: %s (%s)\n", host, port, rda_status_str(s), rda_last_error());
      return 1;
    }
    rda_viewer_config_default(&cfg);
    cfg.user = &app;
    cfg.on_stream_info = on_stream_info;
    cfg.on_frame = on_frame;
    cfg.on_cursor = on_cursor;
    cfg.on_state = on_state;
    cfg.on_message = on_message;
    cfg.encryption_key = key;
    cfg.auth_token = (const uint8_t*)pair_code;
    cfg.auth_token_len = strlen(pair_code);
    cfg.peer_timeout_ms = 2000; /* how long to wait for the host to answer */
    s = rda_viewer_create(t, &cfg, &app.viewer);
    if (s != RDA_OK) {
      fprintf(stderr, "viewer: %s (%s)\n", rda_status_str(s), rda_last_error());
      return 1;
    }

    /* Wait for the handshake. */
    while (rda_viewer_get_state(app.viewer) == RDA_STATE_CONNECTING) sleep_ms(20);
    if (rda_viewer_get_state(app.viewer) != RDA_STATE_CONNECTED) {
      rda_status why = app.events.why;
      rda_viewer_destroy(app.viewer);
      app.viewer = NULL;
      events_destroy(&app.events);
      if (why == RDA_ERR_AUTH || why == RDA_ERR_PROTOCOL) {
        fprintf(stderr, "refused by the host: %s\n", rda_status_str(why));
        return 1;
      }
      fprintf(stderr, "no answer from %s:%u, retrying (%d)\n", host, port, attempt + 1);
      sleep_ms(500);
      continue;
    }
    connected = 1;

    start = rda_now_us();
    {
      uint64_t now = rda_now_us();
      rda_viewer_send_message(app.viewer, APP_MSG_PING, (const uint8_t*)&now, 8);
    }
    {
      static const char clip[] = "pasted from the viewer";
      rda_viewer_send_message(app.viewer, RDA_MSG_CLIPBOARD_TEXT, (const uint8_t*)clip,
                              sizeof clip - 1);
    }
    if (with_input) {
      /* Wait for a first picture so coordinates mean something. */
      while (app.frames == 0 && rda_now_us() - start < 3000000) sleep_ms(10);
      if (app.width > 0) {
        int i;
        for (i = 0; i <= 10; ++i) {
          send_event(app.viewer, RDA_INPUT_MOUSE_MOVE, app.width / 4 + i * app.width / 20,
                     app.height / 2, 0);
          sleep_ms(16);
        }
        send_event(app.viewer, RDA_INPUT_MOUSE_DOWN, app.width * 3 / 4, app.height / 2,
                   RDA_BUTTON_LEFT);
        send_event(app.viewer, RDA_INPUT_MOUSE_UP, app.width * 3 / 4, app.height / 2,
                   RDA_BUTTON_LEFT);
        send_text(app.viewer, "hello, \xe4\xbd\xa0\xe5\xa5\xbd");
        send_event(app.viewer, RDA_INPUT_KEY_DOWN, 0, 0, 0x28); /* Enter, HID usage */
        send_event(app.viewer, RDA_INPUT_KEY_UP, 0, 0, 0x28);
      }
    }
    events_wait_end(&app.events, (int)(seconds * 1000));

    vs.struct_size = sizeof vs;
    rda_viewer_get_stats(app.viewer, &vs);
    rda_viewer_decoder_name(app.viewer, decoder, sizeof decoder);
    rda_viewer_destroy(app.viewer); /* after this no callback runs */
    app.viewer = NULL;
    events_destroy(&app.events);
  }
  if (!connected) {
    fprintf(stderr, "gave up after %d attempts\n", retries + 1);
    return 1;
  }

  printf("decoder %s: %lu pictures (%lu changed areas), %.2f MB received, %llu decode errors\n",
         decoder, app.frames, app.changed_rects, vs.bytes_received / 1e6,
         (unsigned long long)vs.decode_errors);
  printf("cursor updates %lu, last at %d,%d %s\n", app.cursor_updates, app.cx, app.cy,
         app.cvisible ? "visible" : "hidden");
  printf("session info \"%s\", app ping rtt %.1f ms\n", app.session_info, app.ping_rtt_us / 1000.0);
  if (snapshot && app.screen) {
    if (app.cvisible && app.cursor_bgra) {
      rda_draw_cursor(&app.cursor, app.cx, app.cy, app.screen, app.width, app.height, app.width * 4);
    }
    write_ppm(snapshot, app.screen, app.width, app.height);
    printf("wrote %s\n", snapshot);
  }
  free(app.screen);
  free(app.cursor_bgra);
  pthread_mutex_destroy(&app.cursor_mu);
  pthread_mutex_destroy(&app.msg_mu);
  return app.frames > 0 ? 0 : 1;
}
