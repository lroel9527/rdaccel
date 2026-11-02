#include "linux/video_ffmpeg.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace rdaccel::platform {
namespace {

std::string AvError(int rc) {
  char buf[AV_ERROR_MAX_STRING_SIZE] = {};
  av_strerror(rc, buf, sizeof(buf));
  return buf;
}

struct CodecCtxDeleter {
  void operator()(AVCodecContext* c) const { avcodec_free_context(&c); }
};
struct FrameDeleter {
  void operator()(AVFrame* f) const { av_frame_free(&f); }
};
struct PacketDeleter {
  void operator()(AVPacket* p) const { av_packet_free(&p); }
};
struct SwsDeleter {
  void operator()(SwsContext* s) const { sws_freeContext(s); }
};

using CodecCtxPtr = std::unique_ptr<AVCodecContext, CodecCtxDeleter>;
using FramePtr = std::unique_ptr<AVFrame, FrameDeleter>;
using PacketPtr = std::unique_ptr<AVPacket, PacketDeleter>;
using SwsPtr = std::unique_ptr<SwsContext, SwsDeleter>;

// Both ends use BT.709 limited range, which is also what VideoToolbox uses for
// HD content, so colours match across platforms.
void SetBt709(SwsContext* sws, bool rgb_to_yuv) {
  const int* coeffs = sws_getCoefficients(SWS_CS_ITU709);
  // src_range/dst_range: 1 = full (JPEG), 0 = limited (MPEG).
  if (rgb_to_yuv) {
    sws_setColorspaceDetails(sws, coeffs, 1, coeffs, 0, 0, 1 << 16, 1 << 16);
  } else {
    sws_setColorspaceDetails(sws, coeffs, 0, coeffs, 1, 0, 1 << 16, 1 << 16);
  }
}

class FfmpegHevcEncoder final : public codec::IHevcEncoder {
 public:
  static std::unique_ptr<FfmpegHevcEncoder> Open(const std::string& backend,
                                                 const codec::EncoderConfig& cfg,
                                                 std::string* error) {
    const AVCodec* codec = avcodec_find_encoder_by_name(backend.c_str());
    if (!codec) {
      if (error) *error = backend + ": not available in this FFmpeg build";
      return nullptr;
    }
    CodecCtxPtr ctx(avcodec_alloc_context3(codec));
    ctx->width = cfg.width;
    ctx->height = cfg.height;
    ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    ctx->time_base = {1, 1'000'000};  // pts in microseconds
    ctx->framerate = {cfg.fps, 1};
    ctx->bit_rate = cfg.bitrate_bps;
    ctx->rc_max_rate = cfg.bitrate_bps;
    // A VBV of about half a second keeps single frames from bursting.
    ctx->rc_buffer_size = static_cast<int>(cfg.bitrate_bps / 2);
    ctx->max_b_frames = 0;
    ctx->gop_size = 1 << 30;  // no periodic keyframes; we request them
    ctx->colorspace = AVCOL_SPC_BT709;
    ctx->color_primaries = AVCOL_PRI_BT709;
    ctx->color_trc = AVCOL_TRC_BT709;
    ctx->color_range = AVCOL_RANGE_MPEG;
    ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    if (cfg.min_qp > 0) ctx->qmin = cfg.min_qp;  // NVENC; x265 gets it below

    if (backend == "libx265") {
      av_opt_set(ctx->priv_data, "preset", "ultrafast", 0);
      av_opt_set(ctx->priv_data, "tune", "zerolatency", 0);
      av_opt_set_int(ctx->priv_data, "forced-idr", 1, 0);
      // repeat-headers: VPS/SPS/PPS in-band on every keyframe, which is what
      // the wire format expects. keyint=-1: infinite GOP.
      std::string params = "repeat-headers=1:keyint=-1:scenecut=0:info=0:log-level=error";
      if (cfg.min_qp > 0) params += ":qpmin=" + std::to_string(cfg.min_qp);
      av_opt_set(ctx->priv_data, "x265-params", params.c_str(), 0);
    } else if (backend == "hevc_nvenc") {
      av_opt_set(ctx->priv_data, "preset", "p1", 0);
      av_opt_set(ctx->priv_data, "tune", "ull", 0);
      av_opt_set_int(ctx->priv_data, "zerolatency", 1, 0);
      av_opt_set_int(ctx->priv_data, "forced-idr", 1, 0);
    }

    if (const int rc = avcodec_open2(ctx.get(), codec, nullptr); rc < 0) {
      if (error) *error = backend + ": avcodec_open2 failed: " + AvError(rc);
      return nullptr;
    }
    auto enc = std::unique_ptr<FfmpegHevcEncoder>(new FfmpegHevcEncoder());
    enc->backend_ = backend;
    enc->ctx_ = std::move(ctx);
    enc->frame_.reset(av_frame_alloc());
    enc->frame_->format = AV_PIX_FMT_YUV420P;
    enc->frame_->width = cfg.width;
    enc->frame_->height = cfg.height;
    if (av_frame_get_buffer(enc->frame_.get(), 0) < 0) {
      if (error) *error = "av_frame_get_buffer failed";
      return nullptr;
    }
    enc->packet_.reset(av_packet_alloc());
    enc->sws_.reset(sws_getContext(cfg.width, cfg.height, AV_PIX_FMT_BGRA, cfg.width, cfg.height,
                                   AV_PIX_FMT_YUV420P, SWS_BILINEAR, nullptr, nullptr, nullptr));
    SetBt709(enc->sws_.get(), true);
    return enc;
  }

