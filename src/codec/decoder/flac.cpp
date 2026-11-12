#include "backend.h"
#include "copy.h"
#include "container/container_io.h"

namespace huxerui::media::detail {
namespace {

class MultichannelFlacDecoder final : public DecoderBackend {
public:
  CodecResult<void> Open(const DecoderConfig& config) override {
    const auto& audio = std::get<AudioDecoderConfig>(config);
    info_ = ParseFlacStreamInfo(audio.initialization_data, ContainerErrorCode::InvalidArgument);
    output_ = audio.output;
    return CodecResult<void>::Success();
  }

  CodecResult<SubmitStatus> Submit(const EncodedPacket& packet) override {
    using R = CodecResult<SubmitStatus>;
    if (ready_) return R::Success(SubmitStatus::WouldBlock);
    try {
      auto source = DecodeMultichannelFlac(packet.data.AsBytes(), info_, output_, packet.pts);
      auto frame = source.format == output_ ? CodecResult<AudioFrame>::Success(std::move(source)) : CopyAudio(source, output_);
      if (!frame.Succeeded()) return R::Failure(frame.Error());
      ready_ = std::move(frame).Value();
    } catch (const ContainerError& error) {
      return R::Failure(Error(CodecErrorCode::InvalidData, error.message));
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
    ready_.reset();
    finished_ = false;
    return CodecResult<void>::Success();
  }

private:
  FlacStreamInfo info_{};
  AudioFormat output_;
  std::optional<AudioFrame> ready_;
  bool finished_ = false;
};

} // namespace

std::unique_ptr<DecoderBackend> MakeMultichannelFlacDecoderBackend() { return std::make_unique<MultichannelFlacDecoder>(); }

} // namespace huxerui::media::detail
