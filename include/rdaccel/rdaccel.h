/*
 * rdaccel: low-bandwidth remote desktop streaming.
 *
 * This C header is the library's only stable interface. C++ applications may
 * prefer <rdaccel/rdaccel.hpp>, a header-only wrapper over these functions.
 *
 *   rda_host       capture (or be given) screen frames, encode, send
 *   rda_viewer     receive, decode, hand BGRA pictures to the application
 *   rda_transport  how the two talk: UDP, TCP, an in-process pipe, or yours
 *
 * Conventions
 *   - Every function that can fail returns rda_status (RDA_OK = 0, errors < 0).
 *     rda_last_error() gives a human-readable detail for the calling thread.
 *   - Structs that may grow start with `struct_size`. Fill them with the
 *     matching *_default() function, then change what you need; the library
 *     accepts structs from older headers (shorter struct_size).
 *   - Objects are opaque pointers created by *_create / *_connect / ... and
 *     released by *_destroy.
 *   - Callbacks receive the `user` pointer given with them and run on a
 *     library thread; see docs/integration.md for which one. None runs
 *     before the *_create call that set it up has returned, so callbacks
 *     may use the handle it wrote.
 *   - Pixels are BGRA, 8 bits per channel (byte order B, G, R, A in memory).
 */
#ifndef RDACCEL_RDACCEL_H_
#define RDACCEL_RDACCEL_H_

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#  define RDA_API
#elif defined(__GNUC__) || defined(__clang__)
#  define RDA_API __attribute__((visibility("default")))
#else
#  define RDA_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ---- version ------------------------------------------------------------ */

#define RDA_VERSION_MAJOR 1
#define RDA_VERSION_MINOR 3
#define RDA_VERSION_PATCH 0
/* "rc1" and so on for a pre-release, "" for a release. rda_version() does
 * not include it; rda_version_string() does ("1.0.0-rc1"). */
#define RDA_VERSION_PRERELEASE ""
#define RDA_MAKE_VERSION(major, minor, patch) \
  (((uint32_t)(major) << 16) | ((uint32_t)(minor) << 8) | (uint32_t)(patch))
#define RDA_VERSION RDA_MAKE_VERSION(RDA_VERSION_MAJOR, RDA_VERSION_MINOR, RDA_VERSION_PATCH)

/* The library's version as RDA_MAKE_VERSION() and as text ("0.2.0"). */
RDA_API uint32_t rda_version(void);
RDA_API const char* rda_version_string(void);
/* "macOS" or "Linux". */
RDA_API const char* rda_platform_name(void);
/* Monotonic clock in microseconds, the one the library uses. */
RDA_API uint64_t rda_now_us(void);

/* ---- errors and logging ------------------------------------------------- */

typedef enum rda_status {
  RDA_OK = 0,
  RDA_ERR_INVALID_ARG = -1,  /* null pointer, bad size, unknown option */
  RDA_ERR_NO_MEMORY = -2,
  RDA_ERR_UNSUPPORTED = -3,  /* e.g. no HEVC encoder or decoder on this machine */
  RDA_ERR_PERMISSION = -4,   /* e.g. macOS Screen Recording not granted */
  RDA_ERR_CODEC = -5,        /* encoder or decoder failed */
  RDA_ERR_TRANSPORT = -6,    /* socket or connection error */
  RDA_ERR_TIMEOUT = -7,
  RDA_ERR_CLOSED = -8,       /* the connection or object is closed */
  RDA_ERR_AUTH = -9,
  RDA_ERR_PROTOCOL = -10,    /* incompatible peer or malformed data */
  RDA_ERR_BUSY = -11,        /* already running, or not connected yet */
  RDA_ERR_IO = -12           /* file could not be read or written */
} rda_status;

/* Constant name of a status, e.g. "RDA_ERR_TIMEOUT". */
RDA_API const char* rda_status_str(rda_status s);

/* Detail text for the last failing call on this thread ("" if none). Valid
 * until the next failing call on the same thread. */
RDA_API const char* rda_last_error(void);

typedef enum rda_log_level {
  RDA_LOG_ERROR = 0,
  RDA_LOG_WARN = 1,
  RDA_LOG_INFO = 2,
  RDA_LOG_DEBUG = 3
} rda_log_level;

/* Process-wide log sink. The library never prints by itself; without a sink
 * messages are dropped. Messages at most `max_level` are delivered, from any
 * thread, possibly concurrently. They never contain picture content, key
 * presses or message payloads. Pass fn = NULL to remove the sink. */
typedef void (*rda_log_fn)(void* user, rda_log_level level, const char* message);
RDA_API void rda_set_log(rda_log_fn fn, void* user, rda_log_level max_level);

