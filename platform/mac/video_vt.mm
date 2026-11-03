#include "mac/video_vt.h"

#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <VideoToolbox/VideoToolbox.h>

#include <cstring>
#include <string>
#include <vector>
#include <mutex>
#include <optional>

namespace rdacore::platform {
namespace {

std::string OsStatusText(const char* what, OSStatus st) {
  return std::string(what) + " failed (OSStatus " + std::to_string(st) + ")";
}

void SetProp(VTSessionRef s, CFStringRef key, CFTypeRef value) {
  // Unsupported properties are not fatal; the encoder just ignores them.
  VTSessionSetProperty(s, key, value);
}

void SetIntProp(VTSessionRef s, CFStringRef key, int64_t v) {
  CFNumberRef n = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt64Type, &v);
  SetProp(s, key, n);
  CFRelease(n);
}

// ---------------------------------------------------------------------------
// Encoder
// ---------------------------------------------------------------------------

class VtHevcEncoder final : public codec::IHevcEncoder {
 public:
  ~VtHevcEncoder() override {
    if (session_) {
      VTCompressionSessionCompleteFrames(session_, kCMTimeInvalid);
      VTCompressionSessionInvalidate(session_);
      CFRelease(session_);
    }
  }

  // Session flavours, best first. kAny lets VideoToolbox pick any encoder,
  // including a software one where the OS has it.
  enum class Mode { kLowLatencyHardware, kHardware, kAny };

  bool Init(const codec::EncoderConfig& cfg, Mode mode, std::string* error) {
    cfg_ = cfg;
    NSDictionary* source_attrs = @{
      (id)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_32BGRA),
      (id)kCVPixelBufferWidthKey : @(cfg.width),
      (id)kCVPixelBufferHeightKey : @(cfg.height),
      (id)kCVPixelBufferIOSurfacePropertiesKey : @{},
    };
    NSMutableDictionary* spec = [NSMutableDictionary dictionary];
    if (mode != Mode::kAny) {
      spec[(id)kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder] = @YES;
    }
    if (mode == Mode::kLowLatencyHardware) {
      spec[(id)kVTVideoEncoderSpecification_EnableLowLatencyRateControl] = @YES;
    }
    const OSStatus st = VTCompressionSessionCreate(
        kCFAllocatorDefault, cfg.width, cfg.height, kCMVideoCodecType_HEVC,
        (__bridge CFDictionaryRef)spec, (__bridge CFDictionaryRef)source_attrs, kCFAllocatorDefault,
        &VtHevcEncoder::OnOutput, this, &session_);
    if (st != noErr) {
      session_ = nullptr;
      if (error) *error = OsStatusText("VTCompressionSessionCreate(HEVC)", st);
      return false;
    }
    low_latency_ = mode == Mode::kLowLatencyHardware;
    CFBooleanRef hw = nullptr;
    if (VTSessionCopyProperty(session_, kVTCompressionPropertyKey_UsingHardwareAcceleratedVideoEncoder,
                              kCFAllocatorDefault, &hw) == noErr &&
        hw) {
      hardware_ = CFBooleanGetValue(hw);
      CFRelease(hw);
    } else {
      hardware_ = mode != Mode::kAny;
    }

    SetProp(session_, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);
    SetProp(session_, kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse);
    SetProp(session_, kVTCompressionPropertyKey_ProfileLevel, kVTProfileLevel_HEVC_Main_AutoLevel);
    // No periodic keyframes: the host asks for one when it needs it.
    SetIntProp(session_, kVTCompressionPropertyKey_MaxKeyFrameInterval, 1 << 30);
    SetIntProp(session_, kVTCompressionPropertyKey_ExpectedFrameRate, cfg.fps);
    SetIntProp(session_, kVTCompressionPropertyKey_AverageBitRate, cfg.bitrate_bps);
    SetProp(session_, kVTCompressionPropertyKey_ColorPrimaries, kCVImageBufferColorPrimaries_ITU_R_709_2);
    SetProp(session_, kVTCompressionPropertyKey_TransferFunction,
            kCVImageBufferTransferFunction_ITU_R_709_2);
    SetProp(session_, kVTCompressionPropertyKey_YCbCrMatrix, kCVImageBufferYCbCrMatrix_ITU_R_709_2);
    if (cfg.min_qp > 0) {
      SetIntProp(session_, kVTCompressionPropertyKey_MinAllowedFrameQP, cfg.min_qp);
    }
    VTCompressionSessionPrepareToEncodeFrames(session_);
    return true;
  }

  std::string name() const override {
    if (!hardware_) return "VideoToolbox HEVC (software)";
    return low_latency_ ? "VideoToolbox HEVC (low-latency)" : "VideoToolbox HEVC";
  }

