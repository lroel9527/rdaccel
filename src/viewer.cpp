#include "rdaccel/viewer.h"

#include "platform.h"
#include "session/viewer_session.h"

namespace rdaccel {

struct Viewer::Impl {
  std::unique_ptr<session::ViewerSession> session;
  // Declared after `session`, so it is destroyed first: its destructor stops
  // the receive thread that calls into the session.
  std::unique_ptr<Transport> transport;
  std::string decoder_name;
};

Viewer::Viewer(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

Viewer::~Viewer() { impl_->transport.reset(); }

std::unique_ptr<Viewer> Viewer::Create(std::unique_ptr<Transport> transport,
                                       ViewerHandlers handlers, std::string* error) {
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
  if (impl->transport) impl->session->Attach(impl->transport.get());
  return std::unique_ptr<Viewer>(new Viewer(std::move(impl)));
}

void Viewer::SendInput(const InputEvent& ev) { impl_->session->SendInput(ev); }

void Viewer::FeedRecording(std::span<const uint8_t> bytes) {
  impl_->session->FeedVideoBytes(bytes);
}

std::string Viewer::decoder_name() const { return impl_->decoder_name; }

ViewerStats Viewer::stats() const { return impl_->session->stats(); }

}  // namespace rdaccel