/* ---- basic types -------------------------------------------------------- */

typedef struct rda_rect {
  int32_t x, y, w, h; /* physical pixels, origin top-left */
} rda_rect;

typedef struct rda_image {
  const uint8_t* data; /* BGRA */
  int32_t width;
  int32_t height;
  int32_t stride; /* bytes per row */
} rda_image;

/* Side length of the library's tiles in pixels. */
#define RDA_TILE_SIZE 64

/* ---- cursor ------------------------------------------------------------- */

typedef struct rda_cursor_image {
  int32_t width, height;       /* at most 256 x 256 */
  int32_t hotspot_x, hotspot_y; /* the pixel that sits at the pointer position */
  const uint8_t* bgra;         /* width * height * 4 bytes, premultiplied alpha */
} rda_cursor_image;

typedef struct rda_cursor_state {
  uint32_t struct_size;
  int32_t x, y;      /* host pixels, where the hotspot goes */
  int32_t visible;   /* 0 = hidden */
  uint64_t time_us;  /* host clock */
  uint64_t image_id; /* changes exactly when the image does; 0 = none yet */
  int32_t image_changed;
  const rda_cursor_image* image; /* valid during the callback; NULL if image_id == 0 */
} rda_cursor_state;

/* Alpha-blends `c` with its hotspot at (x, y) into a BGRA picture. */
RDA_API void rda_draw_cursor(const rda_cursor_image* c, int32_t x, int32_t y, uint8_t* bgra,
                             int32_t width, int32_t height, int32_t stride);

/* ---- displays ----------------------------------------------------------- */

typedef struct rda_display_info {
  uint32_t struct_size;
  int32_t index;         /* for rda_host_config.display_index; 0 = primary */
  int32_t x, y;          /* origin on the desktop: points on macOS, pixels on X11 */
  int32_t width, height; /* pixels */
  int32_t primary;       /* nonzero for index 0 */
  char name[64];         /* UTF-8, NUL-terminated, may be empty */
} rda_display_info;

/* The displays rda_host_start_capture() can capture, primary first. Set
 * out[0].struct_size (e.g. to sizeof(rda_display_info)): entries are written
 * that far apart, up to `capacity` of them. `*count` gets the number of
 * displays, also when it exceeds `capacity` (call with capacity 0 and
 * out NULL to ask). RDA_ERR_UNSUPPORTED when there is no display (no X
 * server, ...). */
RDA_API rda_status rda_list_displays(rda_display_info* out, size_t capacity, size_t* count);

/* ---- input -------------------------------------------------------------- */

typedef enum rda_input_kind {
  RDA_INPUT_MOUSE_MOVE = 1,
  RDA_INPUT_MOUSE_DOWN = 2,
  RDA_INPUT_MOUSE_UP = 3,
  RDA_INPUT_SCROLL = 4,
  RDA_INPUT_KEY_DOWN = 5,
  RDA_INPUT_KEY_UP = 6,
  /* Typed text: one Unicode scalar value in `code` (1..0x10FFFF, no
   * surrogates). For what the viewer's keyboard layout or IME produced; the
   * host types it whatever its own layout. Send a string one character at
   * a time. rda_viewer_send_input() returns RDA_ERR_UNSUPPORTED when the
   * host is older than 0.4 and cannot take it. Viewers should send every
   * printable character this way and keep key events for keys that type
   * nothing (Enter, Backspace, arrows, shortcuts with Ctrl/Command): a
   * printable key sent as a key event goes through the host's own input
   * method (Pinyin, Kana, ...) when one is active. */
  RDA_INPUT_TEXT = 7
} rda_input_kind;

/* A viewer-side input event, delivered to the host. The library carries it;
 * injecting it into the host OS is the application's job. */
typedef struct rda_input_event {
  uint32_t struct_size;
  rda_input_kind kind;
  int32_t x, y;             /* host pixels (mouse events) */
  int32_t delta_x, delta_y; /* scroll, in wheel lines: y > 0 up, x > 0 right */
  uint32_t code;            /* RDA_BUTTON_* for mouse events; for keys a USB
                               HID usage (keyboard page 0x07), see below;
                               for RDA_INPUT_TEXT a Unicode scalar value */
  uint32_t modifiers;       /* RDA_MOD_* bits held during the event */
} rda_input_event;

RDA_API void rda_input_event_default(rda_input_event* ev);

#define RDA_BUTTON_LEFT 1
#define RDA_BUTTON_RIGHT 2
#define RDA_BUTTON_MIDDLE 3
#define RDA_BUTTON_BACK 4
#define RDA_BUTTON_FORWARD 5

