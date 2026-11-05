#include "codec/session.h"

namespace huxerui::media {
namespace {
struct ProbeState : std::enable_shared_from_this<ProbeState> {
  detail::DecoderConfig config;
  std::function<void(CodecResult<void>)> completion;
  std::unique_ptr<detail::DecoderBackend> backend;
#ifndef __EMSCRIPTEN__
  std::shared_ptr<detail::CodecWorker> worker;
#endif
  void Schedule(unsigned delay = 0) {
#ifdef __EMSCRIPTEN__
    auto self = shared_from_this();
    detail::DispatchCodec([self] { self->Run(); }, delay);
#else
    worker->Schedule(delay);
#endif
  }
  void Run() {
    auto result = CodecResult<void>::Success();
    try {
      if (!backend) {
        const auto valid = detail::ValidateConfig(config);
        if (!valid.Succeeded()) result = CodecResult<void>::Failure(valid.Error());
        else { backend = detail::MakeDecoderBackend(); result = backend->Open(config); }
      }
      if (result.Succeeded()) {
        const auto ready = backend->Ready();
        if (!ready.Succeeded()) result = CodecResult<void>::Failure(ready.Error());
        else if (!ready.Value()) {
          Schedule(2);
          return;
        }
      }
    } catch (const std::exception& error) {
      result = CodecResult<void>::Failure(detail::Error(CodecErrorCode::PlatformFailure, error.what()));
    }
    if (backend) {
      auto closed = backend->Close();
      backend.reset();
      if (!closed.Succeeded()) result = std::move(closed);
    }
#ifdef __EMSCRIPTEN__
    if (completion) completion(std::move(result));
#else
    worker->Finish(std::move(result), std::move(completion));
#endif
  }
};
template<class Config> void ProbeConfig(const Config& config, std::function<void(CodecResult<void>)> completion) {
  auto probe = std::make_shared<ProbeState>();
  auto copy = config;
  copy.wakeup = {};
  copy.cleanup_completed = {};
  probe->config = std::move(copy);
  probe->completion = std::move(completion);
#ifndef __EMSCRIPTEN__
  probe->worker = detail::CodecWorker::Start([probe](bool) { probe->Run(); });
#endif
  probe->Schedule();
}
template<class T> CodecResult<T> Closed() {
  return CodecResult<T>::Failure(detail::Error(CodecErrorCode::Closed, "Decoder handle has no session"));
}
} // namespace
AudioDecoder::AudioDecoder(std::shared_ptr<detail::DecoderState<AudioDecodeOutput>> state) : state_(std::move(state)) {}
AudioDecoder::AudioDecoder(AudioDecoder&&) noexcept = default;
AudioDecoder& AudioDecoder::operator=(AudioDecoder&& other) noexcept {
  if (this != &other) { Close(); state_ = std::move(other.state_); }
  return *this;
}
AudioDecoder::~AudioDecoder() { Close(); }
CodecResult<AudioDecoder> AudioDecoder::Create(const AudioDecoderConfig& config) {
  auto state = detail::DecoderState<AudioDecodeOutput>::Create(config);
  if (!state.Succeeded()) return CodecResult<AudioDecoder>::Failure(state.Error());
  return CodecResult<AudioDecoder>::Success(AudioDecoder(std::move(state).Value()));
}
void AudioDecoder::Probe(const AudioDecoderConfig& config, std::function<void(CodecResult<void>)> completion) {
  ProbeConfig(config, std::move(completion));
}
CodecResult<SubmitStatus> AudioDecoder::TrySubmit(const EncodedPacket& packet) { return state_ ? state_->Submit(packet) : Closed<SubmitStatus>(); }
CodecResult<std::optional<AudioDecodeOutput>> AudioDecoder::TryRead() { return state_ ? state_->Read() : Closed<std::optional<AudioDecodeOutput>>(); }
CodecResult<void> AudioDecoder::Finish() { return state_ ? state_->Finish() : Closed<void>(); }
CodecResult<void> AudioDecoder::Reset() { return state_ ? state_->Reset() : Closed<void>(); }
void AudioDecoder::Close() noexcept { if (state_) state_->Close(); }

VideoDecoder::VideoDecoder(std::shared_ptr<detail::DecoderState<VideoDecodeOutput>> state) : state_(std::move(state)) {}
VideoDecoder::VideoDecoder(VideoDecoder&&) noexcept = default;
VideoDecoder& VideoDecoder::operator=(VideoDecoder&& other) noexcept {
  if (this != &other) { Close(); state_ = std::move(other.state_); }
  return *this;
}
VideoDecoder::~VideoDecoder() { Close(); }
CodecResult<VideoDecoder> VideoDecoder::Create(const VideoDecoderConfig& config) {
  auto state = detail::DecoderState<VideoDecodeOutput>::Create(config);
  if (!state.Succeeded()) return CodecResult<VideoDecoder>::Failure(state.Error());
  return CodecResult<VideoDecoder>::Success(VideoDecoder(std::move(state).Value()));
}
void VideoDecoder::Probe(const VideoDecoderConfig& config, std::function<void(CodecResult<void>)> completion) {
  ProbeConfig(config, std::move(completion));
}
CodecResult<SubmitStatus> VideoDecoder::TrySubmit(const EncodedPacket& packet) { return state_ ? state_->Submit(packet) : Closed<SubmitStatus>(); }
CodecResult<std::optional<VideoDecodeOutput>> VideoDecoder::TryRead() { return state_ ? state_->Read() : Closed<std::optional<VideoDecodeOutput>>(); }
CodecResult<void> VideoDecoder::Finish() { return state_ ? state_->Finish() : Closed<void>(); }
CodecResult<void> VideoDecoder::Reset() { return state_ ? state_->Reset() : Closed<void>(); }
void VideoDecoder::Close() noexcept { if (state_) state_->Close(); }
} // namespace huxerui::media
