#include "codec/encoder/backend.h"
#include "media_video_android.h"

#include <cstring>
#include <cstdlib>
#include <utility>
#include <dlfcn.h>
#include <sys/system_properties.h>
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

namespace {
class AndroidVideoEncoder final : public VideoEncoderBackend {
public:
  CodecResult<void> Open(const VideoEncoderConfig& config) override {
    config_ = config; config_.wakeup = {}; config_.cleanup_completed = {};
    char version[PROP_VALUE_MAX]{};
    __system_property_get("ro.build.version.sdk", version);
    const auto api = std::strtol(version, nullptr, 10);
    const bool hevc = config.codec == CodecId::Hevc;
    const bool surface_input = config.input_storage == VideoStorage::Platform;
    const bool ten_bit = config.input.pixel_format == VideoPixelFormat::P010;
    const bool baseline = !hevc && config.profile == H264Profile::Baseline;
    const auto* mime = hevc ? "video/hevc" : "video/avc";
    input_format_ = reinterpret_cast<AMediaFormat* (*)(AMediaCodec*)>(dlsym(RTLD_DEFAULT, "AMediaCodec_getInputFormat"));
    set_parameters_ = reinterpret_cast<media_status_t (*)(AMediaCodec*, const AMediaFormat*)>(dlsym(RTLD_DEFAULT, "AMediaCodec_setParameters"));
    if (api < 28 || !input_format_ || !set_parameters_)
      return CodecResult<void>::Failure(Error(CodecErrorCode::Unsupported, "Video encoding requires Android API 28 or later"));
    if (ten_bit && api < 33)
      return CodecResult<void>::Failure(Error(CodecErrorCode::Unsupported, "P010 encoding requires Android API 33 or later"));
    if (api < 29 && !baseline && config.max_b_frames)
      return CodecResult<void>::Failure(Error(CodecErrorCode::Unsupported, "A numeric B-picture bound requires Android API 29 or later"));
    codec_ = AMediaCodec_createEncoderByType(mime);
    if (!codec_) return CodecResult<void>::Failure(Error(CodecErrorCode::Unsupported, "No Android video encoder"));
    auto* format = AMediaFormat_new();
    if (!format) return CodecResult<void>::Failure(Error(CodecErrorCode::ResourceExhausted, "Cannot allocate video MediaFormat"));
    std::unique_ptr<AMediaFormat, decltype(&AMediaFormat_delete)> owner(format, AMediaFormat_delete);
    AMediaFormat_setString(format, "mime", mime);
    AMediaFormat_setInt32(format, "width", config.input.width); AMediaFormat_setInt32(format, "height", config.input.height);
    const auto row = config.input.width * (ten_bit ? 2 : 1);
    if (surface_input) {
      AMediaFormat_setInt32(format, "color-format", 0x7f000789);
    } else {
      AMediaFormat_setInt32(format, "color-format", ten_bit ? 54 : 21);
      AMediaFormat_setInt32(format, "stride", row); AMediaFormat_setInt32(format, "slice-height", config.input.height);
    }
    AMediaFormat_setInt32(format, "bitrate", config.bitrate);
    AMediaFormat_setFloat(format, "frame-rate", float(config.frame_rate_numerator) / config.frame_rate_denominator);
    AMediaFormat_setFloat(format, "i-frame-interval", float(config.key_frame_interval) * config.frame_rate_denominator / config.frame_rate_numerator);
    AMediaFormat_setInt32(format, "profile", hevc ? (ten_bit ? 2 : 1) :
        baseline ? 1 : config.profile == H264Profile::Main ? 2 : 8);
    frame_bytes_ = std::size_t(row) * config.input.height * 3 / 2;
    if (!surface_input) AMediaFormat_setInt32(format, "max-input-size", static_cast<std::int32_t>(frame_bytes_));
    if (!baseline) {
      if (api >= 29) AMediaFormat_setInt32(format, "max-bframes", VideoEncoderBFrames(config));
      else AMediaFormat_setInt32(format, "latency", config.max_b_frames ? 1 : static_cast<std::int32_t>(VideoEncoderWindow(config) - 1));
    }
    if (config.input.range != ColorRange::Unknown)
      AMediaFormat_setInt32(format, "color-range", config.input.range == ColorRange::Full ? 1 : 2);
    if (config.input.primaries != ColorPrimaries::Unknown || config.input.matrix != ColorMatrix::Unknown) {
      const auto standard = config.input.primaries == ColorPrimaries::BT601_525 ? 4 :
          config.input.primaries == ColorPrimaries::BT601_625 ? 2 :
          config.input.matrix == ColorMatrix::BT601 ? 4 : 1;
      AMediaFormat_setInt32(format, "color-standard", standard);
    }
    if (config.input.transfer != ColorTransfer::Unknown) {
      if (config.input.transfer == ColorTransfer::SRGB)
        return CodecResult<void>::Failure(Error(CodecErrorCode::Unsupported, "Android video cannot request exact sRGB transfer signaling"));
      AMediaFormat_setInt32(format, "color-transfer", config.input.transfer == ColorTransfer::Linear ? 1 : 3);
    }
    auto status = AMediaCodec_configure(codec_, format, nullptr, nullptr, AMEDIACODEC_CONFIGURE_FLAG_ENCODE);
    if (status != AMEDIA_OK && (baseline || hevc)) {
      // A vendor encoder can advertise byte-buffer input and still reject the configuration.
      const char* candidates[]{hevc ? "c2.android.hevc.encoder" : "c2.android.avc.encoder",
          hevc ? nullptr : "OMX.google.h264.encoder"};
      for (const char* name : candidates) {
        if (!name) break;
        if (codec_) {
          const auto retired = AMediaCodec_delete(std::exchange(codec_, nullptr));
          if (retired != AMEDIA_OK)
            return CodecResult<void>::Failure(MediaCodecError(retired, "Cannot retire rejected video encoder"));
        }
        codec_ = AMediaCodec_createCodecByName(name);
        if (!codec_) continue;
        status = AMediaCodec_configure(codec_, format, nullptr, nullptr, AMEDIACODEC_CONFIGURE_FLAG_ENCODE);
        if (status == AMEDIA_OK) break;
      }
    }
    if (status != AMEDIA_OK)
      return CodecResult<void>::Failure(Error(CodecErrorCode::Unsupported, "Android rejected video configuration", std::to_string(status)));
    auto* actual = input_format_(codec_);
    if (!actual) return CodecResult<void>::Failure(Error(CodecErrorCode::InvalidData, "Android video input format is missing"));
    std::unique_ptr<AMediaFormat, decltype(&AMediaFormat_delete)> actual_owner(actual, AMediaFormat_delete);
    std::int32_t layout = 0, stride = ten_bit ? 0 : row, rows = config.input.height;
    AMediaFormat_getInt32(actual, "stride", &stride); AMediaFormat_getInt32(actual, "slice-height", &rows);
    if (!surface_input && (!AMediaFormat_getInt32(actual, "color-format", &layout) ||
        (ten_bit ? layout != 54 : layout != 19 && layout != 21) ||
        stride != std::int32_t(row) || rows != std::int32_t(config.input.height)))
      return CodecResult<void>::Failure(Error(CodecErrorCode::Unsupported, "Android video input requires an identifiable packed CPU YUV layout"));
    layout_ = ten_bit ? VideoPixelFormat::P010 : layout == 19 ? VideoPixelFormat::I420 : VideoPixelFormat::NV12;
    std::int32_t latency = 0, max_b = 0;
    if (AMediaFormat_getInt32(actual, "latency", &latency) && latency >= std::int32_t(VideoEncoderWindow(config)))
      return CodecResult<void>::Failure(Error(CodecErrorCode::Unsupported, "Android encoder delay exceeds the output window"));
    if (config.max_b_frames && *config.max_b_frames && (!AMediaFormat_getInt32(actual, "max-bframes", &max_b) ||
        max_b < 0 || std::uint32_t(max_b) > *config.max_b_frames))
      return CodecResult<void>::Failure(Error(CodecErrorCode::Unsupported, "Android did not confirm the requested B-picture bound"));
    if (surface_input) {
      const auto create_surface = reinterpret_cast<media_status_t (*)(AMediaCodec*, ANativeWindow**)>(
          dlsym(RTLD_DEFAULT, "AMediaCodec_createInputSurface"));
      surface_eos_ = reinterpret_cast<decltype(surface_eos_)>(dlsym(RTLD_DEFAULT, "AMediaCodec_signalEndOfInputStream"));
      if (!create_surface || !surface_eos_)
        return CodecResult<void>::Failure(Error(CodecErrorCode::Unsupported, "MediaCodec encoder surfaces are unavailable"));
      ANativeWindow* window = nullptr;
      status = create_surface(codec_, &window);
      if (status != AMEDIA_OK || !window)
        return CodecResult<void>::Failure(Error(CodecErrorCode::Unsupported, "MediaCodec rejected its encoder input surface", std::to_string(status)));
      surface_.reset(window);
      renderer_ = std::make_unique<AndroidVideoRenderer>(surface_.get());
    }
    status = AMediaCodec_start(codec_);
    if (status != AMEDIA_OK) return CodecResult<void>::Failure(MediaCodecError(status, "Cannot start video encoder"));
    started_ = true;
    return CodecResult<void>::Success();
  }
  CodecResult<SubmitStatus> Submit(const VideoFrame& frame) override {
    using R = CodecResult<SubmitStatus>;
    const auto duration = frame.duration ? frame.duration->Rescale(1000000) :
        MediaTimestamp{config_.frame_rate_denominator, config_.frame_rate_numerator}.Rescale(1000000);
    if (!duration || duration->ticks <= 0 || frame.pts.ticks > INT64_MAX - duration->ticks)
      return R::Failure(Error(CodecErrorCode::InvalidData, "Android video input timing exceeds signed bounds"));
    if (renderer_) {
      renderer_->DrawToSurface(frame);
      return R::Success(SubmitStatus::Accepted);
    }
    const auto index = AMediaCodec_dequeueInputBuffer(codec_, 0);
    if (index == AMEDIACODEC_INFO_TRY_AGAIN_LATER) return R::Success(SubmitStatus::WouldBlock);
    if (index < 0) return R::Failure(MediaCodecError(static_cast<media_status_t>(index), "Cannot dequeue video input"));
    std::size_t capacity = 0;
    auto* bytes = AMediaCodec_getInputBuffer(codec_, index, &capacity);
    if (!bytes || capacity < frame_bytes_)
      return R::Failure(Error(CodecErrorCode::ResourceExhausted, "Android video input buffer is too small"));
    CopyVideoEncoderPixels(frame, layout_, {reinterpret_cast<std::byte*>(bytes), frame_bytes_});
    const auto status = AMediaCodec_queueInputBuffer(codec_, index, 0, frame_bytes_, std::uint64_t(frame.pts.ticks), 0);
    if (status != AMEDIA_OK) return R::Failure(MediaCodecError(status, "Cannot queue video input"));
    input_end_ = frame.pts.ticks + duration->ticks;
    return R::Success(SubmitStatus::Accepted);
  }
  CodecResult<std::optional<VideoEncoderBackendOutput>> Poll() override {
    using R = CodecResult<std::optional<VideoEncoderBackendOutput>>;
    if (pending_) { auto packet = std::move(*pending_); pending_.reset(); return R::Success(std::move(packet)); }
    if (eos_) return R::Success(std::nullopt);
    if (pending_eos_) { eos_ = true; return R::Success(VideoEncoderEnd{}); }
    AMediaCodecBufferInfo info{};
    const auto index = AMediaCodec_dequeueOutputBuffer(codec_, &info, 0);
    if (index == AMEDIACODEC_INFO_TRY_AGAIN_LATER || index == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED)
      return R::Success(std::nullopt);
    if (index == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
      auto described = ReadFormat();
      if (!described.Succeeded()) return R::Failure(described.Error());
      if (!format_) { format_ = std::move(described).Value(); return R::Success(*format_); }
      if (*format_ != described.Value()) return R::Failure(Error(CodecErrorCode::Unsupported, "Android video configuration changed"));
      return R::Success(std::nullopt);
    }
    if (index < 0) return R::Failure(MediaCodecError(static_cast<media_status_t>(index), "Cannot dequeue video output"));
    auto result = R::Success(std::nullopt);
    if (info.size < 0 || std::size_t(info.size) > config_.max_packet_bytes)
      result = R::Failure(Error(CodecErrorCode::ResourceExhausted, "Android video output exceeds the access-unit bound"));
    else if (info.size && !(info.flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG)) {
      const auto* data = AMediaCodec_getOutputBuffer(codec_, index, nullptr);
      if (!data) result = R::Failure(Error(CodecErrorCode::InvalidData, "Android video output data is missing"));
      else {
        if (!partial_.empty() && info.presentationTimeUs != partial_pts_)
          result = R::Failure(Error(CodecErrorCode::InvalidData, "Android video fragments have different presentation times"));
        else if (std::size_t(info.size) > config_.max_packet_bytes - partial_.size())
          result = R::Failure(Error(CodecErrorCode::ResourceExhausted, "Android video fragments exceed the access-unit bound"));
        else {
          partial_pts_ = info.presentationTimeUs;
          const auto* first = reinterpret_cast<const std::byte*>(data);
          partial_.insert(partial_.end(), first, first + info.size);
          if (!(info.flags & AMEDIACODEC_BUFFER_FLAG_PARTIAL_FRAME)) {
            auto normalized = NormalizeVideoEncoderPacket(partial_, 0, config_.max_packet_bytes, config_.codec); partial_.clear();
            if (!normalized.Succeeded()) result = R::Failure(normalized.Error());
            else {
              auto described = ReadFormat();
              if (!described.Succeeded()) result = R::Failure(described.Error());
              else if (format_ && *format_ != described.Value()) result = R::Failure(Error(CodecErrorCode::Unsupported, "Android video configuration changed"));
              else {
                EncodedPacket packet{Own(std::move(normalized).Value()), {info.presentationTimeUs, 1000000}, std::nullopt, std::nullopt, false};
                if (!format_) { format_ = std::move(described).Value(); pending_ = std::move(packet); result = R::Success(*format_); }
                else result = R::Success(std::move(packet));
              }
            }
          }
        }
      }
    }
    const auto released = AMediaCodec_releaseOutputBuffer(codec_, index, false);
    if (released != AMEDIA_OK) return R::Failure(MediaCodecError(released, "Cannot release video output"));
    if (!result.Succeeded()) return result;
    if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) {
      if (!partial_.empty()) return R::Failure(Error(CodecErrorCode::InvalidData, "Android video ended during a partial access unit"));
      pending_eos_ = true;
    }
    return result;
  }
  CodecResult<void> RequestKeyFrame() override {
    auto* format = AMediaFormat_new();
    if (!format) return CodecResult<void>::Failure(Error(CodecErrorCode::ResourceExhausted, "Cannot allocate key-picture parameters"));
    AMediaFormat_setInt32(format, "request-sync", 0);
    const auto status = set_parameters_(codec_, format), deleted = AMediaFormat_delete(format);
    if (status != AMEDIA_OK) return CodecResult<void>::Failure(MediaCodecError(status, "Android rejected the key-picture request"));
    if (deleted != AMEDIA_OK) return CodecResult<void>::Failure(MediaCodecError(deleted, "Cannot release key-picture parameters"));
    return CodecResult<void>::Success();
  }
  CodecResult<SubmitStatus> Finish() override {
    if (renderer_) {
      const auto status = surface_eos_(codec_);
      if (status != AMEDIA_OK)
        return CodecResult<SubmitStatus>::Failure(MediaCodecError(status, "Android rejected video Surface EOS"));
      return CodecResult<SubmitStatus>::Success(SubmitStatus::Accepted);
    }
    const auto index = AMediaCodec_dequeueInputBuffer(codec_, 0);
    if (index == AMEDIACODEC_INFO_TRY_AGAIN_LATER) return CodecResult<SubmitStatus>::Success(SubmitStatus::WouldBlock);
    if (index < 0) return CodecResult<SubmitStatus>::Failure(MediaCodecError(static_cast<media_status_t>(index), "Cannot dequeue video EOS input"));
    const auto status = AMediaCodec_queueInputBuffer(codec_, index, 0, 0, std::uint64_t(input_end_), AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
    if (status != AMEDIA_OK) return CodecResult<SubmitStatus>::Failure(MediaCodecError(status, "Android rejected video EOS"));
    return CodecResult<SubmitStatus>::Success(SubmitStatus::Accepted);
  }
  CodecResult<void> Close() override {
    std::optional<CodecError> failure;
    renderer_.reset();
    if (codec_) {
      if (started_) {
        const auto status = AMediaCodec_stop(codec_);
        if (status != AMEDIA_OK) failure = MediaCodecError(status, "Cannot stop video encoder");
      }
      const auto status = AMediaCodec_delete(codec_); codec_ = nullptr;
      if (status != AMEDIA_OK) failure = MediaCodecError(status, "Cannot delete video encoder");
    }
    surface_.reset();
    pending_.reset(); Bytes{}.swap(partial_);
    return failure ? CodecResult<void>::Failure(*failure) : CodecResult<void>::Success();
  }
private:
  CodecResult<VideoEncodeFormat> ReadFormat() {
    using R = CodecResult<VideoEncodeFormat>;
    auto* format = AMediaCodec_getOutputFormat(codec_);
    if (!format) return R::Failure(Error(CodecErrorCode::InvalidData, "Android video output format is missing"));
    std::unique_ptr<AMediaFormat, decltype(&AMediaFormat_delete)> owner(format, AMediaFormat_delete);
    std::int32_t reorder_depth = 0;
    if (config_.max_b_frames && AMediaFormat_getInt32(format, "output-reorder-depth", &reorder_depth) &&
        (reorder_depth < 0 || std::uint32_t(reorder_depth) > *config_.max_b_frames))
      return R::Failure(Error(CodecErrorCode::Unsupported, "Android changed the requested video reorder depth",
          std::to_string(reorder_depth)));
    Bytes description;
    for (const char* key : {"csd-0", "csd-1", "csd-2"}) {
      void* data = nullptr; std::size_t size = 0;
      if (!AMediaFormat_getBuffer(format, key, &data, &size)) continue;
      if (!data || size > 65536 - description.size())
        return R::Failure(Error(CodecErrorCode::ResourceExhausted, "Android video parameter sets exceed 64 KiB"));
      const auto* first = static_cast<const std::byte*>(data);
      description.insert(description.end(), first, first + size);
    }
    return ReadVideoEncodeFormat(description, config_);
  }
  VideoEncoderConfig config_;
  AMediaCodec* codec_ = nullptr;
  std::unique_ptr<ANativeWindow, decltype(&ANativeWindow_release)> surface_{nullptr, ANativeWindow_release};
  std::unique_ptr<AndroidVideoRenderer> renderer_;
  media_status_t (*surface_eos_)(AMediaCodec*) = nullptr;
  AMediaFormat* (*input_format_)(AMediaCodec*) = nullptr;
  media_status_t (*set_parameters_)(AMediaCodec*, const AMediaFormat*) = nullptr;
  VideoPixelFormat layout_ = VideoPixelFormat::NV12;
  std::size_t frame_bytes_ = 0;
  std::int64_t input_end_ = 0, partial_pts_ = 0;
  Bytes partial_;
  std::optional<VideoEncodeFormat> format_;
  std::optional<EncodedPacket> pending_;
  bool started_ = false, eos_ = false, pending_eos_ = false;
};
}
std::unique_ptr<VideoEncoderBackend> MakeVideoEncoderBackend() { return std::make_unique<AndroidVideoEncoder>(); }
} // namespace huxerui::media::detail