#define RDA_MOD_SHIFT 0x01u
#define RDA_MOD_CTRL 0x02u
#define RDA_MOD_ALT 0x04u  /* Option on macOS */
#define RDA_MOD_META 0x08u /* Command on macOS, Windows/Super key */
#define RDA_MOD_CAPS 0x10u /* Caps Lock is on */

/* Key codes on the wire are USB HID usages (0x04 = A, 0x28 = Enter,
 * 0xE0 = left Control, ...), the same on every OS. These convert native
 * codes. The from_* functions return 0 for an unknown key; the to_*
 * functions return RDA_KEYCODE_NONE when the OS has no such key. */
#define RDA_KEYCODE_NONE 0xFFFFFFFFu
/* macOS virtual key codes (kVK_*, NSEvent.keyCode, CGKeyCode). */
RDA_API uint32_t rda_keycode_from_mac(uint32_t mac_keycode);
RDA_API uint32_t rda_keycode_to_mac(uint32_t hid_usage);
/* Linux evdev codes (KEY_*; an X11 keycode minus 8). */
RDA_API uint32_t rda_keycode_from_evdev(uint32_t evdev_code);
RDA_API uint32_t rda_keycode_to_evdev(uint32_t hid_usage);

/* ---- input injection ----------------------------------------------------- */

/* Optional: plays rda_input_event into the host OS, for applications that
 * do not inject input themselves. Linux: the XTest extension of the X
 * server in DISPLAY. macOS: CGEvent, which needs the Accessibility
 * permission. Typical use: create one next to the host and call
 * rda_injector_inject() from on_input. */
typedef struct rda_injector rda_injector;

typedef struct rda_injector_config {
  uint32_t struct_size;
  int32_t display_index; /* 0; the display the host captures */
  /* The picture size the viewer's coordinates refer to: the host's
   * capture_width / capture_height when it scales. 0 = native size. */
  int32_t source_width, source_height;
} rda_injector_config;

RDA_API void rda_injector_config_default(rda_injector_config* config);
/* RDA_ERR_PERMISSION without the macOS Accessibility permission,
 * RDA_ERR_UNSUPPORTED without an X server with XTest or for a display
 * that does not exist. config may be NULL for the defaults. */
RDA_API rda_status rda_injector_create(const rda_injector_config* config, rda_injector** out);
RDA_API void rda_injector_destroy(rda_injector* injector); /* releases held keys and buttons */
/* Thread-safe. Scroll deltas are wheel lines (y > 0 up, x > 0 right).
 * Keys pass through as physical keys; RDA_INPUT_TEXT types the character
 * whatever the host's layout. macOS: while an input method is the current
 * input source, text is typed with the ASCII-capable layout selected (the
 * input method would otherwise take the characters as keystrokes), and the
 * input method comes back after a second without text, at the next key
 * event, or on destroy. That needs the main thread to run a run loop
 * (AppKit, dispatch_main, CFRunLoopRun); without one, text goes in as is.
 * RDA_ERR_UNSUPPORTED for a key or character the host cannot produce. */
RDA_API rda_status rda_injector_inject(rda_injector* injector, const rda_input_event* ev);

/* ---- keyboard input sources (1.2) ----------------------------------------- */

/* A keyboard layout (ABC, US) or input method mode (Pinyin, Kana): what
 * turns physical keys into characters. macOS only so far (Text Input
 * Sources); elsewhere these functions return RDA_ERR_UNSUPPORTED. On macOS
 * they need the main thread, or a main thread that runs a run loop (AppKit,
 * dispatch_main, CFRunLoopRun). */
typedef struct rda_input_source_info {
  uint32_t struct_size;
  char id[128];           /* "com.apple.inputmethod.SCIM.ITABC", "com.apple.keylayout.ABC" */
  char name[128];         /* localized, UTF-8, for display */
  char language[32];      /* its first language, BCP 47 ("zh-Hans", "en"); may be "" */
  int32_t input_method;   /* nonzero: composes text (Pinyin, Kana); 0: a plain layout */
  int32_t ascii_capable;  /* nonzero: types ASCII as is (ABC, US) */
} rda_input_source_info;

/* The input sources the user enabled and can switch to, as for
 * rda_list_displays(): set out[0].struct_size; *count gets the number of
 * sources, also when it exceeds `capacity`. */
RDA_API rda_status rda_list_input_sources(rda_input_source_info* out, size_t capacity, size_t* count);
/* The input source selected now. Set out->struct_size first. */
RDA_API rda_status rda_current_input_source(rda_input_source_info* out);
/* Selects an enabled input source by id for the keys the injector sends
 * next (and for the user at that computer: it is the system's selection).
 * It also ends the injector's own switch away from an input method for
 * RDA_INPUT_TEXT, so that choice is not undone later. RDA_ERR_INVALID_ARG
 * for a source that is not enabled there. Thread-safe. */
