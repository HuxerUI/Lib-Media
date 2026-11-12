#ifdef MEDIA_HAS_FLAC_ENCODER

#include "backend.h"
#include "container/container_io.h"

#include <FLAC/stream_encoder.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>

namespace huxerui::media::detail {
namespace {

class FlacEncoder final : public AudioEncoderBackend {
public:
  CodecResult<void> Open(const AudioEncoderConfig& config) override {
    using R = CodecResult<void>;
    format_ = {config.input.sample_rate, config.input.channel_layout, {}, CodecId::Flac};
    channels_ = config.input.ChannelCount();
    float_input_ = config.input.sample_format == AudioSampleFormat::F32;
    encoder_.reset(FLAC__stream_encoder_new());
    if (!encoder_) return R::Failure(Error(CodecErrorCode::ResourceExhausted, "Cannot allocate the FLAC encoder"));
    const auto block = AudioEncoderBlockSamples(CodecId::Flac);
    pcm_.resize(block * channels_);
    if (!FLAC__stream_encoder_set_channels(encoder_.get(), channels_) ||
        !FLAC__stream_encoder_set_sample_rate(encoder_.get(), format_.sample_rate) ||
        !FLAC__stream_encoder_set_bits_per_sample(encoder_.get(), float_input_ ? 24 : 16) ||
        !FLAC__stream_encoder_set_compression_level(encoder_.get(), 5) ||
        !FLAC__stream_encoder_set_blocksize(encoder_.get(), block))
      return R::Failure(Error(CodecErrorCode::Unsupported, "FLAC encoder rejected the PCM format"));
    const auto status = FLAC__stream_encoder_init_stream(encoder_.get(), Write, nullptr, nullptr, nullptr, this);
    if (status != FLAC__STREAM_ENCODER_INIT_STATUS_OK)
      return R::Failure(Error(allocation_failed_ ? CodecErrorCode::ResourceExhausted : CodecErrorCode::Unsupported,
          "Cannot initialize the FLAC encoder", FLAC__StreamEncoderInitStatusString[status]));
    if (format_.initialization_data.empty())
      return R::Failure(Error(CodecErrorCode::InvalidData, "FLAC encoder did not return STREAMINFO"));
    output_.emplace_back(format_);
    return R::Success();
  }

  CodecResult<SubmitStatus> Submit(ByteView bytes, std::uint32_t samples, std::int64_t) override {
    using R = CodecResult<SubmitStatus>;
    if (!output_.empty()) return R::Success(SubmitStatus::WouldBlock);
    const auto values = samples * channels_;
    for (unsigned i = 0; i < values; ++i) {
      if (float_input_) {
        float sample;
        std::memcpy(&sample, bytes.data() + i * sizeof(sample), sizeof(sample));
        if (!std::isfinite(sample)) return R::Failure(Error(CodecErrorCode::InvalidData, "Nonfinite FLAC PCM sample"));
        constexpr auto scale = 8388608.0;
        pcm_[i] = static_cast<FLAC__int32>(std::min(std::lround(std::clamp(double(sample), -1.0, 1.0) * scale), 8388607L));
      } else {
        std::int16_t sample;
        std::memcpy(&sample, bytes.data() + i * sizeof(sample), sizeof(sample));
        pcm_[i] = sample;
      }
    }
    if (!FLAC__stream_encoder_process_interleaved(encoder_.get(), pcm_.data(), samples))
      return R::Failure(Failure("FLAC encoding failed"));
    return R::Success(SubmitStatus::Accepted);
  }

  CodecResult<std::optional<AudioEncoderBackendOutput>> Poll() override {
    using R = CodecResult<std::optional<AudioEncoderBackendOutput>>;
    if (output_.empty()) return R::Success(std::nullopt);
    auto next = std::move(output_.front());
    output_.pop_front();
    return R::Success(std::move(next));
  }

  CodecResult<SubmitStatus> Finish() override {
    using R = CodecResult<SubmitStatus>;
    if (!output_.empty()) return R::Success(SubmitStatus::WouldBlock);
    if (!FLAC__stream_encoder_finish(encoder_.get())) return R::Failure(Failure("FLAC encoder drain failed"));
    output_.emplace_back(AudioEncoderEnd{0, 0});
    return R::Success(SubmitStatus::Accepted);
  }

  CodecResult<void> Close() override {
    encoder_.reset();
    output_.clear();
    return CodecResult<void>::Success();
  }

private:
  CodecError Failure(const char* message) const {
    return Error(allocation_failed_ ? CodecErrorCode::ResourceExhausted : CodecErrorCode::PlatformFailure, message,
                 FLAC__stream_encoder_get_resolved_state_string(encoder_.get()));
  }

  static FLAC__StreamEncoderWriteStatus Write(const FLAC__StreamEncoder*, const FLAC__byte* data, std::size_t size,
      std::uint32_t samples, std::uint32_t, void* context) noexcept {
    auto& self = *static_cast<FlacEncoder*>(context);
    try {
      const auto bytes = std::as_bytes(std::span(data, size));
      if (!samples) {
        if (size == 38 && (data[0] & 127) == 0 && (Number<std::uint32_t>(bytes) & 0xffffffu) == 34)
          self.format_.initialization_data.assign(bytes.begin() + 4, bytes.end());
      } else {
        if (!size || size > kAudioEncoderPacketBytes || self.output_.size() >= kAudioEncoderExtraPackets)
          return FLAC__STREAM_ENCODER_WRITE_STATUS_FATAL_ERROR;
        const auto rate = self.format_.sample_rate;
        self.output_.emplace_back(EncodedPacket{Own(Bytes(bytes.begin(), bytes.end())), {self.next_, rate},
            {}, MediaTimestamp{samples, rate}, true});
        self.next_ += samples;
      }
      return FLAC__STREAM_ENCODER_WRITE_STATUS_OK;
    } catch (...) {
      self.allocation_failed_ = true;
      return FLAC__STREAM_ENCODER_WRITE_STATUS_FATAL_ERROR;
    }
  }

  std::unique_ptr<FLAC__StreamEncoder, decltype(&FLAC__stream_encoder_delete)> encoder_{nullptr, FLAC__stream_encoder_delete};
  std::vector<FLAC__int32> pcm_;
  AudioEncodeFormat format_;
  std::deque<AudioEncoderBackendOutput> output_;
  std::int64_t next_ = 0;
  unsigned channels_ = 0;
  bool float_input_ = false, allocation_failed_ = false;
};

} // namespace

std::unique_ptr<AudioEncoderBackend> MakeFlacEncoderBackend() { return std::make_unique<FlacEncoder>(); }

} // namespace huxerui::media::detail

#endif
