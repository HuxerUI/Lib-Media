#include "codec/decoder/backend.h"
#include "media_video_ios.h"
#include "codec/decoder/copy.h"
#include "codec/hevc.h"
#include "container/container_io.h"

#include <cmath>
#include <deque>
#include <mutex>

#import <AudioToolbox/AudioToolbox.h>
#import <VideoToolbox/VideoToolbox.h>
#import <Foundation/Foundation.h>

namespace huxerui::media::detail {
namespace {
CodecError AppleError(OSStatus status, const char* operation) {
  return Error(CodecErrorCode::PlatformFailure, operation, std::to_string(status));
}
MediaTimestamp Timestamp(CMTime time) { return {time.value, static_cast<std::uint32_t>(time.timescale)}; }
CMTime Timestamp(MediaTimestamp time) {
  const auto scaled = time.Rescale(1000000).value();
  return CMTimeMake(scaled.ticks, 1000000);
}
struct PixelMapping {
  CVPixelBufferRef image;
  std::optional<CodecError>* failure;
  bool locked = false;
  PixelMapping(CVPixelBufferRef value, std::optional<CodecError>* error) : image(value), failure(error) { CVPixelBufferRetain(image); }
  ~PixelMapping() {
    if (locked) {
      const auto status = CVPixelBufferUnlockBaseAddress(image, kCVPixelBufferLock_ReadOnly);
      if (status) *failure = AppleError(status, "Cannot unmap decoded pixels");
    }
    CVPixelBufferRelease(image);
  }
};
class IOSDecoder final : public DecoderBackend {
public:
  CodecResult<void> Open(const DecoderConfig& config) override {
    config_ = config;
    if (auto* audio = std::get_if<AudioDecoderConfig>(&config_)) {
      const auto channels = audio->output.ChannelCount();
      AudioStreamBasicDescription input{};
      input.mSampleRate = audio->output.sample_rate;
      input.mChannelsPerFrame = channels;
      audio_sample_limit_ = AudioSampleLimit(*audio).Value();
      Bytes cookie;
      switch (audio->codec) {
        case CodecId::Aac: {
          input.mFormatID = kAudioFormatMPEG4AAC;
          input.mFormatFlags = kMPEG4Object_AAC_LC;
          input.mFramesPerPacket = 1024;
          const auto& asc = audio->initialization_data;
          const std::uint8_t esds[]{3, 25, 0, 0, 0, 4, 17, 0x40, 0x15, 0, 0, 0,
              0, 0, 0, 0, 0, 0, 0, 0, 5, 2, std::to_integer<std::uint8_t>(asc[0]),
              std::to_integer<std::uint8_t>(asc[1]), 6, 1, 2};
          const auto bytes = std::as_bytes(std::span(esds));
          cookie.assign(bytes.begin(), bytes.end());
          break;
        }
        case CodecId::Opus:
          input.mFormatID = kAudioFormatOpus;
          audio_gain_ = std::pow(10.0f, Number<std::int16_t>(audio->initialization_data, 16, true) / 5120.0f);
          break;
        case CodecId::Flac: {
          input.mFormatID = kAudioFormatFLAC;
          const auto info = ParseFlacStreamInfo(audio->initialization_data, ContainerErrorCode::InvalidArgument);
          input.mFormatFlags = info.bits <= 16 ? kAppleLosslessFormatFlag_16BitSourceData :
              info.bits <= 20 ? kAppleLosslessFormatFlag_20BitSourceData :
              info.bits <= 24 ? kAppleLosslessFormatFlag_24BitSourceData : kAppleLosslessFormatFlag_32BitSourceData;
          input.mFramesPerPacket = info.min_block == info.max_block ? info.max_block : 0;
          Put(cookie, 50u);
          Put(cookie, Tag("dfLa"));
          Put(cookie, 0u);
          Put(cookie, 0x80000022u);
          Append(cookie, audio->initialization_data);
          break;
        }
        case CodecId::Mp3:
          input.mFormatID = kAudioFormatMPEGLayer3;
          input.mFramesPerPacket = audio_sample_limit_;
          mp3_.emplace(audio->output, audio_sample_limit_);
          break;
        default:
          return CodecResult<void>::Failure(Error(CodecErrorCode::Unsupported, "Unsupported AudioConverter codec"));
      }
      AudioStreamBasicDescription output{};
      output.mSampleRate = input.mSampleRate;
      output.mFormatID = kAudioFormatLinearPCM;
      output.mFormatFlags = UInt32(kAudioFormatFlagIsPacked) | UInt32(kAudioFormatFlagsNativeEndian)
          | (audio->output.sample_format == AudioSampleFormat::F32 ? kAudioFormatFlagIsFloat : kAudioFormatFlagIsSignedInteger);
      if (audio->output.packing == AudioPacking::Planar) output.mFormatFlags |= kAudioFormatFlagIsNonInterleaved;
      output.mChannelsPerFrame = channels;
      output.mFramesPerPacket = 1;
      output.mBitsPerChannel = audio->output.BytesPerSample() * 8;
      output.mBytesPerFrame = audio->output.BytesPerSample() * (audio->output.packing == AudioPacking::Planar ? 1 : channels);
      output.mBytesPerPacket = output.mBytesPerFrame;
      auto status = AudioConverterNew(&input, &output, &audio_);
      if (status) return CodecResult<void>::Failure(AppleError(status, "AudioConverterNew failed"));
      if (!cookie.empty()) {
        status = AudioConverterSetProperty(audio_, kAudioConverterDecompressionMagicCookie,
                                           static_cast<UInt32>(cookie.size()), cookie.data());
        if (status) return CodecResult<void>::Failure(AppleError(status, "Audio decoder cookie was rejected"));
      }
      if (audio->codec == CodecId::Opus) {
        // OpusHead trim belongs to the container timeline. Decode the complete packet here.
        const AudioConverterPrimeInfo prime{};
        status = AudioConverterSetProperty(audio_, kAudioConverterPrimeInfo, sizeof(prime), &prime);
        if (status) return CodecResult<void>::Failure(AppleError(status, "Cannot disable Opus decoder trim"));
      }
      return CodecResult<void>::Success();
    }
    const auto& video = std::get<VideoDecoderConfig>(config_);
    OSStatus status;
    NSDictionary* specification = nil;
    std::vector<const std::uint8_t*> parameters;
    std::vector<std::size_t> sizes;
    const auto add_parameters = [&](const auto& entries) {
      for (const auto& entry : entries) {
        parameters.push_back(reinterpret_cast<const std::uint8_t*>(entry.data()));
        sizes.push_back(entry.size());
      }
    };
    if (video.codec == CodecId::Vp9) {
#if __IPHONE_OS_VERSION_MAX_ALLOWED >= 260200
      if (@available(iOS 26.2, *)) VTRegisterSupplementalVideoDecoderIfAvailable(kCMVideoCodecType_VP9);
#endif
      if (!VTIsHardwareDecodeSupported(kCMVideoCodecType_VP9))
        return CodecResult<void>::Failure(Error(CodecErrorCode::Unsupported, "VP9 hardware decoding is unavailable"));
      // VideoToolbox needs a vpcC record for VP9 profile 0 packets from Matroska and WebM.
      constexpr std::uint8_t vpcc[]{1, 0, 0, 0, 0, 0, 0x82, 2, 2, 2, 0, 0};
      NSDictionary* extensions = @{(__bridge NSString*)kCMFormatDescriptionExtension_SampleDescriptionExtensionAtoms:
          @{@"vpcC": [NSData dataWithBytes:vpcc length:sizeof(vpcc)]}};
      status = CMVideoFormatDescriptionCreate(kCFAllocatorDefault, kCMVideoCodecType_VP9, video.width, video.height,
          (__bridge CFDictionaryRef)extensions, &description_);
      if (@available(iOS 17.0, *))
        specification = @{(__bridge NSString*)kVTVideoDecoderSpecification_RequireHardwareAcceleratedVideoDecoder: @YES};
    } else if (video.codec == CodecId::Hevc) {
      const auto hevc = ParseHevcConfiguration(video.initialization_data, true, true);
      add_parameters(hevc.parameters);
      status = CMVideoFormatDescriptionCreateFromHEVCParameterSets(kCFAllocatorDefault, parameters.size(),
          parameters.data(), sizes.data(), hevc.length, nullptr, &description_);
    } else {
      const auto avc = ParseAvc(video.initialization_data).Value();
      add_parameters(avc.parameters);
      status = CMVideoFormatDescriptionCreateFromH264ParameterSets(kCFAllocatorDefault, parameters.size(),
          parameters.data(), sizes.data(), avc.length_size, &description_);
    }
    if (status) return CodecResult<void>::Failure(AppleError(status, "Video format description was rejected"));
    const auto dimensions = CMVideoFormatDescriptionGetDimensions(description_);
    if (dimensions.width <= 0 || dimensions.height <= 0 || dimensions.width > video.width || dimensions.height > video.height)
      return CodecResult<void>::Failure(Error(CodecErrorCode::InvalidArgument, "Video dimensions exceed configured bounds"));
    const auto pixel_type = video.output == VideoPixelFormat::P010
        ? kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange : kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
    NSDictionary* attributes = video.output_storage == VideoStorage::Platform ? @{
      (__bridge NSString*)kCVPixelBufferPixelFormatTypeKey: @(pixel_type),
      (__bridge NSString*)kCVPixelBufferMetalCompatibilityKey: @YES,
      (__bridge NSString*)kCVPixelBufferIOSurfacePropertiesKey: @{}
    } : @{(id)kCVPixelBufferPixelFormatTypeKey: @(pixel_type)};
    VTDecompressionOutputCallbackRecord callback{Output, this};
    status = VTDecompressionSessionCreate(kCFAllocatorDefault, description_, (__bridge CFDictionaryRef)specification,
        (__bridge CFDictionaryRef)attributes, &callback, &video_);
    if (status) return CodecResult<void>::Failure(AppleError(status, "VTDecompressionSessionCreate failed"));
    return CodecResult<void>::Success();
  }
  CodecResult<SubmitStatus> Submit(const EncodedPacket& packet) override {
    if (audio_) {
      if (!audio_packets_.empty()) return CodecResult<SubmitStatus>::Success(SubmitStatus::WouldBlock);
      const auto samples = AudioPacketSamples(std::get<AudioDecoderConfig>(config_), packet.data.AsBytes());
      if (!samples.Succeeded()) return CodecResult<SubmitStatus>::Failure(samples.Error());
      audio_packets_.push_back({packet, samples.Value()});
      if (mp3_) mp3_->Expect(packet.pts);
      else audio_times_.emplace_back(packet.pts, samples.Value());
      return CodecResult<SubmitStatus>::Success(SubmitStatus::Accepted);
    }
    CMBlockBufferRef block = nullptr;
    auto bytes = packet.data.AsBytes();
    auto status = CMBlockBufferCreateWithMemoryBlock(kCFAllocatorDefault, nullptr, bytes.size(), kCFAllocatorDefault,
        nullptr, 0, bytes.size(), 0, &block);
    if (status) return CodecResult<SubmitStatus>::Failure(AppleError(status, "Cannot allocate compressed video input"));
    status = CMBlockBufferReplaceDataBytes(bytes.data(), block, 0, bytes.size());
    CMSampleBufferRef sample = nullptr;
    if (!status) {
      const CMSampleTimingInfo time{packet.duration ? Timestamp(*packet.duration) : kCMTimeInvalid,
          Timestamp(packet.pts), packet.dts ? Timestamp(*packet.dts) : kCMTimeInvalid};
      const std::size_t size = bytes.size();
      status = CMSampleBufferCreateReady(kCFAllocatorDefault, block, description_, 1, 1, &time, 1, &size, &sample);
    }
    CFRelease(block);
    if (status) return CodecResult<SubmitStatus>::Failure(AppleError(status, "Cannot create compressed video sample"));
    VTDecodeInfoFlags flags = 0;
    const auto& video = std::get<VideoDecoderConfig>(config_);
    const bool hidden = video.codec == CodecId::Vp9 && !Vp9PacketDisplays(bytes).Value();
    status = VTDecompressionSessionDecodeFrame(video_, sample, hidden ? kVTDecodeFrame_DoNotOutputFrame : 0,
        hidden ? this : nullptr, &flags);
    CFRelease(sample);
    if (status) return CodecResult<SubmitStatus>::Failure(AppleError(status, "VideoToolbox decoding failed"));
    if (!hidden && (flags & kVTDecodeInfo_FrameDropped))
      return CodecResult<SubmitStatus>::Failure(Error(CodecErrorCode::InvalidData, "VideoToolbox dropped a frame"));
    return CodecResult<SubmitStatus>::Success(SubmitStatus::Accepted);
  }
  CodecResult<std::optional<BackendOutput>> Poll() override {
    using R = CodecResult<std::optional<BackendOutput>>;
    if (audio_) {
      if (audio_packets_.empty() && !active_packet_ && !finished_) return R::Success(std::nullopt);
      auto decoded = ReadAudio();
      if (!decoded.Succeeded()) return R::Failure(decoded.Error());
      return R::Success(std::move(decoded).Value());
    }
    std::lock_guard lock(output_mutex_);
    if (failure_) return R::Failure(*failure_);
    if (!output_.empty()) {
      auto frame = std::move(output_.front()); output_.pop_front();
      return R::Success(BackendOutput{std::move(frame)});
    }
    if (finished_ && !eos_) { eos_ = true; return R::Success(BackendOutput{EndOfStream{}}); }
    return R::Success(std::nullopt);
  }
  CodecResult<SubmitStatus> Finish() override {
    finished_ = true;
    if (video_) {
      auto status = VTDecompressionSessionFinishDelayedFrames(video_);
      if (!status) status = VTDecompressionSessionWaitForAsynchronousFrames(video_);
      if (status) return CodecResult<SubmitStatus>::Failure(AppleError(status, "AVC drain failed"));
    }
    return CodecResult<SubmitStatus>::Success(SubmitStatus::Accepted);
  }
  CodecResult<void> Close() override {
    std::optional<CodecError> error;
    if (video_) {
      const auto status = VTDecompressionSessionWaitForAsynchronousFrames(video_);
      if (status) error = AppleError(status, "VideoToolbox completion wait failed");
      VTDecompressionSessionInvalidate(video_);
      CFRelease(video_);
      video_ = nullptr;
    }
    if (description_) { CFRelease(description_); description_ = nullptr; }
    if (audio_) {
      const auto status = AudioConverterDispose(audio_);
      audio_ = nullptr;
      if (status) error = AppleError(status, "AudioConverterDispose failed");
    }
    output_.clear(); audio_packets_.clear(); audio_times_.clear(); active_packet_.reset();
    if (error) return CodecResult<void>::Failure(*error);
    return CodecResult<void>::Success();
  }
private:
  static constexpr OSStatus NeedInput = 'hxin';
  static OSStatus Input(AudioConverterRef, UInt32* count, AudioBufferList* data,
      AudioStreamPacketDescription** descriptions, void* context) {
    auto& self = *static_cast<IOSDecoder*>(context);
    // The next input callback ends the converter's borrow of the previous packet.
    self.active_packet_.reset();
    if (self.audio_packets_.empty()) {
      *count = 0;
      return self.finished_ ? noErr : NeedInput;
    }
    self.active_packet_ = std::move(self.audio_packets_.front());
    self.audio_packets_.pop_front();
    const auto bytes = self.active_packet_->packet.data.AsBytes();
    *count = 1;
    data->mNumberBuffers = 1;
    data->mBuffers[0] = {std::get<AudioDecoderConfig>(self.config_).output.ChannelCount(),
        static_cast<UInt32>(bytes.size()), const_cast<std::byte*>(bytes.data())};
    self.packet_description_ = {0, self.active_packet_->samples, static_cast<UInt32>(bytes.size())};
    if (descriptions) *descriptions = &self.packet_description_;
    return noErr;
  }
  CodecResult<std::optional<BackendOutput>> ReadAudio() {
    using R = CodecResult<std::optional<BackendOutput>>;
    if (eos_) return R::Success(std::nullopt);
    const auto& config = std::get<AudioDecoderConfig>(config_);
    const auto format = config.output;
    const auto count = format.packing == AudioPacking::Planar ? format.ChannelCount() : 1;
    const auto stride = format.BytesPerSample() * format.ChannelCount() / count;
    struct Buffers { UInt32 count; AudioBuffer planes[2]; } buffers{};
    buffers.count = count;
    const auto expected = audio_times_.empty() ? audio_sample_limit_ : audio_times_.front().second;
    UInt32 samples = expected;
    std::vector<Bytes> storage(count, Bytes(samples * stride));
    for (unsigned i = 0; i < count; ++i) buffers.planes[i] = {format.ChannelCount() / count,
        static_cast<UInt32>(storage[i].size()), storage[i].data()};
    const auto status = AudioConverterFillComplexBuffer(audio_, Input, this, &samples,
        reinterpret_cast<AudioBufferList*>(&buffers), nullptr);
    if (status && status != NeedInput) return R::Failure(AppleError(status, "AudioConverter decoding failed"));
    if (config.codec == CodecId::Opus && !audio_times_.empty() && samples != expected)
      return R::Failure(Error(CodecErrorCode::InvalidData, "AudioConverter changed the Opus packet sample count"));
    if (samples) {
      if (!mp3_ && audio_times_.empty())
        return R::Failure(Error(CodecErrorCode::InvalidData, "PCM output has no input timing"));
      AudioFrame frame{format, mp3_ ? MediaTimestamp{0, format.sample_rate} : audio_times_.front().first, samples, {}};
      for (auto& bytes : storage) { bytes.resize(samples * stride); frame.planes.push_back(Own(std::move(bytes))); }
      if (mp3_) {
        const auto buffered = mp3_->Append(frame);
        if (!buffered.Succeeded()) return R::Failure(buffered.Error());
        if (auto packet = mp3_->Read()) return R::Success(BackendOutput{DecodedFrame{std::move(*packet)}});
        return R::Success(std::nullopt);
      }
      if (audio_gain_ != 1.0f) {
        auto adjusted = CopyAudio(frame, format, audio_gain_);
        if (!adjusted.Succeeded()) return R::Failure(adjusted.Error());
        frame = std::move(adjusted).Value();
      }
      audio_times_.pop_front();
      return R::Success(BackendOutput{DecodedFrame{std::move(frame)}});
    }
    if (finished_ && status == noErr) {
      if (mp3_ && !mp3_->Drained())
        return R::Failure(Error(CodecErrorCode::InvalidData, "AudioConverter MP3 drain has missing PCM samples"));
      eos_ = true;
      return R::Success(BackendOutput{EndOfStream{}});
    }
    return R::Success(std::nullopt);
  }
  static void Output(void* context, void* hidden, OSStatus status, VTDecodeInfoFlags flags, CVImageBufferRef image,
      CMTime pts, CMTime duration) {
    auto& self = *static_cast<IOSDecoder*>(context);
    std::lock_guard lock(self.output_mutex_);
    if (self.failure_) return;
    if (hidden && !status) return;
    if (status || !image || (flags & kVTDecodeInfo_FrameDropped)) {
      self.failure_ = AppleError(status, "VideoToolbox returned a failed/dropped frame"); return;
    }
    try {
      const auto& config = std::get<VideoDecoderConfig>(self.config_);
      const auto type = CVPixelBufferGetPixelFormatType(image);
      const bool ten_bit = type == kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange ||
                           type == kCVPixelFormatType_420YpCbCr10BiPlanarFullRange;
      const bool eight_bit = type == kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange ||
                             type == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange;
      if ((!ten_bit && !eight_bit) || ten_bit != (config.output == VideoPixelFormat::P010)) {
        self.failure_ = Error(CodecErrorCode::Unsupported, "VideoToolbox returned an unexpected CPU pixel format"); return;
      }
      VideoFormat format;
      format.pixel_format = ten_bit ? VideoPixelFormat::P010 : VideoPixelFormat::NV12;
      format.width = static_cast<std::uint32_t>(CVPixelBufferGetWidth(image));
      format.height = static_cast<std::uint32_t>(CVPixelBufferGetHeight(image));
      if (format.width > config.width || format.height > config.height) {
        self.failure_ = Error(CodecErrorCode::InvalidData, "Video output exceeds configured dimensions"); return;
      }
      const auto rect = CVImageBufferGetCleanRect(image);
      format.visible = {static_cast<std::uint32_t>(rect.origin.x), static_cast<std::uint32_t>(rect.origin.y),
          static_cast<std::uint32_t>(rect.size.width), static_cast<std::uint32_t>(rect.size.height)};
      format.range = type == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange ||
          type == kCVPixelFormatType_420YpCbCr10BiPlanarFullRange ? ColorRange::Full : ColorRange::Limited;
      if (auto aspect = CVBufferCopyAttachment(image, kCVImageBufferPixelAspectRatioKey, nullptr)) {
        std::int64_t horizontal = 0, vertical = 0;
        bool valid = false;
        if (CFGetTypeID(aspect) == CFDictionaryGetTypeID()) {
          auto dictionary = static_cast<CFDictionaryRef>(aspect);
          auto x = static_cast<CFNumberRef>(CFDictionaryGetValue(dictionary, kCVImageBufferPixelAspectRatioHorizontalSpacingKey));
          auto y = static_cast<CFNumberRef>(CFDictionaryGetValue(dictionary, kCVImageBufferPixelAspectRatioVerticalSpacingKey));
          valid = x && y && CFGetTypeID(x) == CFNumberGetTypeID() && CFGetTypeID(y) == CFNumberGetTypeID()
              && CFNumberGetValue(x, kCFNumberSInt64Type, &horizontal) && CFNumberGetValue(y, kCFNumberSInt64Type, &vertical)
              && horizontal > 0 && vertical > 0 && horizontal <= UINT32_MAX && vertical <= UINT32_MAX;
        }
        CFRelease(aspect);
        if (!valid) { self.failure_ = Error(CodecErrorCode::InvalidData, "Invalid VideoToolbox pixel aspect ratio"); return; }
        format.aspect_width = static_cast<std::uint32_t>(horizontal);
        format.aspect_height = static_cast<std::uint32_t>(vertical);
      }

      auto attachment = [&](CFStringRef key, CFStringRef value) {
        auto found = CVBufferCopyAttachment(image, key, nullptr);
        const bool same = found && CFEqual(found, value);
        if (found) CFRelease(found);
        return same;
      };
      if (attachment(kCVImageBufferColorPrimariesKey, kCVImageBufferColorPrimaries_ITU_R_709_2)) format.primaries = ColorPrimaries::BT709;
      else if (attachment(kCVImageBufferColorPrimariesKey, kCVImageBufferColorPrimaries_ITU_R_2020)) format.primaries = ColorPrimaries::BT2020;
      if (attachment(kCVImageBufferTransferFunctionKey, kCVImageBufferTransferFunction_ITU_R_709_2)) format.transfer = ColorTransfer::BT709;
      else if (attachment(kCVImageBufferTransferFunctionKey, kCVImageBufferTransferFunction_SMPTE_ST_2084_PQ)) format.transfer = ColorTransfer::PQ;
      else if (attachment(kCVImageBufferTransferFunctionKey, kCVImageBufferTransferFunction_ITU_R_2100_HLG)) format.transfer = ColorTransfer::HLG;
      if (attachment(kCVImageBufferYCbCrMatrixKey, kCVImageBufferYCbCrMatrix_ITU_R_709_2)) format.matrix = ColorMatrix::BT709;
      else if (attachment(kCVImageBufferYCbCrMatrixKey, kCVImageBufferYCbCrMatrix_ITU_R_601_4)) format.matrix = ColorMatrix::BT601;
      else if (attachment(kCVImageBufferYCbCrMatrixKey, kCVImageBufferYCbCrMatrix_ITU_R_2020)) format.matrix = ColorMatrix::BT2020;
      const auto duration_on_timeline = CMTIME_IS_NUMERIC(duration) ? std::optional(Timestamp(duration)) : std::nullopt;
      if (config.output_storage == VideoStorage::Platform) {
        auto retained = ios::RetainPixelBuffer(image, format, Timestamp(pts), duration_on_timeline);
        if (!retained.Succeeded()) self.failure_ = retained.Error();
        else self.output_.push_back(DecodedFrame{std::move(retained).Value()});
        return;
      }
      auto owner = std::make_shared<PixelMapping>(image, &self.failure_);
      const auto locked = CVPixelBufferLockBaseAddress(image, kCVPixelBufferLock_ReadOnly);
      if (locked) { self.failure_ = AppleError(locked, "Cannot map decoded pixels"); return; }
      owner->locked = true;
      std::vector<VideoPlane> planes;
      for (std::size_t i = 0; i < 2; ++i) {
        const auto stride = CVPixelBufferGetBytesPerRowOfPlane(image, i);
        const auto size = stride * CVPixelBufferGetHeightOfPlane(image, i);
        auto* base = static_cast<const std::byte*>(CVPixelBufferGetBaseAddressOfPlane(image, i));
        planes.push_back({BufferReference({base, size}, owner), stride, (i ? 2u : 1u) * (ten_bit ? 2u : 1u)});
      }
      auto copied = CopyVideo(format, Timestamp(pts), duration_on_timeline, std::move(planes), config.output);
      owner.reset();
      if (!copied.Succeeded()) self.failure_ = copied.Error();
      else if (!self.failure_) self.output_.push_back(DecodedFrame{std::move(copied).Value()});
    } catch (const std::exception& error) { self.failure_ = Error(CodecErrorCode::ResourceExhausted, error.what()); }
  }
  DecoderConfig config_;
  AudioConverterRef audio_ = nullptr;
  VTDecompressionSessionRef video_ = nullptr;
  CMVideoFormatDescriptionRef description_ = nullptr;
  struct AudioInput {
    EncodedPacket packet;
    unsigned samples;
  };
  std::deque<AudioInput> audio_packets_;
  std::optional<AudioInput> active_packet_;
  std::deque<std::pair<MediaTimestamp, unsigned>> audio_times_;
  unsigned audio_sample_limit_ = 0;
  std::optional<PcmPackets> mp3_;
  float audio_gain_ = 1.0f;
  AudioStreamPacketDescription packet_description_{};
  std::mutex output_mutex_;
  std::deque<DecodedFrame> output_;
  std::optional<CodecError> failure_;
  bool finished_ = false, eos_ = false;
};
} // namespace
std::unique_ptr<DecoderBackend> MakeDecoderBackend() { return std::make_unique<IOSDecoder>(); }
} // namespace huxerui::media::detail