RDA_API rda_status rda_injector_select_input_source(rda_injector* injector, const char* id);

/* ---- connection --------------------------------------------------------- */

/* A host and a viewer shake hands (protocol version, optional auth token),
 * keep the connection alive and notice when the other side is gone. */
typedef enum rda_state {
  RDA_STATE_CONNECTING = 0,  /* waiting for the peer's handshake */
  RDA_STATE_CONNECTED = 1,
  RDA_STATE_DISCONNECTED = 2 /* final; create a new host or viewer to retry */
} rda_state;

/* Application messages: reliable, ordered, up to 16 MiB each, sent apart
 * from the picture so they do not hold it up. Types below
 * RDA_MSG_USER_FIRST are reserved for the library. */
#define RDA_MSG_CLIPBOARD_TEXT 1u /* UTF-8 text, no terminating NUL */
#define RDA_MSG_USER_FIRST 0x10000u
#define RDA_MSG_MAX_SIZE (16u * 1024u * 1024u)

/* ---- transport ---------------------------------------------------------- */

typedef struct rda_transport rda_transport;

/* Logical channels. A transport may give each its own ordered stream (so a
 * large video frame does not delay input) or multiplex them. */
typedef enum rda_channel {
  RDA_CHANNEL_CONTROL = 0,
  RDA_CHANNEL_INPUT = 1,
  RDA_CHANNEL_SURFACE = 2,
  RDA_CHANNEL_VIDEO = 3,
  RDA_CHANNEL_CURSOR = 4,
  RDA_CHANNEL_MESSAGE = 5 /* application messages; reliable like control */
} rda_channel;
#define RDA_CHANNEL_COUNT 6

typedef struct rda_transport_stats {
  uint32_t struct_size;
  int64_t bandwidth_bps;  /* estimated deliverable rate; 0 = unknown */
  uint32_t rtt_us;        /* smoothed round trip; 0 = unknown */
  uint64_t queued_bytes;  /* accepted but not yet on the wire */
  uint64_t bytes_in_flight;
  uint64_t packets_sent;
  uint64_t packets_lost;
  uint64_t packets_spurious; /* 1.3: of packets_lost, acknowledged after all (late,
                                not lost); packets_lost - packets_spurious is real loss */
} rda_transport_stats;

/* Receive callback handed to a transport. `ctx` is the library's. */
typedef void (*rda_receive_fn)(void* ctx, rda_channel ch, const uint8_t* bytes, size_t n);

/* Your own transport (QUIC, WebRTC data channel, relay, ...). Requirements:
 *   send          thread-safe; bytes on one channel arrive in order at the
 *                 peer, possibly split or merged differently. Return RDA_OK,
 *                 or RDA_ERR_CLOSED once the connection is gone. Carry every
 *                 channel number below 256 as is: later versions may add
 *                 channels after RDA_CHANNEL_COUNT.
 *   set_receiver  store fn + ctx and call fn on your thread for every
 *                 received chunk. Hold bytes that arrive before it is set.
 *                 fn == NULL clears it.
 *   close         stop sending and receiving.
 *   get_stats     optional (NULL): return 1 and fill `out` if you measure the
 *                 path; the host then adapts its bitrate.
 *   destroy       stop your threads, wait for a running receive call to
 *                 return, free `self`. Called exactly once. */
typedef struct rda_transport_vtable {
  uint32_t struct_size;
  rda_status (*send)(void* self, rda_channel ch, const uint8_t* bytes, size_t n);
  void (*set_receiver)(void* self, rda_receive_fn fn, void* ctx);
  void (*close)(void* self);
  int (*get_stats)(void* self, rda_transport_stats* out);
  void (*destroy)(void* self);
} rda_transport_vtable;

/* Wraps your implementation. On failure `destroy(self)` has been called. */
RDA_API rda_status rda_transport_custom(const rda_transport_vtable* vtable, void* self,
                                        rda_transport** out);

/* Two connected in-process endpoints. */
RDA_API rda_status rda_pipe_create(rda_transport** a, rda_transport** b);

/* TCP, fixed bitrate (no stats). Host side: listens on `port` (IPv4 and
 * IPv6) and returns at once; the first viewer to connect becomes the peer.
 * Create the host right away and watch on_state for RDA_STATE_CONNECTED. */
RDA_API rda_status rda_tcp_bind(uint16_t port, rda_transport** out);
RDA_API rda_status rda_tcp_connect(const char* host, uint16_t port, rda_transport** out);

