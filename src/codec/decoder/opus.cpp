#ifdef MEDIA_HAS_OPUS

#include "backend.h"
#include "copy.h"
#include "container/container_io.h"

#include <opus/opus_multistream.h>

namespace huxerui::media::detail {
namespace {

class MappedOpusDecoder final : public DecoderBackend {
public:
  CodecResult<void> Open(const DecoderConfig& config) override {
    const auto& audio = std::get<AudioDecoderConfig>(config);
    const auto head = ParseOpus(audio.initialization_data, ContainerErrorCode::InvalidArgument);
    output_ = audio.output;
    std::array<unsigned char, 8> mapping{};
    for (unsigned i = 0; i < head.channels; ++i)
      mapping[i] = head.mapping[WaveToVorbisChannel(i, head.channels)];
    int status = OPUS_OK;
    decoder_.reset(opus_multistream_decoder_create(48000, head.channels, head.streams, head.coupled, mapping.data(), &status));
    if (!decoder_ || status != OPUS_OK)
      return CodecResult<void>::Failure(Error(CodecErrorCode::Unsupported, "Cannot open mapped Opus decoder", opus_strerror(status)));
    const auto gain = Number<std::int16_t>(audio.initialization_data, 16, true);
    status = opus_multistream_decoder_ctl(decoder_.get(), OPUS_SET_GAIN(gain));
    if (status != OPUS_OK)
      return CodecResult<void>::Failure(Error(CodecErrorCode::Unsupported, "Cannot set Opus header gain", opus_strerror(status)));
    return CodecResult<void>::Success();
  }

  CodecResult<SubmitStatus> Submit(const EncodedPacket& packet) override {
    using R = CodecResult<SubmitStatus>;
    if (output_ready_) return R::Success(SubmitStatus::WouldBlock);
    const auto data = packet.data.AsBytes();
    if (data.size() > INT32_MAX)
      return R::Failure(Error(CodecErrorCode::InvalidData, "Mapped Opus packet exceeds its byte limit"));
    const auto channels = output_.ChannelCount();
    std::vector<float> samples(5760 * channels);
    const auto count = opus_multistream_decode_float(decoder_.get(), reinterpret_cast<const unsigned char*>(data.data()),
        static_cast<opus_int32>(data.size()), samples.data(), 5760, 0);
    if (count <= 0)
      return R::Failure(Error(CodecErrorCode::InvalidData, "Mapped Opus packet could not be decoded", opus_strerror(count)));
    auto format = output_;
    format.packing = AudioPacking::Interleaved;
    format.sample_format = AudioSampleFormat::F32;
    const auto bytes = std::as_bytes(std::span(samples).first(count * channels));
    AudioFrame frame{format, packet.pts, static_cast<std::uint32_t>(count), {Own(Bytes(bytes.begin(), bytes.end()))}};
    auto converted = format == output_ ? CodecResult<AudioFrame>::Success(std::move(frame)) : CopyAudio(frame, output_);
    if (!converted.Succeeded()) return R::Failure(converted.Error());
    output_ready_ = DecodedFrame{std::move(converted).Value()};
    return R::Success(SubmitStatus::Accepted);
  }

  CodecResult<std::optional<BackendOutput>> Poll() override {
    return CodecResult<std::optional<BackendOutput>>::Success(std::exchange(output_ready_, std::nullopt));
  }
  std::uint32_t PresentationTimescale() const noexcept override { return 48000; }
  CodecResult<SubmitStatus> Finish() override {
    if (output_ready_) return CodecResult<SubmitStatus>::Success(SubmitStatus::WouldBlock);
    output_ready_ = EndOfStream{};
    return CodecResult<SubmitStatus>::Success(SubmitStatus::Accepted);
  }
  CodecResult<void> Close() override {
    output_ready_.reset();
    decoder_.reset();
    return CodecResult<void>::Success();
  }

private:
  AudioFormat output_;
  std::optional<BackendOutput> output_ready_;
  std::unique_ptr<OpusMSDecoder, decltype(&opus_multistream_decoder_destroy)> decoder_{nullptr, opus_multistream_decoder_destroy};
};

} // namespace

std::unique_ptr<DecoderBackend> MakeMappedOpusDecoderBackend() { return std::make_unique<MappedOpusDecoder>(); }

} // namespace huxerui::media::detail

#endif
