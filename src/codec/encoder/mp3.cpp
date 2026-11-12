#ifdef MEDIA_HAS_MP3_ENCODER

#include "backend.h"
#include "container/container_io.h"

#include <lame/lame.h>

#include <array>
#include <cstring>
#include <deque>

namespace huxerui::media::detail {
namespace {

class Mp3Encoder final : public AudioEncoderBackend {
public:
  CodecResult<void> Open(const AudioEncoderConfig& config) override {
    using R = CodecResult<void>;
    encoder_.reset(lame_init());
    if (!encoder_) return R::Failure(Error(CodecErrorCode::ResourceExhausted, "Cannot allocate the MP3 encoder"));
    rate_ = config.input.sample_rate;
    channels_ = config.input.ChannelCount();
    lame_set_write_id3tag_automatic(encoder_.get(), 0);
    const auto bitrate = static_cast<int>(config.bitrate / 1000);
    if (lame_set_in_samplerate(encoder_.get(), rate_) || lame_set_out_samplerate(encoder_.get(), rate_) ||
        lame_set_num_channels(encoder_.get(), channels_) || lame_set_mode(encoder_.get(), channels_ == 1 ? MONO : JOINT_STEREO) ||
        lame_set_VBR(encoder_.get(), vbr_off) || lame_set_brate(encoder_.get(), bitrate) ||
        lame_set_bWriteVbrTag(encoder_.get(), 0) || lame_init_params(encoder_.get()) < 0 ||
        lame_get_out_samplerate(encoder_.get()) != static_cast<int>(rate_) ||
        lame_get_brate(encoder_.get()) != bitrate || lame_get_framesize(encoder_.get()) != Block)
      return R::Failure(Error(CodecErrorCode::Unsupported, "MP3 encoder rejected the PCM geometry or bitrate"));
    priming_ = lame_get_encoder_delay(encoder_.get());
    if (priming_ < 0 || priming_ > 4095)
      return R::Failure(Error(CodecErrorCode::Unsupported, "MP3 encoder returned an invalid delay"));
    next_ = -priming_;
    output_.emplace_back(AudioEncodeFormat{rate_, config.input.channel_layout, {}, CodecId::Mp3});
    return R::Success();
  }

  CodecResult<SubmitStatus> Submit(ByteView bytes, std::uint32_t samples, std::int64_t) override {
    using R = CodecResult<SubmitStatus>;
    if (!output_.empty()) return R::Success(SubmitStatus::WouldBlock);
    std::memcpy(pcm_.data(), bytes.data(), bytes.size());
    const auto size = channels_ == 1 ? lame_encode_buffer(encoder_.get(), pcm_.data(), pcm_.data(), samples,
        buffer_.data(), buffer_.size()) : lame_encode_buffer_interleaved(encoder_.get(), pcm_.data(), samples,
        buffer_.data(), buffer_.size());
    return ReadFrames(size);
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
    auto result = ReadFrames(lame_encode_flush(encoder_.get(), buffer_.data(), buffer_.size()));
    if (!result.Succeeded()) return result;
    const auto padding = lame_get_encoder_padding(encoder_.get());
    if (!partial_.empty() || padding < 0 || padding > 4095)
      return R::Failure(Error(CodecErrorCode::InvalidData, "MP3 encoder drain returned incomplete timing or frame data"));
    output_.emplace_back(AudioEncoderEnd{static_cast<std::uint64_t>(priming_), static_cast<std::uint64_t>(padding)});
    return R::Success(SubmitStatus::Accepted);
  }

  CodecResult<void> Close() override {
    encoder_.reset();
    output_.clear();
    return CodecResult<void>::Success();
  }

private:
  CodecResult<SubmitStatus> ReadFrames(int size) {
    using R = CodecResult<SubmitStatus>;
    if (size < 0 || static_cast<std::size_t>(size) > buffer_.size())
      return R::Failure(Error(CodecErrorCode::PlatformFailure, "MP3 encoding failed", std::to_string(size)));
    Append(partial_, std::as_bytes(std::span(buffer_).first(size)));
    auto bytes = ByteView(partial_);
    while (bytes.size() >= 4) {
      const auto header = ParseMp3FrameHeader(bytes);
      if (!header || header->rate != rate_ || header->channels != channels_ || header->samples != Block)
        return R::Failure(Error(CodecErrorCode::InvalidData, "MP3 encoder returned an unexpected frame header"));
      if (bytes.size() < header->length) break;
      if (output_.size() >= kAudioEncoderExtraPackets)
        return R::Failure(Error(CodecErrorCode::ResourceExhausted, "MP3 encoder exceeded its reserved packet window"));
      const auto frame = bytes.first(header->length);
      output_.emplace_back(EncodedPacket{Own(Bytes(frame.begin(), frame.end())), {next_, rate_},
          {}, MediaTimestamp{Block, rate_}, Mp3FrameRestartable(frame, *header)});
      next_ += Block;
      bytes = bytes.subspan(header->length);
    }
    partial_.erase(partial_.begin(), partial_.begin() + (partial_.size() - bytes.size()));
    return R::Success(SubmitStatus::Accepted);
  }

  static constexpr int Block = AudioEncoderBlockSamples(CodecId::Mp3);
  std::unique_ptr<lame_global_flags, decltype(&lame_close)> encoder_{nullptr, lame_close};
  std::array<std::int16_t, Block * 2> pcm_{};
  // libmp3lame requires room for 1.25 bytes per input sample per channel plus 7200 bytes.
  std::array<unsigned char, Block * 5 / 4 + 7200> buffer_{};
  std::deque<AudioEncoderBackendOutput> output_;
  Bytes partial_;
  std::int64_t next_ = 0;
  unsigned rate_ = 0, channels_ = 0;
  int priming_ = 0;
};

} // namespace

std::unique_ptr<AudioEncoderBackend> MakeMp3EncoderBackend() { return std::make_unique<Mp3Encoder>(); }

} // namespace huxerui::media::detail

#endif
