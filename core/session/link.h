#pragma once

// The connection layer shared by Host and Viewer: the version handshake,
// keepalive and peer timeout, Goodbye on close, and application messages.
// It owns the transport's receive callback and parses the channels it is
// responsible for; everything else goes on to the session.
//
//   host side:    parses control, input and message; nothing is forwarded raw
//                 (the host session may be created later, mid-stream).
//   viewer side:  parses control (Hello) and message; control, video and
//                 cursor bytes are also forwarded raw to the viewer session.

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "proto/messages.h"
#include "rdacore/connection.h"
#include "transport/transport.h"

namespace rdacore::session {

using LinkState = ConnectionState;
using LinkError = ConnectionError;

const char* LinkErrorText(LinkError e);

struct LinkOptions {
  // No bytes from the peer for this long = disconnected. 0 = never.
  int32_t peer_timeout_ms = 10'000;
  int32_t ping_interval_ms = 1'000;
  // Viewer: sent in ViewerHello for the host's auth hook.
  std::vector<uint8_t> auth_token;
};

class Link {
 public:
  enum class Role { kHost, kViewer };

  struct Handlers {
    std::function<void(LinkState, LinkError)> on_state;
    std::function<void(uint32_t type, std::span<const uint8_t> payload)> on_message;
    // Host: decoded control and input messages Link does not consume
    // (InputEvent, KeyframeRequest, Feedback).
    std::function<void(const proto::Message&)> on_other;
    // Viewer: raw bytes for the session (control, video, cursor).
    std::function<void(transport::Channel, std::span<const uint8_t>)> on_raw;
    // Host: decides whether a viewer may connect (token from ViewerHello).
    std::function<bool(std::span<const uint8_t> token)> on_auth;
  };

  // Does not take ownership of `transport`, and does not install the receive
  // callback: call OnReceive from it.
  Link(Role role, transport::ITransport* transport, LinkOptions options, Handlers handlers);
  // Calls Shutdown().
  ~Link();
  Link(const Link&) = delete;
  Link& operator=(const Link&) = delete;

  // Starts the keepalive/timeout thread; the viewer also sends ViewerHello.
  void Start();

  void OnReceive(transport::Channel ch, std::span<const uint8_t> bytes);

  // Reliable, ordered application message. False (with `error`) when
  // disconnected, the type is reserved, or the payload is too large.
  bool SendMessage(uint32_t type, std::span<const uint8_t> payload, std::string* error);

  LinkState state() const { return state_.load(); }
  // The peer's capability bits (proto::kCap*), known once connected.
  uint64_t peer_caps() const { return peer_caps_.load(); }
  LinkError error() const;
  bool disconnected() const { return state_.load() == LinkState::kDisconnected; }

  // Closes from our side: Goodbye to the peer, state kDisconnected (kClosed).
  void Close();
  // Disconnects with `e` without telling the peer (the channel to it is
  // unusable, e.g. the encryption failed).
  void Abort(LinkError e) { SetState(LinkState::kDisconnected, e); }

  // For the owner's destructor, while the transport still works: stops the
  // timer, says Goodbye, and makes later events silent (no more callbacks).
  // Idempotent.
  void Shutdown();

 private:
  void Timer();
  void SetState(LinkState s, LinkError e);
  void Fail(LinkError e, proto::GoodbyeReason why);
  void HandleControl(proto::Message&& m);
  bool SendControl(const proto::Message& m);

  const Role role_;
  transport::ITransport* transport_;
  const LinkOptions options_;
  Handlers h_;

  std::atomic<LinkState> state_{LinkState::kConnecting};
  std::atomic<uint64_t> peer_caps_{0};
  mutable std::mutex state_mu_;     // guards error_ with state_ transitions
  std::recursive_mutex cb_mu_;      // serializes state callbacks
  LinkError error_ = LinkError::kNone;

  std::atomic<uint64_t> last_rx_us_{0};
  // Host on a listening transport (no peer at Start): when a peer turned up.
  // One that has not said ViewerHello within peer_timeout_ms is dropped, so
  // a stray sender cannot keep the place of the viewer.
  bool listening_ = false;
  uint64_t peer_since_us_ = 0;  // timer thread only
  proto::StreamParser control_parser_, input_parser_, message_parser_;
  std::mutex send_mu_;  // keeps a chunked AppMessage contiguous

  std::mutex timer_mu_;
  std::condition_variable timer_cv_;
  bool stop_ = false;
  std::atomic<bool> quiet_{false};
  std::thread timer_;
};

}  // namespace rdacore::session
