#include "rdacore/viewer.h"

#include "platform.h"
#include "session/link.h"
#include "session/viewer_session.h"

namespace rdacore {

struct Viewer::Impl {
  std::unique_ptr<session::ViewerSession> session;
  // Handshake, keepalive, messages; passes the session its bytes.
  std::unique_ptr<session::Link> link;
  // Declared after `session` and `link`, so it is destroyed first: its
  // destructor stops the receive thread that calls into them.
  std::unique_ptr<Transport> transport;
  std::string decoder_name;
};

Viewer::Viewer(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

Viewer::~Viewer() {
  if (impl_->link) impl_->link->Shutdown();  // Goodbye while the transport works
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
  auto dec = platform::CreateHevcDecoder(&err);
  if (!dec) {
    if (error) *error = err;
    return nullptr;
  }
  auto impl = std::make_unique<Impl>();
  impl->decoder_name = dec->name();
  impl->session = std::make_unique<session::ViewerSession>(nullptr, std::move(dec));
  impl->session->SetDecoderFactory([] { return platform::CreateHevcDecoder(nullptr); });
  if (handlers.on_stream_info) {
    impl->session->SetHelloHandler([fn = std::move(handlers.on_stream_info)](const proto::Hello& h) {
      fn(static_cast<int32_t>(h.width), static_cast<int32_t>(h.height));
    });
  }
  if (handlers.on_frame) {
    impl->session->SetFrameHandler([fn = std::move(handlers.on_frame)](
                                       const tile::ImageView& img, const std::vector<Rect>& regions,
                                       uint64_t frame_id, uint64_t capture_time_us) {
      fn(img, regions, FrameInfo{frame_id, capture_time_us});
    });
  }
  if (handlers.on_cursor) impl->session->SetCursorHandler(std::move(handlers.on_cursor));
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
    impl->transport->SetReceiver(
        [link](Channel ch, std::span<const uint8_t> bytes) { link->OnReceive(ch, bytes); });
    link->Start();
  }
  return std::unique_ptr<Viewer>(new Viewer(std::move(impl)));
}

void Viewer::SendInput(const InputEvent& ev) {
  if (impl_->link && impl_->link->disconnected()) return;
  impl_->session->SendInput(ev);
}

bool Viewer::SendMessage(uint32_t type, std::span<const uint8_t> payload, std::string* error) {
  if (!impl_->link) {
    if (error) *error = "no transport";
    return false;
  }
  return impl_->link->SendMessage(type, payload, error);
}

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

std::string Viewer::decoder_name() const { return impl_->decoder_name; }

ViewerStats Viewer::stats() const { return impl_->session->stats(); }

}  // namespace rdacore
