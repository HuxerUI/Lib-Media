#include "codec/encoder/backend.h"

#include <cstring>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>

namespace huxerui::media::detail {
namespace {
CodecError MediaCodecError(media_status_t status, const char* message) {
  return Error(CodecErrorCode::PlatformFailure, message, std::to_string(status));
}
class AndroidAudioEncoder final : public AudioEncoderBackend {
public:
  CodecResult<void> Open(const AudioEncoderConfig& config) override {
    config_ = config;
    config_.wakeup = {}; config_.cleanup_completed = {};
    codec_ = AMediaCodec_createEncoderByType("audio/mp4a-latm");
    if (!codec_) return CodecResult<void>::Failure(Error(CodecErrorCode::Unsupported, "No Android AAC encoder"));
    auto* format = AMediaFormat_new();
    if (!format) return CodecResult<void>::Failure(Error(CodecErrorCode::ResourceExhausted, "Cannot allocate AAC MediaFormat"));
    AMediaFormat_setString(format, "mime", "audio/mp4a-latm");
    AMediaFormat_setInt32(format, "sample-rate", config.input.sample_rate);
    AMediaFormat_setInt32(format, "channel-count", config.input.ChannelCount());
    AMediaFormat_setInt32(format, "pcm-encoding", 2);
    AMediaFormat_setInt32(format, "aac-profile", 2);
    AMediaFormat_setInt32(format, "bitrate", config.bitrate);
    AMediaFormat_setInt32(format, "max-input-size", 4096);
    auto status = AMediaCodec_configure(codec_, format, nullptr, nullptr, AMEDIACODEC_CONFIGURE_FLAG_ENCODE);
    const auto deleted = AMediaFormat_delete(format);
    if (status != AMEDIA_OK) return CodecResult<void>::Failure(Error(CodecErrorCode::Unsupported, "Android rejected AAC encoder configuration", std::to_string(status)));
    if (deleted != AMEDIA_OK) return CodecResult<void>::Failure(MediaCodecError(deleted, "Cannot release encoder MediaFormat"));
    status = AMediaCodec_start(codec_);
    if (status != AMEDIA_OK) return CodecResult<void>::Failure(MediaCodecError(status, "Cannot start AAC encoder"));
    started_ = true;
    return CodecResult<void>::Success();
  }
  CodecResult<SubmitStatus> Submit(std::span<const std::byte> pcm, std::uint32_t samples, std::int64_t first_sample) override {
    auto pts = MediaTimestamp{first_sample, config_.input.sample_rate}.Rescale(1000000);
    if (!pts) return CodecResult<SubmitStatus>::Failure(Error(CodecErrorCode::InvalidData, "PCM timestamp exceeds MediaCodec range"));
    auto result = Queue(pcm, pts->ticks, 0);
    if (result.Succeeded() && result.Value() == SubmitStatus::Accepted) input_end_ = first_sample + samples;
    return result;
  }
  CodecResult<std::optional<AudioEncoderBackendOutput>> Poll() override {
    using R = CodecResult<std::optional<AudioEncoderBackendOutput>>;
    if (pending_) { auto packet = std::move(pending_); pending_.reset(); return R::Success(std::move(*packet)); }
    if (eos_) return R::Success(std::nullopt);
    if (pending_eos_) { eos_ = true; return R::Success(AudioEncoderEnd{priming_, padding_}); }
    AMediaCodecBufferInfo info{};
    const auto index = AMediaCodec_dequeueOutputBuffer(codec_, &info, 0);
    if (index == AMEDIACODEC_INFO_TRY_AGAIN_LATER || index == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) return R::Success(std::nullopt);
    if (index == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
      auto result = ReadFormat();
      if (!result.Succeeded()) return R::Failure(result.Error());
      return TakeFormat();
    }
    if (index < 0) return R::Failure(MediaCodecError(static_cast<media_status_t>(index), "Cannot dequeue AAC output"));
    auto result = R::Success(std::nullopt);
    if (info.size < 0 || std::uint32_t(info.size) > kAudioEncoderPacketBytes)
      result = R::Failure(Error(CodecErrorCode::ResourceExhausted, "Android AAC packet exceeds 64 KiB"));
    else if (info.size) {
      // NDK output pointers start at the valid data; use the declared size, as in the decoder adapter.
      const auto* bytes = AMediaCodec_getOutputBuffer(codec_, index, nullptr);
      if (!bytes) result = R::Failure(Error(CodecErrorCode::InvalidData, "Android AAC output is missing"));
      else if (info.flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG) {
        auto parsed = ReadAudioEncodeFormat({reinterpret_cast<const std::byte*>(bytes), std::size_t(info.size)}, config_);
        if (!parsed.Succeeded()) result = R::Failure(parsed.Error());
        else if (format_ && *format_ != parsed.Value()) result = R::Failure(Error(CodecErrorCode::InvalidData, "Android AAC initialization data changed"));
        else { format_ = std::move(parsed).Value(); result = TakeFormat(); }
      } else if (!format_) result = R::Failure(Error(CodecErrorCode::InvalidData, "Android AAC packet precedes initialization data"));
      else {
        const auto* first = reinterpret_cast<const std::byte*>(bytes);
        EncodedPacket packet{Own(Bytes(first, first + info.size)), {info.presentationTimeUs, 1000000}, std::nullopt, std::nullopt, true};
        if (!described_) { pending_ = std::move(packet); result = TakeFormat(); }
        else result = R::Success(std::move(packet));
      }
    }
    const auto released = AMediaCodec_releaseOutputBuffer(codec_, index, false);
    if (released != AMEDIA_OK) return R::Failure(MediaCodecError(released, "Cannot release AAC output buffer"));
    if (!result.Succeeded()) return result;
    if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) {
      auto updated = ReadFormat();
      if (!updated.Succeeded()) return R::Failure(updated.Error());
      pending_eos_ = true;
    }
    return result;
  }
  CodecResult<SubmitStatus> Finish() override {
    const auto pts = MediaTimestamp{input_end_, config_.input.sample_rate}.Rescale(1000000);
    if (!pts) return CodecResult<SubmitStatus>::Failure(Error(CodecErrorCode::InvalidData, "AAC EOS timestamp exceeds MediaCodec range"));
    return Queue({}, pts->ticks, AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
  }
  CodecResult<void> Close() override {
    std::optional<CodecError> failure;
    if (codec_) {
      if (started_) {
        const auto status = AMediaCodec_stop(codec_);
        if (status != AMEDIA_OK) failure = MediaCodecError(status, "Cannot stop AAC encoder");
      }
      const auto status = AMediaCodec_delete(codec_); codec_ = nullptr;
      if (status != AMEDIA_OK) failure = MediaCodecError(status, "Cannot delete AAC encoder");
    }
    pending_.reset();
    return failure ? CodecResult<void>::Failure(*failure) : CodecResult<void>::Success();
  }
private:
  CodecResult<SubmitStatus> Queue(std::span<const std::byte> pcm, std::int64_t pts, std::uint32_t flags) {
    using R = CodecResult<SubmitStatus>;
    const auto index = AMediaCodec_dequeueInputBuffer(codec_, 0);
    if (index == AMEDIACODEC_INFO_TRY_AGAIN_LATER) return R::Success(SubmitStatus::WouldBlock);
    if (index < 0) return R::Failure(MediaCodecError(static_cast<media_status_t>(index), "Cannot dequeue AAC input"));
    if (!pcm.empty()) {
      std::size_t capacity = 0;
      auto* bytes = AMediaCodec_getInputBuffer(codec_, index, &capacity);
      if (!bytes || capacity < pcm.size()) return R::Failure(Error(CodecErrorCode::ResourceExhausted, "MediaCodec PCM input buffer is too small"));
      std::memcpy(bytes, pcm.data(), pcm.size());
    }
    const auto status = AMediaCodec_queueInputBuffer(codec_, index, 0, pcm.size(), static_cast<std::uint64_t>(pts), flags);
    if (status != AMEDIA_OK) return R::Failure(MediaCodecError(status, "Cannot queue AAC input"));
    return R::Success(SubmitStatus::Accepted);
  }
  CodecResult<void> ReadFormat() {
    auto* output_format = AMediaCodec_getOutputFormat(codec_);
    if (!output_format) return CodecResult<void>::Failure(Error(CodecErrorCode::InvalidData, "Android AAC format is missing"));
    std::unique_ptr<AMediaFormat, decltype(&AMediaFormat_delete)> owner(output_format, AMediaFormat_delete);
    auto check = [&](const char* key, std::int32_t expected) {
      std::int32_t value;
      return !AMediaFormat_getInt32(output_format, key, &value) || value == expected;
    };
    if (!check("sample-rate", config_.input.sample_rate) || !check("channel-count", config_.input.ChannelCount()) ||
        !check("aac-profile", 2) || !check("bitrate", config_.bitrate))
      return CodecResult<void>::Failure(Error(CodecErrorCode::Unsupported, "Android changed the AAC encoder configuration"));
    void* data = nullptr; std::size_t size = 0;
    if (AMediaFormat_getBuffer(output_format, "csd-0", &data, &size)) {
      auto parsed = ReadAudioEncodeFormat({static_cast<const std::byte*>(data), size}, config_);
      if (!parsed.Succeeded()) return CodecResult<void>::Failure(parsed.Error());
      if (format_ && *format_ != parsed.Value()) return CodecResult<void>::Failure(Error(CodecErrorCode::InvalidData, "Android AAC format changed"));
      format_ = std::move(parsed).Value();
    }
    std::int32_t count;
    if (AMediaFormat_getInt32(output_format, "encoder-delay", &count) && count >= 0) priming_ = count;
    if (AMediaFormat_getInt32(output_format, "encoder-padding", &count) && count >= 0) padding_ = count;
    return CodecResult<void>::Success();
  }
  CodecResult<std::optional<AudioEncoderBackendOutput>> TakeFormat() {
    if (!format_ || described_) return CodecResult<std::optional<AudioEncoderBackendOutput>>::Success(std::nullopt);
    described_ = true;
    return CodecResult<std::optional<AudioEncoderBackendOutput>>::Success(*format_);
  }
  AudioEncoderConfig config_;
  AMediaCodec* codec_ = nullptr;
  std::optional<AudioEncodeFormat> format_;
  std::optional<EncodedPacket> pending_;
  std::optional<std::uint64_t> priming_, padding_;
  std::int64_t input_end_ = 0;
  bool started_ = false, described_ = false, eos_ = false, pending_eos_ = false;
};
} // namespace
std::unique_ptr<AudioEncoderBackend> MakeAudioEncoderBackend() { return std::make_unique<AndroidAudioEncoder>(); }
} // namespace huxerui::media::detail
