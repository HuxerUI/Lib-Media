#include "codec/encoder/backend.h"

#include <algorithm>
#include <cstring>
#import <AudioToolbox/AudioToolbox.h>

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
} // namespace huxerui::media::detail
