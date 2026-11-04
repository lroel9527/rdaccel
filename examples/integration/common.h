/* Shared helpers for the two integration programs. Nothing here is rdaccel
 * API; it is the glue an application writes around it. */
#ifndef RDA_INTEGRATION_COMMON_H_
#define RDA_INTEGRATION_COMMON_H_

#include <rdaccel/rdaccel.h>

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* The application's own message types, above the library's reserved range. */
#define APP_MSG_SESSION_INFO (RDA_MSG_USER_FIRST + 1) /* host -> viewer: "name=...;display=..." */
#define APP_MSG_PING (RDA_MSG_USER_FIRST + 2)         /* either way: 8-byte rda_now_us() */
#define APP_MSG_PONG (RDA_MSG_USER_FIRST + 3)         /* echo of a ping */

static inline int hexval(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

/* Reads a 32-byte key written as 64 hex digits (whitespace ignored) from a
 * file, the way an application might keep a pairing secret on disk. */
static inline int load_key(const char* path, uint8_t key[32]) {
  FILE* f = fopen(path, "r");
  char buf[256];
  size_t n, i, k = 0;
  if (!f) {
    perror(path);
    return 0;
  }
  n = fread(buf, 1, sizeof buf - 1, f);
  fclose(f);
  for (i = 0; i < n && k < 64; ++i) {
    int v = hexval(buf[i]);
    if (buf[i] == ' ' || buf[i] == '\n' || buf[i] == '\r' || buf[i] == '\t') continue;
    if (v < 0) break;
    if (k % 2 == 0) key[k / 2] = (uint8_t)(v << 4);
    else key[k / 2] |= (uint8_t)v;
    ++k;
  }
  if (k != 64) {
    fprintf(stderr, "%s: expected 64 hex digits\n", path);
    return 0;
  }
  return 1;
}

static inline const char* arg_value(int argc, char** argv, const char* name, const char* fallback) {
  size_t len = strlen(name);
  int i;
  for (i = 1; i < argc; ++i) {
    if (strncmp(argv[i], name, len) == 0 && argv[i][len] == '=') return argv[i] + len + 1;
    if (strcmp(argv[i], name) == 0) return "1";
  }
  return fallback;
}

static inline void log_to_stderr(void* user, rda_log_level level, const char* message) {
  static const char* names[] = {"error", "warn", "info", "debug"};
  (void)user;
  fprintf(stderr, "[rdaccel %s] %s\n", names[level], message);
}

/* Fails loudly when the header and the library disagree on the major
 * version (or, before 1.0, the minor version): the struct layouts would not
 * match. */
static inline int check_library_version(void) {
  uint32_t lib = rda_version();
  uint32_t want_major = RDA_VERSION_MAJOR, want_minor = RDA_VERSION_MINOR;
  if ((lib >> 16) != want_major || (want_major == 0 && ((lib >> 8) & 0xff) != want_minor)) {
    fprintf(stderr, "built against rdaccel %u.%u, running with %s\n", want_major, want_minor,
            rda_version_string());
    return 0;
  }
  return 1;
}

/* What the library's threads signal and main() waits on. */
typedef struct {
  pthread_mutex_t mu;
  pthread_cond_t cv;
  int state;       /* last rda_state seen */
  rda_status why;  /* reason of the last change */
  int fatal;       /* a background error that ends the session */
  int disconnects; /* how many times RDA_STATE_DISCONNECTED was seen */
} session_events;

static inline void events_init(session_events* e) {
  pthread_mutex_init(&e->mu, NULL);
  pthread_cond_init(&e->cv, NULL);
  e->state = RDA_STATE_CONNECTING;
  e->why = RDA_OK;
  e->fatal = 0;
  e->disconnects = 0;
}

static inline void events_destroy(session_events* e) {
  pthread_cond_destroy(&e->cv);
  pthread_mutex_destroy(&e->mu);
}

static inline void events_set_state(session_events* e, rda_state s, rda_status why) {
  pthread_mutex_lock(&e->mu);
  e->state = (int)s;
  e->why = why;
  if (s == RDA_STATE_DISCONNECTED) e->disconnects++;
  pthread_cond_broadcast(&e->cv);
  pthread_mutex_unlock(&e->mu);
}

static inline void events_set_fatal(session_events* e) {
  pthread_mutex_lock(&e->mu);
  e->fatal = 1;
  pthread_cond_broadcast(&e->cv);
  pthread_mutex_unlock(&e->mu);
}

/* Waits up to `ms` for a disconnect or a fatal error; returns 1 if one
 * happened. */
static inline int events_wait_end(session_events* e, int ms) {
  struct timespec ts;
  int ended;
  clock_gettime(CLOCK_REALTIME, &ts);
  ts.tv_sec += ms / 1000;
  ts.tv_nsec += (long)(ms % 1000) * 1000000L;
  if (ts.tv_nsec >= 1000000000L) {
    ts.tv_sec += 1;
    ts.tv_nsec -= 1000000000L;
  }
  pthread_mutex_lock(&e->mu);
  while (e->state != RDA_STATE_DISCONNECTED && !e->fatal) {
    if (pthread_cond_timedwait(&e->cv, &e->mu, &ts) != 0) break;
  }
  ended = e->state == RDA_STATE_DISCONNECTED || e->fatal;
  pthread_mutex_unlock(&e->mu);
  return ended;
}

/* Waits up to `ms` for disconnect number `n` (counted from 1) or a fatal
 * error; returns 1 if one happened. A host that keeps serving goes back to
 * waiting after each disconnect, so the state alone does not tell. */
static inline int events_wait_disconnects(session_events* e, int n, int ms) {
  struct timespec ts;
  int ended;
  clock_gettime(CLOCK_REALTIME, &ts);
  ts.tv_sec += ms / 1000;
  ts.tv_nsec += (long)(ms % 1000) * 1000000L;
  if (ts.tv_nsec >= 1000000000L) {
    ts.tv_sec += 1;
    ts.tv_nsec -= 1000000000L;
  }
  pthread_mutex_lock(&e->mu);
  while (e->disconnects < n && !e->fatal) {
    if (pthread_cond_timedwait(&e->cv, &e->mu, &ts) != 0) break;
  }
  ended = e->disconnects >= n || e->fatal;
  pthread_mutex_unlock(&e->mu);
  return ended;
}

#endif /* RDA_INTEGRATION_COMMON_H_ */
