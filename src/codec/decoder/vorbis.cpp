#include "backend.h"
#include "copy.h"
#include "../vorbis.h"

namespace huxerui::media::detail {
namespace {

class VorbisDecoder final : public DecoderBackend {
public:
  ~VorbisDecoder() override { Release(); }

  CodecResult<void> Open(const DecoderConfig& config) override {
    const auto& audio = std::get<AudioDecoderConfig>(config);
    output_ = audio.output;
    try {
      headers_ = std::make_unique<VorbisHeaders>(audio.initialization_data, ContainerErrorCode::InvalidArgument);
    } catch (const ContainerError& error) {
      return CodecResult<void>::Failure(Error(CodecErrorCode::InvalidArgument, error.message));
    }
    if (vorbis_synthesis_init(&dsp_, headers_->Info()) || vorbis_block_init(&dsp_, &block_))
      return CodecResult<void>::Failure(Error(CodecErrorCode::ResourceExhausted, "Cannot initialize Vorbis synthesis"));
    return CodecResult<void>::Success();
  }

  CodecResult<SubmitStatus> Submit(const EncodedPacket& packet) override {
    using R = CodecResult<SubmitStatus>;
    if (ready_) return R::Success(SubmitStatus::WouldBlock);
    auto input = VorbisPacket(packet.data.AsBytes());
    input.packetno = sequence_++;
    if (vorbis_synthesis(&block_, &input) || vorbis_synthesis_blockin(&dsp_, &block_))
      return R::Failure(Error(CodecErrorCode::InvalidData, "Invalid Vorbis synthesis packet"));
    float** planes = nullptr;
    const auto count = vorbis_synthesis_pcmout(&dsp_, &planes);
    const auto expected = VorbisOverlap(previous_, static_cast<unsigned>(block_.pcmend));
    if (count < 0 || static_cast<unsigned>(count) != expected || expected > headers_->MaxSamples())
      return R::Failure(Error(CodecErrorCode::InvalidData, "Vorbis PCM count differs from its overlap window"));
    previous_ = static_cast<unsigned>(block_.pcmend);
    if (count) {
      auto format = output_;
      format.packing = AudioPacking::Planar;
      format.sample_format = AudioSampleFormat::F32;
      AudioFrame source{format, packet.pts, expected, {}};
      for (unsigned channel = 0; channel < format.ChannelCount(); ++channel) {
        const auto bytes = std::as_bytes(std::span(planes[channel], expected));
        source.planes.push_back(Own(Bytes(bytes.begin(), bytes.end())));
      }
      auto frame = format == output_ ? CodecResult<AudioFrame>::Success(std::move(source)) : CopyAudio(source, output_);
      if (!frame.Succeeded()) return R::Failure(frame.Error());
      if (vorbis_synthesis_read(&dsp_, count))
        return R::Failure(Error(CodecErrorCode::InvalidData, "Cannot retire the decoded Vorbis samples"));
      ready_ = std::move(frame).Value();
    }
    return R::Success(SubmitStatus::Accepted);
  }

  CodecResult<std::optional<BackendOutput>> Poll() override {
    using R = CodecResult<std::optional<BackendOutput>>;
    if (auto frame = std::exchange(ready_, std::nullopt)) return R::Success(BackendOutput{DecodedFrame{std::move(*frame)}});
    if (std::exchange(finished_, false)) return R::Success(BackendOutput{EndOfStream{}});
    return R::Success(std::nullopt);
  }

  std::uint32_t PresentationTimescale() const noexcept override { return output_.sample_rate; }

  CodecResult<SubmitStatus> Finish() override {
    finished_ = true;
    return CodecResult<SubmitStatus>::Success(SubmitStatus::Accepted);
  }

  CodecResult<void> Close() override {
    Release();
    return CodecResult<void>::Success();
  }

private:
  void Release() {
    vorbis_block_clear(&block_);
    vorbis_dsp_clear(&dsp_);
    headers_.reset();
    ready_.reset();
  }

  std::unique_ptr<VorbisHeaders> headers_;
  vorbis_dsp_state dsp_{};
  vorbis_block block_{};
  AudioFormat output_;
  std::optional<AudioFrame> ready_;
  ogg_int64_t sequence_ = 3;
  unsigned previous_ = 0;
  bool finished_ = false;
};

} // namespace

std::unique_ptr<DecoderBackend> MakeVorbisDecoderBackend() { return std::make_unique<VorbisDecoder>(); }

} // namespace huxerui::media::detail
