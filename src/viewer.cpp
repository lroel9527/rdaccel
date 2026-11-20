#include "rdacore/viewer.h"

#include "platform.h"
#include "session/link.h"
#include "session/viewer_session.h"
#include "log/log.h"
#include "transport/secure_transport.h"
#if RDACCEL_HAVE_AUDIO
#include "audio/receiver.h"
#endif
#include "capture/capturer.h"
#include <cstring>

namespace rdacore {

struct Viewer::Impl {
#if RDACCEL_HAVE_AUDIO
  // The host's sound (ViewerOptions::audio). Outlives the transport, whose
  // receive thread feeds it, and the session, whose frames it times.
  std::unique_ptr<audio::Receiver> audio;
#endif
  std::unique_ptr<session::ViewerSession> session;
  // Handshake, keepalive, messages; passes the session its bytes.
  std::unique_ptr<session::Link> link;
  // Declared after `session` and `link`, so it is destroyed first: its
  // destructor stops the receive thread that calls into them.
  std::unique_ptr<Transport> transport;
};

Viewer::Viewer(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

Viewer::~Viewer() {
  if (impl_->link) impl_->link->Shutdown();  // Goodbye while the transport works
  impl_->session->Stop();                    // its decode thread may send
  impl_->transport.reset();
}

std::unique_ptr<Viewer> Viewer::Create(std::unique_ptr<Transport> transport,
                                       ViewerHandlers handlers, std::string* error) {
  return Create(std::move(transport), std::move(handlers), ViewerOptions{}, error);
}

std::unique_ptr<Viewer> Viewer::Create(std::unique_ptr<Transport> transport,
                                       ViewerHandlers handlers, ViewerOptions options,
                                       std::string* error) {
  std::string err;
  auto dec = platform::CreateVideoDecoder(&err);
  if (!dec) {
    if (error) *error = err;
    return nullptr;
  }
  auto impl = std::make_unique<Impl>();
#if RDACCEL_HAVE_AUDIO
  const bool audio = options.audio && transport && transport->CarriesDatagrams();
  if (audio) impl->audio = std::make_unique<audio::Receiver>();
  audio::Receiver* receiver = impl->audio.get();
#else
  const bool audio = false;
#endif
  impl->session = std::make_unique<session::ViewerSession>(nullptr, std::move(dec));
  impl->session->SetDecoderFactory([] { return platform::CreateVideoDecoder(nullptr); });
  if (handlers.on_stream_info) {
    impl->session->SetHelloHandler([fn = std::move(handlers.on_stream_info)](const proto::Hello& h) {
      fn(static_cast<int32_t>(h.width), static_cast<int32_t>(h.height));
    });
  }
  // When a picture was shown, for keeping the sound with it.
  std::function<void(uint64_t capture_time_us)> shown;
#if RDACCEL_HAVE_AUDIO
  if (receiver) shown = [receiver](uint64_t t) { receiver->NoteFrameShown(t, capture::MonotonicMicros()); };
#endif
  if (handlers.on_frame || shown) {
    impl->session->SetFrameHandler([fn = std::move(handlers.on_frame), shown](
                                       const tile::ImageView& img, const std::vector<Rect>& regions,
                                       uint64_t frame_id, uint64_t capture_time_us) {
      if (fn) fn(img, regions, FrameInfo{frame_id, capture_time_us});
      if (shown) shown(capture_time_us);
    });
  }
  if (handlers.on_cursor) impl->session->SetCursorHandler(std::move(handlers.on_cursor));
  transport::SecureTransport* secure = nullptr;
  if (transport && !options.encryption_key.empty()) {
    if (options.encryption_key.size() != transport::kSecureKeySize) {
      if (error) *error = "encryption_key must be 32 bytes";
      return nullptr;
    }
    auto s = std::make_unique<transport::SecureTransport>(
        std::move(transport), /*is_host=*/false,
        std::span<const uint8_t, transport::kSecureKeySize>(options.encryption_key.data(),
                                                            transport::kSecureKeySize));
    secure = s.get();
    transport = std::move(s);
  }
  // Handlers are in place before any byte can arrive.
  impl->transport = std::move(transport);
  if (impl->transport) {
    session::ViewerSession* vs = impl->session.get();
    impl->session->Attach(impl->transport.get(), /*own_receiver=*/false);
    session::Link::Handlers h;
    h.on_state = std::move(handlers.on_state);
    h.on_message = std::move(handlers.on_message);
    h.on_raw = [vs](Channel ch, std::span<const uint8_t> bytes) { vs->OnReceive(ch, bytes); };
    session::LinkOptions lo;
    lo.peer_timeout_ms = options.peer_timeout_ms;
    lo.auth_token = std::move(options.auth_token);
    impl->link = std::make_unique<session::Link>(session::Link::Role::kViewer, impl->transport.get(),
                                                 std::move(lo), std::move(h));
    session::Link* link = impl->link.get();
    link->SetAudio(audio);
#if RDACCEL_HAVE_AUDIO
    if (receiver) {
      impl->transport->SetDatagramReceiver(
          [receiver](std::span<const uint8_t> b) { receiver->OnPacket(b, capture::MonotonicMicros()); });
    }
#endif
    if (secure) {
      secure->SetErrorHandler([link](ConnectionError e, const std::string& msg) {
        log::Warn("viewer: " + msg);
        link->Abort(e);
      });
    }
    impl->transport->SetReceiver(
        [link](Channel ch, std::span<const uint8_t> bytes) { link->OnReceive(ch, bytes); });
    link->Start();
  }
  return std::unique_ptr<Viewer>(new Viewer(std::move(impl)));
}

bool Viewer::SendInput(const InputEvent& ev, std::string* error) {
  // The host ignores input from a viewer it has not accepted yet.
  if (impl_->link && impl_->link->state() != ConnectionState::kConnected) return true;
  if (ev.kind == InputKind::kText && impl_->link &&
      !(impl_->link->peer_caps() & proto::kCapTextInput)) {
    if (error) *error = "the host does not take text input (older version)";
    return false;
  }
  impl_->session->SendInput(ev);
  return true;
}

bool Viewer::SendMessage(uint32_t type, std::span<const uint8_t> payload, std::string* error) {
  if (!impl_->link) {
    if (error) *error = "no transport";
    return false;
  }
  return impl_->link->SendMessage(type, payload, error);
}

bool Viewer::SendBulkMessage(uint32_t type, std::span<const uint8_t> payload, std::string* error) {
  if (!impl_->link) {
    if (error) *error = "no transport";
    return false;
  }
  return impl_->link->SendBulkMessage(type, payload, error);
}

bool Viewer::SendInputMessage(uint32_t type, std::span<const uint8_t> payload, std::string* error) {
  if (!impl_->link) {
    if (error) *error = "no transport";
    return false;
  }
  return impl_->link->SendInputMessage(type, payload, error);
}

int64_t Viewer::BulkQueuedBytes() const { return impl_->link ? impl_->link->BulkQueuedBytes() : -1; }

ConnectionState Viewer::state() const {
  return impl_->link ? impl_->link->state() : ConnectionState::kConnecting;
}

void Viewer::RequestKeyframe() {
  if (impl_->link && impl_->link->disconnected()) return;
  impl_->session->RequestKeyframe();
}

void Viewer::FeedRecording(std::span<const uint8_t> bytes) {
  impl_->session->FeedVideoBytes(bytes);
}

std::string Viewer::decoder_name() const { return impl_->session->decoder_name(); }  // follows a codec change

ViewerStats Viewer::stats() const {
  ViewerStats st = impl_->session->stats();
#if RDACCEL_HAVE_AUDIO
  if (impl_->audio) {
    const auto a = impl_->audio->stats();
    st.audio_packets = a.packets;
    st.audio_bytes = a.bytes;
    st.audio_lost = a.lost;
    st.audio_late = a.late;
    st.audio_underruns = a.underruns;
    st.audio_buffer_ms = a.buffer_ms;
    st.audio_target_ms = a.target_ms;
    st.audio_drift_ppm = a.drift_ppm;
    st.av_offset_ms = a.av_offset_ms;
    st.bytes_received += a.bytes;
  }
#endif
  return st;
}

size_t Viewer::ReadAudio(float* out, size_t frames, uint64_t play_time_us) {
#if RDACCEL_HAVE_AUDIO
  if (impl_->audio) return impl_->audio->Read(out, frames, play_time_us, capture::MonotonicMicros());
#else
  (void)play_time_us;
#endif
  std::memset(out, 0, frames * 2 * sizeof(float));
  return 0;
}

}  // namespace rdacore