  std::string name() const override { return backend_; }
  const std::string& last_error() const override { return error_; }

  void SetBitrate(int64_t bps) override {
    ctx_->bit_rate = bps;
    ctx_->rc_max_rate = bps;
  }

  std::optional<codec::HevcAccessUnit> Encode(const tile::ImageView& bgra, uint64_t pts_us,
                                              bool force_keyframe) override {
    error_.clear();
    if (bgra.width != ctx_->width || bgra.height != ctx_->height) {
      error_ = "frame size changed; recreate the encoder";
      return std::nullopt;
    }
    if (av_frame_make_writable(frame_.get()) < 0) {
      error_ = "av_frame_make_writable failed";
      return std::nullopt;
    }
    const uint8_t* src[1] = {bgra.data};
    const int src_stride[1] = {bgra.stride};
    sws_scale(sws_.get(), src, src_stride, 0, bgra.height, frame_->data, frame_->linesize);

    frame_->pts = static_cast<int64_t>(pts_us);
    frame_->pict_type = force_keyframe ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
    if (force_keyframe) {
      frame_->flags |= AV_FRAME_FLAG_KEY;
    } else {
      frame_->flags &= ~AV_FRAME_FLAG_KEY;
    }

    if (const int rc = avcodec_send_frame(ctx_.get(), frame_.get()); rc < 0) {
      error_ = "avcodec_send_frame: " + AvError(rc);
      return std::nullopt;
    }
    std::vector<uint8_t> annexb;
    for (;;) {
      const int rc = avcodec_receive_packet(ctx_.get(), packet_.get());
      if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) break;
      if (rc < 0) {
        error_ = "avcodec_receive_packet: " + AvError(rc);
        return std::nullopt;
      }
      annexb.insert(annexb.end(), packet_->data, packet_->data + packet_->size);
      av_packet_unref(packet_.get());
    }
    if (annexb.empty()) return std::nullopt;  // encoder is buffering
    return codec::AnnexBToAccessUnit(annexb);
  }

 private:
  FfmpegHevcEncoder() = default;

  std::string backend_;
  CodecCtxPtr ctx_;
  FramePtr frame_;
  PacketPtr packet_;
  SwsPtr sws_;
  std::string error_;
};