  // Encodes one gray keyframe. Some machines (e.g. virtual Macs) create a
  // session but then fail every frame with kVTVideoEncoderNotAvailableNowErr,
  // so a session is only trusted after this succeeds.
  bool Probe(std::string* error) {
    std::vector<uint8_t> gray(static_cast<size_t>(cfg_.width) * cfg_.height * 4, 0x80);
    if (Encode({gray.data(), cfg_.width, cfg_.height, cfg_.width * 4}, 0, true)) return true;
    if (error) *error = error_.empty() ? "encoder produced no output" : error_;
    return false;
  }
  const std::string& last_error() const override { return error_; }

  void SetBitrate(int64_t bps) override {
    SetIntProp(session_, kVTCompressionPropertyKey_AverageBitRate, bps);
  }

  std::optional<codec::HevcAccessUnit> Encode(const tile::ImageView& img, uint64_t pts_us,
                                              bool force_keyframe) override {
    error_.clear();
    if (img.width != cfg_.width || img.height != cfg_.height) {
      error_ = "frame size changed; recreate the encoder";
      return std::nullopt;
    }
    CVPixelBufferRef pb = nullptr;
    CVPixelBufferPoolRef pool = VTCompressionSessionGetPixelBufferPool(session_);
    if (!pool || CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault, pool, &pb) != kCVReturnSuccess) {
      error_ = "could not get a pixel buffer from the encoder pool";
      return std::nullopt;
    }
    // M0 copies the frame; M1 will pass the captured IOSurface straight in.
    CVPixelBufferLockBaseAddress(pb, 0);
    auto* dst = static_cast<uint8_t*>(CVPixelBufferGetBaseAddress(pb));
    const size_t dst_stride = CVPixelBufferGetBytesPerRow(pb);
    const size_t row = static_cast<size_t>(img.width) * 4;
    for (int32_t y = 0; y < img.height; ++y) {
      std::memcpy(dst + y * dst_stride, img.data + static_cast<size_t>(y) * img.stride, row);
    }
    CVPixelBufferUnlockBaseAddress(pb, 0);

    NSDictionary* props = force_keyframe ? @{(id)kVTEncodeFrameOptionKey_ForceKeyFrame : @YES} : nil;
    {
      std::lock_guard<std::mutex> lock(mu_);
      pending_.reset();
      callback_error_.clear();
    }
    const CMTime pts = CMTimeMake(static_cast<int64_t>(pts_us), 1'000'000);
    OSStatus st = VTCompressionSessionEncodeFrame(session_, pb, pts, kCMTimeInvalid,
                                                  (__bridge CFDictionaryRef)props, nullptr, nullptr);
    CVPixelBufferRelease(pb);
    if (st != noErr) {
      error_ = OsStatusText("VTCompressionSessionEncodeFrame", st);
      return std::nullopt;
    }
    // Wait for this frame's output so Encode() is synchronous, which keeps
    // M0 simple. With low-latency mode this costs nothing extra.
    st = VTCompressionSessionCompleteFrames(session_, kCMTimeInvalid);
    if (st != noErr) {
      error_ = OsStatusText("VTCompressionSessionCompleteFrames", st);
      return std::nullopt;
    }
    std::lock_guard<std::mutex> lock(mu_);
    if (!callback_error_.empty()) error_ = callback_error_;
    auto out = std::move(pending_);
    pending_.reset();
    return out;
  }

 private:
  static void OnOutput(void* refcon, void*, OSStatus status, VTEncodeInfoFlags flags,
                       CMSampleBufferRef sample) {
    static_cast<VtHevcEncoder*>(refcon)->HandleOutput(status, flags, sample);
  }

  void HandleOutput(OSStatus status, VTEncodeInfoFlags flags, CMSampleBufferRef sample) {
    std::lock_guard<std::mutex> lock(mu_);
    if (status != noErr) {
      callback_error_ = OsStatusText("encoder callback", status);
      return;
    }
    if ((flags & kVTEncodeInfo_FrameDropped) || !sample) return;

    codec::HevcAccessUnit au;
    CMFormatDescriptionRef fmt = CMSampleBufferGetFormatDescription(sample);
    int nal_len_size = 4;
    size_t ps_count = 0;
    if (CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(fmt, 0, nullptr, nullptr, &ps_count,
                                                           &nal_len_size) != noErr) {
      callback_error_ = "could not read the HEVC format description";
      return;
    }

    CMBlockBufferRef block = CMSampleBufferGetDataBuffer(sample);
    const size_t total = CMBlockBufferGetDataLength(block);
    std::vector<uint8_t> raw(total);
    if (CMBlockBufferCopyDataBytes(block, 0, total, raw.data()) != kCMBlockBufferNoErr) {
      callback_error_ = "could not copy encoded data";
      return;
    }

    // Re-frame with 4-byte lengths (the wire format) and look at the NAL
    // types directly. The NotSync attachment is not reliable across encoders
    // (the software HEVC encoder does not mark its first frame), but an IRAP
    // NAL unit always means a keyframe. Parameter sets that an encoder puts
    // in-band are moved out, as the wire format carries them separately.
    size_t i = 0;
    while (i + static_cast<size_t>(nal_len_size) <= raw.size()) {
      size_t len = 0;
      for (int b = 0; b < nal_len_size; ++b) len = (len << 8) | raw[i + b];
      i += static_cast<size_t>(nal_len_size);
      if (len == 0 || len > raw.size() - i) break;
      const std::span<const uint8_t> nal(raw.data() + i, len);
      i += len;
      const uint8_t type = codec::HevcNalTypeOf(nal);
      if (codec::IsHevcParameterSet(type)) {
        au.parameter_sets.emplace_back(nal.begin(), nal.end());
        continue;
      }
      if (type == codec::kNalAud) continue;
      if (codec::IsHevcIrap(type)) au.keyframe = true;
      codec::AppendLengthPrefixed(nal, au.nal_data);
    }
    if (au.nal_data.empty()) {
      callback_error_ = "encoder output had no picture data";
      return;
    }
    if (!au.keyframe) {
      au.parameter_sets.clear();
    } else if (au.parameter_sets.empty()) {
      for (size_t k = 0; k < ps_count; ++k) {
        const uint8_t* ps = nullptr;
        size_t ps_size = 0;
        if (CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(fmt, k, &ps, &ps_size, nullptr,
                                                               nullptr) == noErr) {
          au.parameter_sets.emplace_back(ps, ps + ps_size);
        }
      }
    }
    pending_ = std::move(au);
  }

  codec::EncoderConfig cfg_;
  VTCompressionSessionRef session_ = nullptr;
  bool hardware_ = true;
  bool low_latency_ = false;
  std::mutex mu_;
  std::optional<codec::HevcAccessUnit> pending_;
  std::string callback_error_;
  std::string error_;
};