/* UDP with loss recovery and BBR-style congestion control; reports stats, so
 * the host adapts its bitrate. Use on real networks. Host side: binds `port`
 * (IPv4 and IPv6) and returns at once; the first viewer that sends becomes
 * the peer. Create the host right away and watch on_state for
 * RDA_STATE_CONNECTED. */
RDA_API rda_status rda_udp_bind(uint16_t port, rda_transport** out);
RDA_API rda_status rda_udp_connect(const char* host, uint16_t port, rda_transport** out);
/* Uses your UDP socket (after NAT traversal, from a launcher, ...); taken
 * over (closed with the transport) only on success. A connected socket talks
 * to its peer; an unconnected one to the first sender, host side only.
 * viewer_side != 0 for the end that creates the viewer (must be connected). */
RDA_API rda_status rda_udp_from_fd(int fd, int viewer_side, rda_transport** out);

/* A host-side transport that writes the screen stream to a .rdstream file. */
RDA_API rda_status rda_recording_create(const char* path, rda_transport** out);

/* Using a transport directly (a relay, a test). Not needed when it is handed
 * to a host or viewer, which then owns it. */
RDA_API rda_status rda_transport_send(rda_transport* t, rda_channel ch, const uint8_t* bytes,
                                      size_t n);
RDA_API void rda_transport_set_receiver(rda_transport* t, rda_receive_fn fn, void* ctx);
RDA_API void rda_transport_close(rda_transport* t);
/* Returns 1 and fills `out` (set out->struct_size first) if the path is measured. */
RDA_API int rda_transport_get_stats(rda_transport* t, rda_transport_stats* out);
/* Frees a transport that was not handed to rda_host_create / rda_viewer_create. */
RDA_API void rda_transport_destroy(rda_transport* t);

/* ---- host --------------------------------------------------------------- */

typedef struct rda_host rda_host;

typedef struct rda_host_config {
  uint32_t struct_size;

  int32_t fps;             /* 60 */
  int64_t bitrate_bps;     /* 8 Mbps; the ceiling with adaptive bitrate */
  int32_t min_qp;          /* 24; video quality ceiling, 0 = encoder decides */
  const char* encoder;     /* NULL = automatic; Linux: "hevc_nvenc", "libx265" or
                              "hevc_vaapi" (experimental; device from
                              RDACCEL_VAAPI_DEVICE, default renderD128) */

  /* rda_host_start_capture() only */
  int32_t capture_width;   /* 0 = native; otherwise scaled (even numbers) */
  int32_t capture_height;
  int32_t capture_cursor;  /* 0; draw the cursor into the picture */

  int32_t cursor_channel;  /* 1; send the cursor apart from the picture */
  int32_t cursor_poll_hz;  /* 120 */

  int32_t lossless;                /* 1; text and UI through the lossless layer */
  int32_t lossless_max_colors;     /* 256 */
  int32_t lossless_max_colors_hot; /* 4 */
  int32_t detect_moves;            /* 1; scrolling and moved windows as copies */
  int32_t refine_frames;           /* 8 */
  int32_t tile_cache;              /* 1 */
  int32_t refresh;                 /* 0 */
  int32_t progressive;             /* 1; fast video-only first picture, then exact */
  int32_t video_regions;           /* 1; video-like areas get their own stream */
  int32_t video_region_min_qp;     /* 27 */
  int64_t video_region_bitrate_bps; /* 0 = a share of bitrate_bps */

  int32_t adaptive_bitrate; /* 1; follow the transport's bandwidth estimate */
  int64_t min_bitrate_bps;  /* 300 kbps */
  int32_t max_queue_ms;     /* 50; skip frames rather than queue longer */

  /* Callbacks (all optional), each called with `user`. */
  void* user;
  /* Input from the viewer. Transport receive thread. */
  void (*on_input)(void* user, const rda_input_event* ev);
  /* Something on a background thread failed. `status` says what it means:
   *   RDA_ERR_UNSUPPORTED  no cursor channel (cursor source unavailable);
   *                        the picture keeps streaming
   *   anything else        the host has stopped and will send no more
   *                        pictures; destroy it (not from this callback):
   *     RDA_ERR_CODEC       the encoder could not be created
   *     RDA_ERR_PERMISSION  capture stopped: permission taken away (macOS)
   *     RDA_ERR_CLOSED      capture stopped: display gone, sharing ended
   * A single picture that fails to encode is not reported here (it is
   * logged and counted in rda_host_stats.encode_errors; the next one is a
   * keyframe). Capture or cursor thread. */
  void (*on_error)(void* user, rda_status status, const char* message);

  /* Connection. */
  int32_t peer_timeout_ms; /* 10000; silence from a connected viewer = gone; 0 = never */
  /* Connection changes. `reason` is RDA_OK, or why it disconnected:
   * RDA_ERR_CLOSED (goodbye or transport closed), RDA_ERR_TIMEOUT,
   * RDA_ERR_PROTOCOL (incompatible version), RDA_ERR_AUTH (on_auth refused).
   * With keep_serving, RDA_STATE_DISCONNECTED is followed by
   * RDA_STATE_CONNECTING when the host waits for the next viewer.
   * Transport thread, the library's timer thread, or its serve thread. */
  void (*on_state)(void* user, rda_state state, rda_status reason);
  /* An application message from the viewer. Transport receive thread. */
  void (*on_message)(void* user, uint32_t type, const uint8_t* payload, size_t n);
  /* Decides whether a viewer may connect, from the token in its config;
   * nonzero = accept. NULL = accept everyone. Transport receive thread.
   * With a hook, nothing about the screen (its size, pictures, the cursor)
   * is sent before the viewer is accepted; the host holds the newest frame
   * and sends it once it is. */
  int (*on_auth)(void* user, const uint8_t* token, size_t n);

  /* Encryption. */
  /* 32 bytes both sides share (copied): everything is encrypted and
   * authenticated with XChaCha20-Poly1305 under fresh per-connection keys.
   * A peer with another key, or none, disconnects with RDA_ERR_AUTH or
   * RDA_ERR_PROTOCOL. NULL = plaintext, which TCP and UDP transports refuse
   * unless allow_plaintext is set (pipes, recordings, simulated and custom
   * transports never need it). */
  const uint8_t* encryption_key;
  int32_t allow_plaintext; /* 0 */

  /* rda_host_start_capture() only */
  int32_t display_index; /* 0 = primary display; see rda_list_displays() */

  /* 1: when the viewer goes (leaves, times out, is refused by on_auth, has
   * the wrong key), wait for the next one on the same port: on_state reports
   * RDA_STATE_DISCONNECTED with the reason, then RDA_STATE_CONNECTING. Capture
   * keeps running and the next viewer starts from a keyframe; stats keep
   * counting. Works with rda_udp_bind, rda_tcp_bind and an unconnected
   * rda_udp_from_fd socket; with other transports the first disconnect stays
   * final. 0: the first disconnect is final. */
  int32_t keep_serving; /* 1 */

  /* 1.1, rda_host_start_capture() only. 1: also capture this process's own
   * windows (a status menu, a statistics panel: what an application shows
   * on the shared screen). 0: leave them out, so a viewer in the same
   * process does not film itself. macOS only; Linux captures everything. */
  int32_t capture_own_windows; /* 0 */
} rda_host_config;

