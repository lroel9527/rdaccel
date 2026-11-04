/* support_host: the host half of a small remote-support product, written
 * the way an outside application would use rdaccel: plain C, only
 * <rdaccel/rdaccel.h>, built against the installed library.
 *
 * It serves one viewer at a time, one after the other, on a UDP port:
 *   - everything is encrypted with a key both sides keep in a file;
 *   - a viewer must present the pairing code (the auth token) to get in;
 *   - the screen (or one display of it) is captured by the library;
 *   - the viewer's mouse and keyboard are played into this machine with
 *     the library's injector (--inject);
 *   - clipboard text from the viewer is kept; pings are answered;
 *   - when the viewer leaves, the host is torn down and a fresh one waits
 *     for the next viewer.
 *
 *   support_host --key-file=PATH [--port=7300] [--pair-code=CODE]
 *                [--display=0] [--inject] [--sessions=0] [--fps=30]
 *   support_host --list-displays
 */
#include "common.h"

#include <signal.h>

static volatile sig_atomic_t g_stop = 0;
static void on_signal(int sig) {
  (void)sig;
  g_stop = 1;
}

typedef struct {
  session_events events;
  const char* pair_code;
  rda_injector* injector; /* may be NULL */
  rda_host* host;         /* set before any callback can use it */
  int display_index;
  pthread_mutex_t clip_mu;
  char clipboard[4096]; /* the "local clipboard" of this demo */
  unsigned long inputs, injected, inject_failures;
} host_app;

static void on_state(void* user, rda_state state, rda_status reason) {
  host_app* app = (host_app*)user;
  if (state == RDA_STATE_CONNECTED) {
    char info[128];
    int n = snprintf(info, sizeof info, "name=support_host;platform=%s;display=%d",
                     rda_platform_name(), app->display_index);
    /* Sending from the state callback is allowed. */
    rda_host_send_message(app->host, APP_MSG_SESSION_INFO, (const uint8_t*)info, (size_t)n);
    printf("viewer connected\n");
  } else if (state == RDA_STATE_DISCONNECTED) {
    printf("viewer gone: %s\n", rda_status_str(reason));
  }
  fflush(stdout);
  events_set_state(&app->events, state, reason);
}

static int on_auth(void* user, const uint8_t* token, size_t n) {
  host_app* app = (host_app*)user;
  size_t want = strlen(app->pair_code), i;
  unsigned diff = (unsigned)(n ^ want);
  /* Constant time over the expected length. */
  for (i = 0; i < want; ++i) diff |= (unsigned)(app->pair_code[i] ^ (i < n ? token[i] : 0));
  return diff == 0;
}

static void on_input(void* user, const rda_input_event* ev) {
  host_app* app = (host_app*)user;
  app->inputs++;
  if (!app->injector) return;
  if (rda_injector_inject(app->injector, ev) == RDA_OK) app->injected++;
  else app->inject_failures++;
}

static void on_message(void* user, uint32_t type, const uint8_t* payload, size_t n) {
  host_app* app = (host_app*)user;
  if (type == RDA_MSG_CLIPBOARD_TEXT) {
    size_t k = n < sizeof app->clipboard - 1 ? n : sizeof app->clipboard - 1;
    pthread_mutex_lock(&app->clip_mu);
    memcpy(app->clipboard, payload, k);
    app->clipboard[k] = 0;
    pthread_mutex_unlock(&app->clip_mu);
    printf("clipboard from viewer: %zu bytes\n", n);
  } else if (type == APP_MSG_PING) {
    rda_host_send_message(app->host, APP_MSG_PONG, payload, n);
  } else {
    printf("unknown message type 0x%x (%zu bytes)\n", type, n);
  }
  fflush(stdout);
}

static void on_error(void* user, rda_status status, const char* message) {
  host_app* app = (host_app*)user;
  fprintf(stderr, "host error %s: %s\n", rda_status_str(status), message);
  /* RDA_ERR_UNSUPPORTED: no cursor channel, the picture keeps streaming.
   * Anything else: the host has stopped (encoder, capture). */
  if (status != RDA_ERR_UNSUPPORTED) events_set_fatal(&app->events);
}

static int list_displays(void) {
  size_t n = 0, i;
  rda_display_info d[16];
  d[0].struct_size = sizeof d[0];
  if (rda_list_displays(d, 16, &n) != RDA_OK) {
    fprintf(stderr, "no displays: %s\n", rda_last_error());
    return 1;
  }
  for (i = 0; i < n && i < 16; ++i) {
    printf("%d: %dx%d at %d,%d%s %s\n", d[i].index, d[i].width, d[i].height, d[i].x, d[i].y,
           d[i].primary ? " (primary)" : "", d[i].name);
  }
  return 0;
}