// ---------------------------------------------------------------------------
// Decoder
// ---------------------------------------------------------------------------

class VtHevcDecoder final : public codec::IHevcDecoder {
 public:
  ~VtHevcDecoder() override { Reset(); }

  std::string name() const override { return "VideoToolbox HEVC"; }
  const std::string& last_error() const override { return error_; }

  bool Decode(const std::vector<std::vector<uint8_t>>& parameter_sets,
              const std::vector<uint8_t>& nal_data, uint64_t pts_us,
              const codec::DecodedFrameFn& on_frame) override {
    if (!parameter_sets.empty() && !UpdateFormat(parameter_sets)) return false;
    if (!session_) {
      error_ = "no keyframe received yet";
      return false;
    }

    CMBlockBufferRef block = nullptr;
    // The block points at nal_data without copying; decoding is synchronous
    // so the data outlives its use.
    OSStatus st = CMBlockBufferCreateWithMemoryBlock(
        kCFAllocatorDefault, const_cast<uint8_t*>(nal_data.data()), nal_data.size(), kCFAllocatorNull,
        nullptr, 0, nal_data.size(), 0, &block);
    if (st != kCMBlockBufferNoErr) {
      error_ = OsStatusText("CMBlockBufferCreateWithMemoryBlock", st);
      return false;
    }
    CMSampleBufferRef sample = nullptr;
    const size_t size = nal_data.size();
    CMSampleTimingInfo timing = {kCMTimeInvalid, CMTimeMake(static_cast<int64_t>(pts_us), 1'000'000),
                                 kCMTimeInvalid};
    st = CMSampleBufferCreateReady(kCFAllocatorDefault, block, format_, 1, 1, &timing, 1, &size,
                                   &sample);
    CFRelease(block);
    if (st != noErr) {
      error_ = OsStatusText("CMSampleBufferCreateReady", st);
      return false;
    }

    decoded_ = nullptr;
    decode_status_ = noErr;
    VTDecodeInfoFlags info = 0;
    st = VTDecompressionSessionDecodeFrame(session_, sample, 0, nullptr, &info);
    CFRelease(sample);
    VTDecompressionSessionWaitForAsynchronousFrames(session_);
    if (st != noErr || decode_status_ != noErr) {
      error_ = OsStatusText("VTDecompressionSessionDecodeFrame", st != noErr ? st : decode_status_);
      if (decoded_) CVPixelBufferRelease(decoded_);
      decoded_ = nullptr;
      return false;
    }
    if (decoded_) {
      CVPixelBufferLockBaseAddress(decoded_, kCVPixelBufferLock_ReadOnly);
      tile::ImageView view{static_cast<const uint8_t*>(CVPixelBufferGetBaseAddress(decoded_)),
                           static_cast<int32_t>(CVPixelBufferGetWidth(decoded_)),
                           static_cast<int32_t>(CVPixelBufferGetHeight(decoded_)),
                           static_cast<int32_t>(CVPixelBufferGetBytesPerRow(decoded_))};
      on_frame(view, pts_us);
      CVPixelBufferUnlockBaseAddress(decoded_, kCVPixelBufferLock_ReadOnly);
      CVPixelBufferRelease(decoded_);
      decoded_ = nullptr;
    }
    return true;
  }

