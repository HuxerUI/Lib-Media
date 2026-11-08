#include "codec/encoder/backend.h"

#include <algorithm>
#include <cstring>
#include <deque>
#include <mutex>
#include <type_traits>
#import <AudioToolbox/AudioToolbox.h>
#import <VideoToolbox/VideoToolbox.h>

namespace huxerui::media::detail {
namespace {
CodecError AudioConverterError(OSStatus status, const char* message) {
  return Error(CodecErrorCode::PlatformFailure, message, std::to_string(status));
}
class MacOSAudioEncoder final : public AudioEncoderBackend {
public:
  CodecResult<void> Open(const AudioEncoderConfig& config) override {
    config_ = config;
    config_.wakeup = {}; config_.cleanup_completed = {};
    AudioStreamBasicDescription input{}, output{};
    input.mSampleRate = config.input.sample_rate;
    input.mFormatID = kAudioFormatLinearPCM;
    input.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked | kAudioFormatFlagsNativeEndian;
    input.mChannelsPerFrame = config.input.ChannelCount();
    input.mBitsPerChannel = 16;
    input.mBytesPerFrame = input.mBytesPerPacket = input.mChannelsPerFrame * 2;
    input.mFramesPerPacket = 1;
    output.mSampleRate = input.mSampleRate;
    output.mFormatID = kAudioFormatMPEG4AAC;
    output.mFormatFlags = kMPEG4Object_AAC_LC;
    output.mChannelsPerFrame = input.mChannelsPerFrame;
    output.mFramesPerPacket = 1024;
    auto status = AudioConverterNew(&input, &output, &converter_);
    if (status) return CodecResult<void>::Failure(Error(CodecErrorCode::Unsupported, "Cannot create AAC AudioConverter", std::to_string(status)));
    UInt32 bitrate = config.bitrate;
    status = AudioConverterSetProperty(converter_, kAudioConverterEncodeBitRate, sizeof(bitrate), &bitrate);
    if (status) return CodecResult<void>::Failure(Error(CodecErrorCode::Unsupported, "AudioConverter rejected AAC bitrate", std::to_string(status)));
    UInt32 size = sizeof(bitrate);
    status = AudioConverterGetProperty(converter_, kAudioConverterEncodeBitRate, &size, &bitrate);
    if (status || bitrate != config.bitrate)
      return CodecResult<void>::Failure(Error(CodecErrorCode::Unsupported, "AudioConverter changed AAC bitrate", std::to_string(status)));
    size = sizeof(output);
    status = AudioConverterGetProperty(converter_, kAudioConverterCurrentOutputStreamDescription, &size, &output);
    if (status || output.mFormatID != kAudioFormatMPEG4AAC || output.mSampleRate != input.mSampleRate ||
        output.mChannelsPerFrame != input.mChannelsPerFrame || output.mFramesPerPacket != 1024)
      return CodecResult<void>::Failure(Error(CodecErrorCode::Unsupported, "AudioConverter changed AAC stream format", std::to_string(status)));
    UInt32 maximum = 0; size = sizeof(maximum);
    status = AudioConverterGetProperty(converter_, kAudioConverterPropertyMaximumOutputPacketSize, &size, &maximum);
    if (status || !maximum || maximum > kAudioEncoderPacketBytes)
      return CodecResult<void>::Failure(Error(CodecErrorCode::ResourceExhausted, "AudioConverter AAC scratch exceeds 64 KiB", std::to_string(status)));
    scratch_.resize(maximum);
    input_.reserve(4096); borrowed_.reserve(4096);
    priming_ = ReadPriming();
    return CodecResult<void>::Success();
  }
  CodecResult<SubmitStatus> Submit(std::span<const std::byte> pcm, std::uint32_t, std::int64_t) override {
    if (input_offset_ < input_.size()) return CodecResult<SubmitStatus>::Success(SubmitStatus::WouldBlock);
    input_.assign(pcm.begin(), pcm.end()); input_offset_ = 0;
    return CodecResult<SubmitStatus>::Success(SubmitStatus::Accepted);
  }
  CodecResult<std::optional<AudioEncoderBackendOutput>> Poll() override {
    using R = CodecResult<std::optional<AudioEncoderBackendOutput>>;
    if (eos_) return R::Success(std::nullopt);
    if (!described_) { described_ = true; return R::Success(DescribeAac(config_)); }
    if (input_offset_ == input_.size() && !finished_) return R::Success(std::nullopt);
    AudioBufferList output{};
    output.mNumberBuffers = 1;
    output.mBuffers[0] = {config_.input.ChannelCount(), static_cast<UInt32>(scratch_.size()), scratch_.data()};
    AudioStreamPacketDescription description{};
    UInt32 packets = 1;
    const auto status = AudioConverterFillComplexBuffer(converter_, Input, this, &packets, &output, &description);
    if (status && status != NeedInput) return R::Failure(AudioConverterError(status, "AudioConverter AAC encoding failed"));
    if (packets) {
      if (packets != 1 || description.mStartOffset < 0 || !description.mDataByteSize ||
          std::uint64_t(description.mStartOffset) > scratch_.size() ||
          description.mDataByteSize > scratch_.size() - std::size_t(description.mStartOffset) ||
          (description.mVariableFramesInPacket && description.mVariableFramesInPacket != 1024))
        return R::Failure(Error(CodecErrorCode::InvalidData, "AudioConverter returned an invalid AAC packet"));
      const auto first = scratch_.begin() + description.mStartOffset;
      const auto delay = priming_.value_or(0);
      if (encoded_samples_ > INT64_MAX || delay > INT64_MAX)
        return R::Failure(Error(CodecErrorCode::InvalidData, "AudioConverter AAC timeline overflow"));
      EncodedPacket packet{Own(Bytes(first, first + description.mDataByteSize)),
          {std::int64_t(encoded_samples_) - std::int64_t(delay), config_.input.sample_rate}, std::nullopt, std::nullopt, true};
      encoded_samples_ += 1024;
      return R::Success(std::move(packet));
    }
    if (finished_ && input_offset_ == input_.size() && status != NeedInput) {
      const auto final_priming = ReadPriming();
      if (priming_ && final_priming && priming_ != final_priming)
        return R::Failure(Error(CodecErrorCode::InvalidData, "AudioConverter changed AAC priming during encoding"));
      eos_ = true;
      // Packet rounding and library fill are included by the shared final sample accounting.
      return R::Success(AudioEncoderEnd{final_priming, std::nullopt});
    }
    return R::Success(std::nullopt);
  }
  CodecResult<SubmitStatus> Finish() override {
    finished_ = true;
    return CodecResult<SubmitStatus>::Success(SubmitStatus::Accepted);
  }
  CodecResult<void> Close() override {
    OSStatus status = noErr;
    if (converter_) { status = AudioConverterDispose(converter_); converter_ = nullptr; }
    Bytes{}.swap(input_); Bytes{}.swap(borrowed_); Bytes{}.swap(scratch_);
    return status ? CodecResult<void>::Failure(AudioConverterError(status, "Cannot dispose AAC AudioConverter")) : CodecResult<void>::Success();
  }
private:
  static constexpr OSStatus NeedInput = 'wait';
  static OSStatus Input(AudioConverterRef, UInt32* packets, AudioBufferList* buffers,
      AudioStreamPacketDescription**, void* context) {
    auto& self = *static_cast<MacOSAudioEncoder*>(context);
    self.borrowed_.clear();
    const auto stride = self.config_.input.ChannelCount() * 2;
    const auto samples = std::min<std::size_t>(*packets, (self.input_.size() - self.input_offset_) / stride);
    *packets = static_cast<UInt32>(samples);
    buffers->mNumberBuffers = 1;
    if (!samples) {
      buffers->mBuffers[0] = {self.config_.input.ChannelCount(), 0, nullptr};
      return self.finished_ ? noErr : NeedInput;
    }
    // Core Audio may borrow these bytes until the next invocation of the input procedure.
    const auto first = self.input_.begin() + self.input_offset_;
    self.borrowed_.assign(first, first + samples * stride);
    self.input_offset_ += samples * stride;
    buffers->mBuffers[0] = {self.config_.input.ChannelCount(), static_cast<UInt32>(self.borrowed_.size()), self.borrowed_.data()};
    return noErr;
  }
  std::optional<std::uint64_t> ReadPriming() const {
    AudioConverterPrimeInfo info{};
    UInt32 size = sizeof(info);
    if (AudioConverterGetProperty(converter_, kAudioConverterPrimeInfo, &size, &info) != noErr) return std::nullopt;
    return info.leadingFrames;
  }
  AudioEncoderConfig config_;
  AudioConverterRef converter_ = nullptr;
  Bytes input_, borrowed_, scratch_;
  std::size_t input_offset_ = 0;
  std::uint64_t encoded_samples_ = 0;
  std::optional<std::uint64_t> priming_;
  bool described_ = false, finished_ = false, eos_ = false;
};
} // namespace
std::unique_ptr<AudioEncoderBackend> MakeAudioEncoderBackend() { return std::make_unique<MacOSAudioEncoder>(); }
namespace {
struct VideoToolboxFailure { CodecError error; };
void VideoStatus(OSStatus status, const char* text, CodecErrorCode code = CodecErrorCode::PlatformFailure) {
  if (status) throw VideoToolboxFailure{Error(code, text, std::to_string(status))};
}
template<class T, class F> CodecResult<T> VideoGuard(F work) {
  try {
    if constexpr (std::is_void_v<T>) { work(); return CodecResult<void>::Success(); }
    else return CodecResult<T>::Success(work());
  } catch (const VideoToolboxFailure& failure) { return CodecResult<T>::Failure(failure.error); }
}
class MacOSVideoEncoder final : public VideoEncoderBackend {
public:
  CodecResult<void> Open(const VideoEncoderConfig& config) override {
    return VideoGuard<void>([&] {
      config_ = config; config_.wakeup = {}; config_.cleanup_completed = {};
      if (config.max_b_frames.value_or(0))
        throw VideoToolboxFailure{Error(CodecErrorCode::Unsupported, "VideoToolbox cannot enforce a numeric consecutive B-picture bound")};
      VideoStatus(VTCompressionSessionCreate(kCFAllocatorDefault, config.input.width, config.input.height,
          kCMVideoCodecType_H264, nullptr, nullptr, nullptr, Output, this, &session_),
          "Cannot create H.264 compression session", CodecErrorCode::Unsupported);
      const auto profile = config.profile == H264Profile::Baseline ? kVTProfileLevel_H264_Baseline_AutoLevel :
          config.profile == H264Profile::Main ? kVTProfileLevel_H264_Main_AutoLevel : kVTProfileLevel_H264_High_AutoLevel;
      Property(kVTCompressionPropertyKey_ProfileLevel, profile);
      const bool reorder = config.profile != H264Profile::Baseline && !config.max_b_frames;
      Property(kVTCompressionPropertyKey_AllowFrameReordering, reorder ? kCFBooleanTrue : kCFBooleanFalse);
      Property(kVTCompressionPropertyKey_AllowOpenGOP, kCFBooleanFalse);
      Integer(kVTCompressionPropertyKey_AverageBitRate, config.bitrate);
      Integer(kVTCompressionPropertyKey_MaxKeyFrameInterval, config.key_frame_interval);
      Integer(kVTCompressionPropertyKey_MaxFrameDelayCount, static_cast<std::int32_t>(VideoEncoderWindow(config) - 1));
      const double rate = double(config.frame_rate_numerator) / config.frame_rate_denominator;
      auto number = CFNumberCreate(kCFAllocatorDefault, kCFNumberDoubleType, &rate);
      if (!number) throw std::bad_alloc();
      const auto status = VTSessionSetProperty(session_, kVTCompressionPropertyKey_ExpectedFrameRate, number);
      CFRelease(number);
      VideoStatus(status, "Cannot configure H.264 frame rate", CodecErrorCode::Unsupported);
      if (config.input.primaries != ColorPrimaries::Unknown)
        Property(kVTCompressionPropertyKey_ColorPrimaries, Primaries(config.input.primaries));
      if (config.input.transfer != ColorTransfer::Unknown)
        Property(kVTCompressionPropertyKey_TransferFunction, Transfer(config.input.transfer));
      if (config.input.matrix != ColorMatrix::Unknown)
        Property(kVTCompressionPropertyKey_YCbCrMatrix, Matrix(config.input.matrix));
      CFTypeRef actual = nullptr;
      VideoStatus(VTSessionCopyProperty(session_, kVTCompressionPropertyKey_AverageBitRate, kCFAllocatorDefault, &actual),
          "Cannot verify H.264 bitrate", CodecErrorCode::Unsupported);
      std::int64_t bitrate = 0;
      const bool matches = actual && CFGetTypeID(actual) == CFNumberGetTypeID() &&
          CFNumberGetValue(static_cast<CFNumberRef>(actual), kCFNumberSInt64Type, &bitrate) && bitrate == config.bitrate;
      if (actual) CFRelease(actual);
      if (!matches) throw VideoToolboxFailure{Error(CodecErrorCode::Unsupported, "VideoToolbox changed the requested bitrate")};
      VideoStatus(VTCompressionSessionPrepareToEncodeFrames(session_), "Cannot prepare H.264 compression", CodecErrorCode::Unsupported);
      scratch_.resize(std::size_t(config.input.width) * config.input.height * 3 / 2);
    });
  }
  CodecResult<SubmitStatus> Submit(const VideoFrame& frame) override {
    return VideoGuard<SubmitStatus>([&] {
      {
        std::lock_guard lock(output_mutex_);
        if (failure_) throw VideoToolboxFailure{*failure_};
        if (pending_frames_ >= VideoEncoderWindow(config_)) return SubmitStatus::WouldBlock;
      }
      CVPixelBufferRef pixel = nullptr;
      const auto type = config_.input.range == ColorRange::Full ?
          kCVPixelFormatType_420YpCbCr8BiPlanarFullRange : kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
      auto surface = CFDictionaryCreate(kCFAllocatorDefault, nullptr, nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
      if (!surface) throw std::bad_alloc();
      const void* keys[]{kCVPixelBufferIOSurfacePropertiesKey};
      const void* values[]{surface};
      auto attributes = CFDictionaryCreate(kCFAllocatorDefault, keys, values, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
      if (surface) CFRelease(surface);
      if (!attributes) throw std::bad_alloc();
      const auto created = CVPixelBufferCreate(kCFAllocatorDefault, config_.input.width, config_.input.height, type, attributes, &pixel);
      if (attributes) CFRelease(attributes);
      VideoStatus(created, "Cannot allocate H.264 NV12 input");
      std::unique_ptr<std::remove_pointer_t<CVPixelBufferRef>, decltype(&CVPixelBufferRelease)> owner(pixel, CVPixelBufferRelease);
      VideoStatus(CVPixelBufferLockBaseAddress(pixel, 0), "Cannot map H.264 NV12 input");
      CopyVideoEncoderPixels(frame, VideoPixelFormat::NV12, scratch_);
      bool valid = CVPixelBufferGetPlaneCount(pixel) == 2;
      for (std::size_t plane = 0; valid && plane < 2; ++plane) {
        auto* target = static_cast<std::byte*>(CVPixelBufferGetBaseAddressOfPlane(pixel, plane));
        const auto stride = CVPixelBufferGetBytesPerRowOfPlane(pixel, plane);
        const auto rows = plane ? config_.input.height / 2 : config_.input.height;
        valid = target && stride >= config_.input.width && CVPixelBufferGetHeightOfPlane(pixel, plane) >= rows;
        if (valid) for (std::size_t y = 0; y < rows; ++y)
          std::memcpy(target + y * stride, scratch_.data() + (plane ? std::size_t(config_.input.width) * config_.input.height : 0) +
              y * config_.input.width, config_.input.width);
      }
      VideoStatus(CVPixelBufferUnlockBaseAddress(pixel, 0), "Cannot unmap H.264 NV12 input");
      if (!valid) throw VideoToolboxFailure{Error(CodecErrorCode::Unsupported, "VideoToolbox returned an unsupported input plane layout")};
      if (config_.input.primaries != ColorPrimaries::Unknown)
        CVBufferSetAttachment(pixel, kCVImageBufferColorPrimariesKey, Primaries(config_.input.primaries), kCVAttachmentMode_ShouldPropagate);
      if (config_.input.transfer != ColorTransfer::Unknown)
        CVBufferSetAttachment(pixel, kCVImageBufferTransferFunctionKey, Transfer(config_.input.transfer), kCVAttachmentMode_ShouldPropagate);
      if (config_.input.matrix != ColorMatrix::Unknown)
        CVBufferSetAttachment(pixel, kCVImageBufferYCbCrMatrixKey, Matrix(config_.input.matrix), kCVAttachmentMode_ShouldPropagate);
      CFDictionaryRef properties = nullptr;
      if (key_requested_) {
        const void* option_keys[]{kVTEncodeFrameOptionKey_ForceKeyFrame};
        const void* option_values[]{kCFBooleanTrue};
        properties = CFDictionaryCreate(kCFAllocatorDefault, option_keys, option_values, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        if (!properties) throw std::bad_alloc();
      }
      const auto duration = frame.duration ? frame.duration->Rescale(1000000) : std::optional<MediaTimestamp>{};
      {
        std::lock_guard lock(output_mutex_); ++pending_frames_;
      }
      VTEncodeInfoFlags flags = 0;
      const auto status = VTCompressionSessionEncodeFrame(session_, pixel, CMTimeMake(frame.pts.ticks, 1000000),
          duration ? CMTimeMake(duration->ticks, 1000000) : kCMTimeInvalid, properties, nullptr, &flags);
      if (properties) CFRelease(properties);
      if (status) {
        std::lock_guard lock(output_mutex_);
        if (pending_frames_) --pending_frames_;
        throw VideoToolboxFailure{Error(CodecErrorCode::PlatformFailure, "VideoToolbox rejected a picture", std::to_string(status))};
      }
      if (flags & kVTEncodeInfo_FrameDropped)
        throw VideoToolboxFailure{Error(CodecErrorCode::InvalidData, "VideoToolbox dropped an accepted picture")};
      key_requested_ = false;
      return SubmitStatus::Accepted;
    });
  }
  CodecResult<std::optional<VideoEncoderBackendOutput>> Poll() override {
    std::lock_guard lock(output_mutex_);
    using R = CodecResult<std::optional<VideoEncoderBackendOutput>>;
    if (failure_) return R::Failure(*failure_);
    if (!output_.empty()) { auto value = std::move(output_.front()); output_.pop_front(); return R::Success(std::move(value)); }
    if (finished_ && !pending_frames_ && !eos_) { eos_ = true; return R::Success(VideoEncoderEnd{}); }
    return R::Success(std::nullopt);
  }
  CodecResult<void> RequestKeyFrame() override { key_requested_ = true; return CodecResult<void>::Success(); }
  CodecResult<SubmitStatus> Finish() override {
    const auto status = VTCompressionSessionCompleteFrames(session_, kCMTimeInvalid);
    if (status) return CodecResult<SubmitStatus>::Failure(Error(CodecErrorCode::PlatformFailure, "Cannot drain H.264 compression", std::to_string(status)));
    finished_ = true;
    return CodecResult<SubmitStatus>::Success(SubmitStatus::Accepted);
  }
  CodecResult<void> Close() override {
    OSStatus status = noErr;
    {
      std::lock_guard lock(output_mutex_); closing_ = true;
    }
    if (session_) {
      status = VTCompressionSessionCompleteFrames(session_, kCMTimeInvalid);
      VTCompressionSessionInvalidate(session_); CFRelease(session_); session_ = nullptr;
    }
    {
      std::lock_guard lock(output_mutex_); output_.clear(); pending_frames_ = 0;
    }
    Bytes{}.swap(scratch_);
    return status ? CodecResult<void>::Failure(Error(CodecErrorCode::PlatformFailure, "Cannot retire H.264 compression", std::to_string(status))) :
        CodecResult<void>::Success();
  }
private:
  void Property(CFStringRef key, CFTypeRef value) {
    VideoStatus(VTSessionSetProperty(session_, key, value), "VideoToolbox rejected a requested H.264 control", CodecErrorCode::Unsupported);
  }
  void Integer(CFStringRef key, std::int32_t value) {
    auto number = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &value);
    if (!number) throw std::bad_alloc();
    const auto status = VTSessionSetProperty(session_, key, number); CFRelease(number);
    VideoStatus(status, "VideoToolbox rejected a numeric H.264 control", CodecErrorCode::Unsupported);
  }
  static CFStringRef Primaries(ColorPrimaries value) {
    return value == ColorPrimaries::BT709 ? kCVImageBufferColorPrimaries_ITU_R_709_2 :
        value == ColorPrimaries::BT601_525 ? kCVImageBufferColorPrimaries_SMPTE_C : kCVImageBufferColorPrimaries_EBU_3213;
  }
  static CFStringRef Transfer(ColorTransfer value) {
    return value == ColorTransfer::BT709 ? kCVImageBufferTransferFunction_ITU_R_709_2 :
        value == ColorTransfer::SRGB ? kCVImageBufferTransferFunction_sRGB : kCVImageBufferTransferFunction_Linear;
  }
  static CFStringRef Matrix(ColorMatrix value) {
    return value == ColorMatrix::BT709 ? kCVImageBufferYCbCrMatrix_ITU_R_709_2 : kCVImageBufferYCbCrMatrix_ITU_R_601_4;
  }
  static void Output(void* context, void*, OSStatus status, VTEncodeInfoFlags flags, CMSampleBufferRef sample) {
    auto& self = *static_cast<MacOSVideoEncoder*>(context);
    std::lock_guard lock(self.output_mutex_);
    if (self.pending_frames_) --self.pending_frames_;
    if (self.closing_ || self.failure_) return;
    try {
      if (status || !sample || (flags & kVTEncodeInfo_FrameDropped) || CMSampleBufferGetNumSamples(sample) != 1)
        throw VideoToolboxFailure{Error(CodecErrorCode::InvalidData, "VideoToolbox returned a failed, dropped, or incomplete picture", std::to_string(status))};
      if (self.output_.size() >= VideoEncoderWindow(self.config_) + 1)
        throw VideoToolboxFailure{Error(CodecErrorCode::ResourceExhausted, "VideoToolbox exceeded reserved output slots")};
      auto description = CMSampleBufferGetFormatDescription(sample);
      const std::uint8_t* parameter = nullptr; std::size_t size = 0, count = 0; int length = 0;
      VideoStatus(CMVideoFormatDescriptionGetH264ParameterSetAtIndex(description, 0, &parameter, &size, &count, &length), "Cannot read H.264 parameter sets");
      if (!count || count > 286) throw VideoToolboxFailure{Error(CodecErrorCode::InvalidData, "Unbounded H.264 parameter set count")};
      Bytes configuration;
      for (std::size_t i = 0; i < count; ++i) {
        VideoStatus(CMVideoFormatDescriptionGetH264ParameterSetAtIndex(description, i, &parameter, &size, nullptr, nullptr), "Cannot read H.264 parameter set");
        if (!parameter || !size || configuration.size() > 65532 || size > 65532 - configuration.size())
          throw VideoToolboxFailure{Error(CodecErrorCode::ResourceExhausted, "VideoToolbox H.264 configuration exceeds 64 KiB")};
        configuration.insert(configuration.end(), {std::byte{0}, std::byte{0}, std::byte{0}, std::byte{1}});
        const auto* first = reinterpret_cast<const std::byte*>(parameter);
        configuration.insert(configuration.end(), first, first + size);
      }
      auto described = ReadVideoEncodeFormat(configuration, self.config_);
      if (!described.Succeeded()) throw VideoToolboxFailure{described.Error()};
      if (self.format_ && *self.format_ != described.Value())
        throw VideoToolboxFailure{Error(CodecErrorCode::Unsupported, "VideoToolbox H.264 configuration changed")};
      auto buffer = CMSampleBufferGetDataBuffer(sample);
      const auto bytes = buffer ? CMBlockBufferGetDataLength(buffer) : 0;
      if (!bytes || bytes > self.config_.max_packet_bytes)
        throw VideoToolboxFailure{Error(CodecErrorCode::ResourceExhausted, "VideoToolbox H.264 access unit exceeds the packet bound")};
      Bytes data(bytes);
      VideoStatus(CMBlockBufferCopyDataBytes(buffer, 0, bytes, data.data()), "Cannot copy H.264 output");
      auto normalized = NormalizeVideoEncoderPacket(data, static_cast<unsigned>(length), self.config_.max_packet_bytes);
      if (!normalized.Succeeded()) throw VideoToolboxFailure{normalized.Error()};
      auto timestamp = [](CMTime value) -> std::optional<MediaTimestamp> {
        if (!CMTIME_IS_NUMERIC(value) || value.timescale <= 0) return std::nullopt;
        return MediaTimestamp{value.value, static_cast<std::uint32_t>(value.timescale)}.Rescale(1000000);
      };
      const auto pts = timestamp(CMSampleBufferGetPresentationTimeStamp(sample));
      const auto raw_dts = CMSampleBufferGetDecodeTimeStamp(sample);
      const auto dts = timestamp(raw_dts);
      if (!pts || (CMTIME_IS_NUMERIC(raw_dts) && !dts))
        throw VideoToolboxFailure{Error(CodecErrorCode::InvalidData, "VideoToolbox returned invalid H.264 timing")};
      if (!self.format_) {
        self.format_ = std::move(described).Value(); self.output_.push_back(*self.format_);
      }
      self.output_.push_back(EncodedPacket{Own(std::move(normalized).Value()), *pts, dts, std::nullopt, false});
    } catch (const VideoToolboxFailure& failure) { self.failure_ = failure.error; }
    catch (const std::exception& error) { self.failure_ = Error(CodecErrorCode::ResourceExhausted, error.what()); }
  }
  VideoEncoderConfig config_;
  VTCompressionSessionRef session_ = nullptr;
  std::mutex output_mutex_;
  std::deque<VideoEncoderBackendOutput> output_;
  std::optional<VideoEncodeFormat> format_;
  std::optional<CodecError> failure_;
  Bytes scratch_;
  std::size_t pending_frames_ = 0;
  bool key_requested_ = false, finished_ = false, eos_ = false, closing_ = false;
};
}
std::unique_ptr<VideoEncoderBackend> MakeVideoEncoderBackend() { return std::make_unique<MacOSVideoEncoder>(); }
} // namespace huxerui::media::detail