class FfmpegHevcDecoder final : public codec::IHevcDecoder {
 public:
  static std::unique_ptr<FfmpegHevcDecoder> Open(std::string* error) {
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_HEVC);
    if (!codec) {
      if (error) *error = "FFmpeg has no HEVC decoder";
      return nullptr;
    }
    CodecCtxPtr ctx(avcodec_alloc_context3(codec));
    ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    if (cfg.min_qp > 0) ctx->qmin = cfg.min_qp;  // NVENC; x265 gets it below
    // Frame threading adds a frame of delay per thread; slices don't.
    ctx->thread_type = FF_THREAD_SLICE;
    if (const int rc = avcodec_open2(ctx.get(), codec, nullptr); rc < 0) {
      if (error) *error = "avcodec_open2(hevc decoder): " + AvError(rc);
      return nullptr;
    }
    auto dec = std::unique_ptr<FfmpegHevcDecoder>(new FfmpegHevcDecoder());
    dec->ctx_ = std::move(ctx);
    dec->frame_.reset(av_frame_alloc());
    dec->packet_.reset(av_packet_alloc());
    return dec;
  }

  std::string name() const override { return "FFmpeg hevc"; }
  const std::string& last_error() const override { return error_; }

  bool Decode(const std::vector<std::vector<uint8_t>>& parameter_sets,
              const std::vector<uint8_t>& nal_data, uint64_t pts_us,
              const codec::DecodedFrameFn& on_frame) override {
    annexb_.clear();
    if (!codec::AccessUnitToAnnexB(parameter_sets, nal_data, annexb_)) {
      error_ = "malformed length-prefixed NAL data";
      return false;
    }
    // FFmpeg requires zeroed padding after the packet data.
    annexb_.resize(annexb_.size() + AV_INPUT_BUFFER_PADDING_SIZE, 0);
    packet_->data = annexb_.data();
    packet_->size = static_cast<int>(annexb_.size() - AV_INPUT_BUFFER_PADDING_SIZE);
    packet_->pts = static_cast<int64_t>(pts_us);
    const int rc = avcodec_send_packet(ctx_.get(), packet_.get());
    packet_->data = nullptr;
    packet_->size = 0;
    if (rc < 0) {
      error_ = "avcodec_send_packet: " + AvError(rc);
      return false;
    }
    for (;;) {
      const int r = avcodec_receive_frame(ctx_.get(), frame_.get());
      if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) break;
      if (r < 0) {
        error_ = "avcodec_receive_frame: " + AvError(r);
        return false;
      }
      Emit(on_frame);
      av_frame_unref(frame_.get());
    }
    return true;
  }

 private:
  FfmpegHevcDecoder() = default;

  void Emit(const codec::DecodedFrameFn& on_frame) {
    const int w = frame_->width, h = frame_->height;
    const auto fmt = static_cast<AVPixelFormat>(frame_->format);
    // Convert with the matrix and range the stream says it uses (its VUI).
    // Streams from other encoders (e.g. VideoToolbox's software encoder) do
    // not always use BT.709 limited, which is what our own encoder writes and
    // what we assume when the stream says nothing.
    const AVColorSpace spc = frame_->colorspace;
    const bool full_range = frame_->color_range == AVCOL_RANGE_JPEG;
    if (!sws_ || w != sws_w_ || h != sws_h_ || fmt != sws_fmt_ || spc != sws_spc_ ||
        full_range != sws_full_) {
      sws_.reset(sws_getContext(w, h, fmt, w, h, AV_PIX_FMT_BGRA, SWS_BILINEAR, nullptr, nullptr,
                                nullptr));
      int cs = SWS_CS_ITU709;
      if (spc == AVCOL_SPC_BT470BG || spc == AVCOL_SPC_SMPTE170M) cs = SWS_CS_ITU601;
      if (spc == AVCOL_SPC_BT2020_NCL || spc == AVCOL_SPC_BT2020_CL) cs = SWS_CS_BT2020;
      const int* coeffs = sws_getCoefficients(cs);
      sws_setColorspaceDetails(sws_.get(), coeffs, full_range ? 1 : 0, coeffs, 1, 0, 1 << 16,
                               1 << 16);
      if (std::getenv("RDACCEL_DEBUG")) {
        std::fprintf(stderr, "rdaccel: decoding %dx%d %s, colorspace %s, %s range\n", w, h,
                     av_get_pix_fmt_name(fmt), av_color_space_name(spc),
                     full_range ? "full" : "limited");
      }
      sws_w_ = w;
      sws_h_ = h;
      sws_fmt_ = fmt;
      sws_spc_ = spc;
      sws_full_ = full_range;
      bgra_.assign(static_cast<size_t>(w) * h * 4, 0);
    }
    uint8_t* dst[1] = {bgra_.data()};
    const int dst_stride[1] = {w * 4};
    sws_scale(sws_.get(), frame_->data, frame_->linesize, 0, h, dst, dst_stride);
    on_frame({bgra_.data(), w, h, w * 4}, static_cast<uint64_t>(frame_->pts));
  }

  CodecCtxPtr ctx_;
  FramePtr frame_;
  PacketPtr packet_;
  SwsPtr sws_;
  int sws_w_ = 0, sws_h_ = 0;
  AVPixelFormat sws_fmt_ = AV_PIX_FMT_NONE;
  AVColorSpace sws_spc_ = AVCOL_SPC_UNSPECIFIED;
  bool sws_full_ = false;
  std::vector<uint8_t> annexb_;
  std::vector<uint8_t> bgra_;
  std::string error_;
};

}  // namespace

std::unique_ptr<codec::IHevcEncoder> CreateFfmpegHevcEncoder(const codec::EncoderConfig& cfg,
                                                             const std::string& prefer,
                                                             std::string* error) {
  std::vector<std::string> order = {"hevc_nvenc", "libx265"};
  if (!prefer.empty()) {
    order.erase(std::remove(order.begin(), order.end(), prefer), order.end());
    order.insert(order.begin(), prefer);
  }
  std::string errors;
  for (const auto& backend : order) {
    std::string err;
    if (auto enc = FfmpegHevcEncoder::Open(backend, cfg, &err)) return enc;
    errors += err + "; ";
  }
  if (error) *error = "no HEVC encoder could be opened: " + errors;
  return nullptr;
}

std::unique_ptr<codec::IHevcDecoder> CreateFfmpegHevcDecoder(std::string* error) {
  return FfmpegHevcDecoder::Open(error);
}

}  // namespace rdaccel::platform