/* One viewer, start to end. Returns 0 to keep serving, 1 to stop. */
static int serve_one(host_app* app, uint16_t port, const uint8_t key[32], int fps) {
  rda_transport* t = NULL;
  rda_host_config cfg;
  rda_host_stats last, now;
  rda_status s;
  uint64_t t0;
  int quit = 0;

  events_init(&app->events);
  s = rda_udp_bind(port, &t);
  if (s != RDA_OK) {
    fprintf(stderr, "bind port %u: %s (%s)\n", port, rda_status_str(s), rda_last_error());
    events_destroy(&app->events);
    return 1;
  }
  rda_host_config_default(&cfg);
  cfg.fps = fps;
  cfg.display_index = app->display_index;
  cfg.encryption_key = key;
  cfg.user = app;
  cfg.on_state = on_state;
  cfg.on_auth = app->pair_code[0] ? on_auth : NULL;
  cfg.on_input = on_input;
  cfg.on_message = on_message;
  cfg.on_error = on_error;

  /* The callbacks use app->host. rda_host_create() writes it before any
   * callback runs (see the header). */
  s = rda_host_create(t, &cfg, &app->host);
  if (s != RDA_OK) {
    fprintf(stderr, "host: %s (%s)\n", rda_status_str(s), rda_last_error());
    events_destroy(&app->events);
    return 1;
  }
  s = rda_host_start_capture(app->host);
  if (s != RDA_OK) {
    fprintf(stderr, "screen capture: %s (%s)\n", rda_status_str(s), rda_last_error());
    rda_host_destroy(app->host);
    app->host = NULL;
    events_destroy(&app->events);
    return 1;
  }
  printf("waiting for a viewer on UDP port %u\n", port);
  fflush(stdout);

  memset(&last, 0, sizeof last);
  last.struct_size = sizeof last;
  t0 = rda_now_us();
  for (;;) {
    int ended = events_wait_end(&app->events, 2000);
    if (g_stop) quit = 1;
    if (ended || quit) break;
    if (rda_host_get_state(app->host) != RDA_STATE_CONNECTED) continue;
    now.struct_size = sizeof now;
    rda_host_get_stats(app->host, &now);
    printf("%5.1fs %3llu frames %7.0f kbps target %6.0f kbps rtt %3u ms inputs %lu\n",
           (rda_now_us() - t0) / 1e6,
           (unsigned long long)(now.frames_encoded - last.frames_encoded),
           (now.bytes_sent - last.bytes_sent) * 8 / 2000.0, now.target_bitrate_bps / 1000.0,
           now.rtt_us / 1000, app->inputs);
    fflush(stdout);
    last = now;
  }
  if (app->events.fatal) quit = 1;
  {
    char enc[64];
    now.struct_size = sizeof now;
    rda_host_get_stats(app->host, &now);
    rda_host_encoder_name(app->host, enc, sizeof enc);
    printf("session over: encoder %s, %llu frames, %.2f MB, %lu inputs (%lu injected, %lu failed)\n",
           enc, (unsigned long long)now.frames_encoded, now.bytes_sent / 1e6, app->inputs,
           app->injected, app->inject_failures);
  }
  /* Not from a callback: main thread. */
  rda_host_destroy(app->host);
  app->host = NULL;
  events_destroy(&app->events);
  app->inputs = app->injected = app->inject_failures = 0;
  return quit;
}

int main(int argc, char** argv) {
  host_app app;
  uint8_t key[32];
  const char* key_file = arg_value(argc, argv, "--key-file", NULL);
  uint16_t port = (uint16_t)atoi(arg_value(argc, argv, "--port", "7300"));
  int sessions = atoi(arg_value(argc, argv, "--sessions", "0"));
  int fps = atoi(arg_value(argc, argv, "--fps", "30"));
  int served = 0;

  if (!check_library_version()) return 1;
  if (getenv("SUPPORT_LOG")) rda_set_log(log_to_stderr, NULL, RDA_LOG_INFO);
  else rda_set_log(log_to_stderr, NULL, RDA_LOG_WARN);
  if (arg_value(argc, argv, "--list-displays", NULL)) return list_displays();
  if (!key_file) {
    fprintf(stderr, "usage: support_host --key-file=PATH [--port=7300] [--pair-code=CODE] "
                    "[--display=0] [--inject] [--sessions=0] [--fps=30]\n");
    return 2;
  }
  if (!load_key(key_file, key)) return 2;

  memset(&app, 0, sizeof app);
  pthread_mutex_init(&app.clip_mu, NULL);
  app.pair_code = arg_value(argc, argv, "--pair-code", "");
  app.display_index = atoi(arg_value(argc, argv, "--display", "0"));
  if (arg_value(argc, argv, "--inject", NULL)) {
    rda_injector_config ic;
    rda_status s;
    rda_injector_config_default(&ic);
    ic.display_index = app.display_index;
    s = rda_injector_create(&ic, &app.injector);
    if (s != RDA_OK) {
      fprintf(stderr, "input injection: %s (%s)\n", rda_status_str(s), rda_last_error());
      return 1;
    }
  }
  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);

  printf("support_host on rdaccel %s (%s)\n", rda_version_string(), rda_platform_name());
  while (!g_stop) {
    if (serve_one(&app, port, key, fps)) break;
    if (sessions > 0 && ++served >= sessions) break;
  }
  if (app.injector) rda_injector_destroy(app.injector);
  pthread_mutex_lock(&app.clip_mu);
  if (app.clipboard[0]) printf("last clipboard text: \"%s\"\n", app.clipboard);
  pthread_mutex_unlock(&app.clip_mu);
  pthread_mutex_destroy(&app.clip_mu);
  return 0;
}