 private:
  static void OnDecoded(void* refcon, void*, OSStatus status, VTDecodeInfoFlags,
                        CVImageBufferRef image, CMTime, CMTime) {
    auto* self = static_cast<VtHevcDecoder*>(refcon);
    self->decode_status_ = status;
    if (status == noErr && image) self->decoded_ = CVPixelBufferRetain(image);
  }

  bool UpdateFormat(const std::vector<std::vector<uint8_t>>& ps) {
    std::vector<const uint8_t*> ptrs;
    std::vector<size_t> sizes;
    for (const auto& p : ps) {
      ptrs.push_back(p.data());
      sizes.push_back(p.size());
    }
    CMFormatDescriptionRef fmt = nullptr;
    OSStatus st = CMVideoFormatDescriptionCreateFromHEVCParameterSets(
        kCFAllocatorDefault, ptrs.size(), ptrs.data(), sizes.data(), 4, nullptr, &fmt);
    if (st != noErr) {
      error_ = OsStatusText("CMVideoFormatDescriptionCreateFromHEVCParameterSets", st);
      return false;
    }
    if (session_ && VTDecompressionSessionCanAcceptFormatDescription(session_, fmt)) {
      CFRelease(format_);
      format_ = fmt;
      return true;
    }
    Reset();
    format_ = fmt;
    NSDictionary* dest = @{
      (id)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_32BGRA),
      (id)kCVPixelBufferIOSurfacePropertiesKey : @{},
    };
    VTDecompressionOutputCallbackRecord cb = {&VtHevcDecoder::OnDecoded, this};
    st = VTDecompressionSessionCreate(kCFAllocatorDefault, format_, nullptr,
                                      (__bridge CFDictionaryRef)dest, &cb, &session_);
    if (st != noErr) {
      session_ = nullptr;
      error_ = OsStatusText("VTDecompressionSessionCreate", st);
      return false;
    }
    SetProp(session_, kVTDecompressionPropertyKey_RealTime, kCFBooleanTrue);
    return true;
  }

  void Reset() {
    if (session_) {
      VTDecompressionSessionWaitForAsynchronousFrames(session_);
      VTDecompressionSessionInvalidate(session_);
      CFRelease(session_);
      session_ = nullptr;
    }
    if (format_) {
      CFRelease(format_);
      format_ = nullptr;
    }
  }

  VTDecompressionSessionRef session_ = nullptr;
  CMFormatDescriptionRef format_ = nullptr;
  CVPixelBufferRef decoded_ = nullptr;
  OSStatus decode_status_ = noErr;
  std::string error_;
};

}  // namespace

std::unique_ptr<codec::IHevcEncoder> CreateVtHevcEncoder(const codec::EncoderConfig& cfg,
                                                         std::string* error) {
  using Mode = VtHevcEncoder::Mode;
  std::string errors;
  for (Mode mode : {Mode::kLowLatencyHardware, Mode::kHardware, Mode::kAny}) {
    std::string err;
    auto probe = std::make_unique<VtHevcEncoder>();
    if (probe->Init(cfg, mode, &err) && probe->Probe(&err)) {
      // Fresh session for the real stream, so it starts at frame 0.
      probe.reset();
      auto enc = std::make_unique<VtHevcEncoder>();
      if (enc->Init(cfg, mode, &err)) return enc;
    }
    static const char* kNames[] = {"low-latency hardware", "hardware", "any"};
    if (!errors.empty()) errors += "; ";
    errors += std::string(kNames[static_cast<int>(mode)]) + ": " + err;
  }
  if (error) *error = "no usable VideoToolbox HEVC encoder (" + errors + ")";
  return nullptr;
}

std::unique_ptr<codec::IHevcDecoder> CreateVtHevcDecoder(std::string*) {
  return std::make_unique<VtHevcDecoder>();
}

}  // namespace rdacore::platform