typedef struct rda_host_stats {
  uint32_t struct_size;
  uint64_t frames_captured;
  uint64_t frames_skipped;  /* no tile changed */
  uint64_t frames_encoded;
  uint64_t video_frames;
  uint64_t keyframes;
  uint64_t keyframe_requests;
  uint64_t encode_errors;
  uint64_t bytes_sent;
  uint64_t video_bytes;
  uint64_t lossless_bytes;
  uint64_t lossless_tiles;
  uint64_t moved_tiles;
  uint64_t cached_tiles;
  uint64_t refreshed_tiles;
  uint64_t progressive_tiles;
  uint64_t region_pictures;
  uint64_t region_bytes;
  uint64_t regions_opened;
  uint64_t regions_closed;
  uint64_t region_encode_us_total;
  uint64_t frames_dropped_congestion;
  uint64_t frames_resubmitted; /* re-sent from memory on a static screen */
  int64_t target_bitrate_bps;
  int64_t bandwidth_estimate_bps;
  uint32_t rtt_us;
  uint64_t cursor_updates;
  uint64_t cursor_images;
  uint64_t cursor_bytes;
  uint64_t encode_us_total;
  uint64_t lossless_us_total;
  uint64_t resolution_changes; /* frames of a new size started a new stream */
  uint64_t encoder_drops;      /* pictures a video encoder (full screen or area)
                                  chose not to produce (the next one catches up) */
  /* 1.1: the transport's packet counters for the current viewer, sampled
   * with rtt_us (0 unless the transport measures the path, as UDP does).
   * packets_lost / packets_sent is the loss rate. */
  uint64_t packets_sent;
  uint64_t packets_lost; /* detected lost and resent; on very short round trips this
                           * includes late acknowledgements */
  /* 1.3: of packets_lost, acknowledged after all (late, not lost: jitter,
   * reordering). packets_lost - packets_spurious is real loss. */
  uint64_t packets_spurious;
} rda_host_stats;

RDA_API void rda_host_config_default(rda_host_config* c);

/* Creates a host sending over `transport`, which it takes over in every case
 * (also on failure). */
RDA_API rda_status rda_host_create(rda_transport* transport, const rda_host_config* config,
                                   rda_host** out);
/* Stops everything. No callback runs after it returns. Must not be called
 * from one of the host's callbacks. */
RDA_API void rda_host_destroy(rda_host* h);

/* Mode 1: the library captures the main display. RDA_ERR_PERMISSION without
 * the macOS Screen Recording permission. */
RDA_API rda_status rda_host_start_capture(rda_host* h);
RDA_API void rda_host_stop_capture(rda_host* h);

/* Mode 2: feed frames yourself. `dirty` (may be NULL) hints what changed.
 * A frame of another size than the last starts a new stream at that size
 * (new encoder, keyframe; the viewer gets on_stream_info again). Calls must
 * not overlap. */
RDA_API rda_status rda_host_submit_frame(rda_host* h, const rda_image* bgra,
                                         uint64_t capture_time_us, const rda_rect* dirty,
                                         size_t dirty_count);

/* The cursor, for applications that feed frames themselves. Thread-safe.
 * time_us 0 = now. The image is copied; resending the same one is free. */
RDA_API void rda_host_set_cursor(rda_host* h, int32_t x, int32_t y, int visible, uint64_t time_us);
RDA_API rda_status rda_host_set_cursor_image(rda_host* h, const rda_cursor_image* image);

/* The next encoded frame becomes a full refresh. */
RDA_API void rda_host_request_keyframe(rda_host* h);

RDA_API rda_state rda_host_get_state(const rda_host* h);
/* Sends an application message to the viewer. Thread-safe. RDA_ERR_BUSY
 * before the connection is up (wait for RDA_STATE_CONNECTED), RDA_ERR_CLOSED
 * once disconnected, RDA_ERR_INVALID_ARG for a reserved type or n above
 * RDA_MSG_MAX_SIZE. */
RDA_API rda_status rda_host_send_message(rda_host* h, uint32_t type, const uint8_t* payload,
                                         size_t n);

/* Set out->struct_size first. Cumulative counters. */
RDA_API rda_status rda_host_get_stats(const rda_host* h, rda_host_stats* out);

/* Encoder in use, copied into buf (always terminated); "" before the first
 * frame. Returns the full length. */
RDA_API size_t rda_host_encoder_name(const rda_host* h, char* buf, size_t buf_size);

/* ---- viewer ------------------------------------------------------------- */

typedef struct rda_viewer rda_viewer;

typedef struct rda_frame_info {
  uint32_t struct_size;
  uint64_t frame_id;
  uint64_t capture_time_us; /* host clock */
} rda_frame_info;

typedef struct rda_viewer_config {
  uint32_t struct_size;
  /* Callbacks (all optional), each called with `user`. Pointers are valid
   * only during the call. on_stream_info and on_frame run on the library's
   * decode thread, one at a time; on_cursor and on_message on the transport
   * receive thread, so the pointer never waits for a picture to decode:
   * they may run while on_frame does (guard what they share). on_state: see
   * below. */
  void* user;
  /* The host's screen size, when the stream starts. */
  void (*on_stream_info)(void* user, int32_t width, int32_t height);
  /* A decoded full-screen picture; `changed` lists what differs from the
   * previous one (count 0 = everything). */
  void (*on_frame)(void* user, const rda_image* bgra, const rda_rect* changed,
                   size_t changed_count, const rda_frame_info* info);
  /* The host's cursor moved, hid or changed its image. */
  void (*on_cursor)(void* user, const rda_cursor_state* state);

  /* Connection. */
  int32_t peer_timeout_ms;   /* 10000; silence from the host = gone; 0 = never */
  const uint8_t* auth_token; /* sent to the host's on_auth (copied); may be NULL */
  size_t auth_token_len;     /* at most 4096 */
  /* Connection changes, as for the host. May also run on the library's
   * timer thread (a timeout), so not necessarily one at a time with the
   * callbacks above. */
  void (*on_state)(void* user, rda_state state, rda_status reason);
  /* An application message from the host. Transport receive thread. */
  void (*on_message)(void* user, uint32_t type, const uint8_t* payload, size_t n);

  /* Encryption, as for the host: the same 32-byte key, or NULL. */
  const uint8_t* encryption_key;
  int32_t allow_plaintext; /* 0 */
} rda_viewer_config;

typedef struct rda_viewer_stats {
  uint32_t struct_size;
  uint64_t frames_decoded;
  uint64_t decode_errors;
  uint64_t bytes_received;
  uint64_t decode_us_total;
  uint64_t keyframe_requests;
} rda_viewer_stats;

RDA_API void rda_viewer_config_default(rda_viewer_config* c);

/* Creates a viewer on `transport` (taken over in every case). `transport` may
 * be NULL to play recordings with rda_viewer_feed_recording().
 * RDA_ERR_UNSUPPORTED if no HEVC decoder is available. */
RDA_API rda_status rda_viewer_create(rda_transport* transport, const rda_viewer_config* config,
                                     rda_viewer** out);
/* No callback runs after it returns. Not from a viewer callback. */
RDA_API void rda_viewer_destroy(rda_viewer* v);

/* Input to the host; dropped unless connected. */
RDA_API rda_status rda_viewer_send_input(rda_viewer* v, const rda_input_event* ev);
/* RDA_STATE_CONNECTING for a viewer without transport. */
RDA_API rda_state rda_viewer_get_state(const rda_viewer* v);
/* Sends an application message to the host; as rda_host_send_message(). */
RDA_API rda_status rda_viewer_send_message(rda_viewer* v, uint32_t type, const uint8_t* payload,
                                           size_t n);
/* Asks the host for a full refresh (the viewer also does it by itself after
 * decode errors). */
RDA_API void rda_viewer_request_keyframe(rda_viewer* v);
/* Decodes recording bytes synchronously on this thread. */
RDA_API rda_status rda_viewer_feed_recording(rda_viewer* v, const uint8_t* bytes, size_t n);
RDA_API rda_status rda_viewer_get_stats(const rda_viewer* v, rda_viewer_stats* out);
RDA_API size_t rda_viewer_decoder_name(const rda_viewer* v, char* buf, size_t buf_size);

/* ---- recordings ---------------------------------------------------------- */

/* Reads a whole .rdstream file. Free *bytes with rda_free(). */
RDA_API rda_status rda_recording_read(const char* path, uint8_t** bytes, size_t* n);
RDA_API void rda_free(void* p);

/* ---- testing and benchmarks ----------------------------------------------- */
/* Not needed in a product: a simulated network and a synthetic desktop, for
 * an application's own tests. They are part of the stable ABI all the same. */

/* A simulated network path running the UDP protocol. */
typedef struct rda_bandwidth_step {
  int64_t at_ms; /* milliseconds after creation */
  int64_t bandwidth_bps;
} rda_bandwidth_step;

typedef struct rda_network_conditions {
  uint32_t struct_size;
  int64_t bandwidth_bps; /* 0 = unlimited */
  int32_t delay_ms;      /* one way */
  int32_t jitter_ms;     /* extra random delay; reorders packets */
  double loss;           /* random loss probability per packet */
  int32_t queue_ms;      /* bottleneck buffer */
  const rda_bandwidth_step* schedule;
  size_t schedule_len;
  uint32_t seed;
} rda_network_conditions;

RDA_API void rda_network_conditions_default(rda_network_conditions* c);
/* `congestion_control` = 0 sends as fast as data comes, for comparison. */
RDA_API rda_status rda_simulated_network_create(const rda_network_conditions* host_to_viewer,
                                                const rda_network_conditions* viewer_to_host,
                                                int congestion_control, rda_transport** host_end,
                                                rda_transport** viewer_end);

/* Renders frame `index` of a deterministic synthetic desktop into `bgra`
 * (width * height * 4 bytes, stride width * 4). With `incremental` != 0 the
 * buffer must hold frame index - 1 and only the changes are drawn. Writes up
 * to `max_rects` changed areas and their total count to `*rect_count`. */
RDA_API rda_status rda_test_pattern_render(uint64_t index, int32_t width, int32_t height,
                                           uint8_t* bgra, int incremental, rda_rect* rects,
                                           size_t max_rects, size_t* rect_count);

/* Enums cross the ABI as 4-byte ints. Building with -fshort-enums (or
 * anything else that makes them smaller) would break it, so refuse. */
#if defined(__cplusplus) && __cplusplus >= 201103L
#  define RDA_STATIC_ASSERT_(c, m) static_assert(c, m)
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#  define RDA_STATIC_ASSERT_(c, m) _Static_assert(c, m)
#endif
#ifdef RDA_STATIC_ASSERT_
RDA_STATIC_ASSERT_(sizeof(rda_status) == 4, "rdaccel: enums must be 4 bytes");
RDA_STATIC_ASSERT_(sizeof(rda_log_level) == 4, "rdaccel: enums must be 4 bytes");
RDA_STATIC_ASSERT_(sizeof(rda_input_kind) == 4, "rdaccel: enums must be 4 bytes");
RDA_STATIC_ASSERT_(sizeof(rda_state) == 4, "rdaccel: enums must be 4 bytes");
RDA_STATIC_ASSERT_(sizeof(rda_channel) == 4, "rdaccel: enums must be 4 bytes");
#  undef RDA_STATIC_ASSERT_
#endif

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* RDACCEL_RDACCEL_H_ */
