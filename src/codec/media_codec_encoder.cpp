#include <huxerui/media_codec.h>

#include "codec/encoder/session.h"

namespace huxerui::media {
namespace {
struct EncoderProbe : std::enable_shared_from_this<EncoderProbe> {
  AudioEncoderConfig config;
  std::function<void(CodecResult<void>)> completion;
  std::unique_ptr<detail::AudioEncoderBackend> backend;
#ifndef __EMSCRIPTEN__
  std::shared_ptr<detail::MediaWorker> worker;
#endif
  void Schedule(unsigned delay = 0) {
#ifdef __EMSCRIPTEN__
    auto self = shared_from_this();
    detail::DispatchMedia([self] { self->Run(); }, delay);
#else
    worker->Schedule(delay);
#endif
  }
  void Run() {
    auto result = CodecResult<void>::Success();
    try {
      if (!backend) {
        result = detail::ValidateAudioEncoderConfig(config);
        if (result.Succeeded()) { backend = detail::MakeAudioEncoderBackend(); result = backend->Open(config); }
      }
      if (result.Succeeded()) {
        auto ready = backend->Ready();
        if (!ready.Succeeded()) result = CodecResult<void>::Failure(ready.Error());
        else if (!ready.Value()) { Schedule(2); return; }
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
template<class T> CodecResult<T> Closed() {
  return CodecResult<T>::Failure(detail::Error(CodecErrorCode::Closed, "Audio encoder handle has no session"));
}
} // namespace
AudioEncoder::AudioEncoder(std::shared_ptr<detail::AudioEncoderState> state) : state_(std::move(state)) {}
AudioEncoder::AudioEncoder(AudioEncoder&&) noexcept = default;
AudioEncoder& AudioEncoder::operator=(AudioEncoder&& other) noexcept {
  if (this != &other) { Close(); state_ = std::move(other.state_); }
  return *this;
}
AudioEncoder::~AudioEncoder() { Close(); }
CodecResult<AudioEncoder> AudioEncoder::Create(const AudioEncoderConfig& config) {
  auto state = detail::AudioEncoderState::Create(config);
  if (!state.Succeeded()) return CodecResult<AudioEncoder>::Failure(state.Error());
  return CodecResult<AudioEncoder>::Success(AudioEncoder(std::move(state).Value()));
}
void AudioEncoder::Probe(const AudioEncoderConfig& config, std::function<void(CodecResult<void>)> completion) {
  auto probe = std::make_shared<EncoderProbe>();
  probe->config = config;
  probe->config.wakeup = {};
  probe->config.cleanup_completed = {};
  probe->completion = std::move(completion);
#ifndef __EMSCRIPTEN__
  probe->worker = detail::MediaWorker::Start([probe](bool) { probe->Run(); });
#endif
  probe->Schedule();
}
CodecResult<SubmitStatus> AudioEncoder::TrySubmit(const AudioFrame& frame) { return state_ ? state_->Submit(frame) : Closed<SubmitStatus>(); }
CodecResult<std::optional<AudioEncodeOutput>> AudioEncoder::TryRead() { return state_ ? state_->Read() : Closed<std::optional<AudioEncodeOutput>>(); }
CodecResult<void> AudioEncoder::Finish() { return state_ ? state_->Finish() : Closed<void>(); }
CodecResult<void> AudioEncoder::Reset() { return state_ ? state_->Reset() : Closed<void>(); }
void AudioEncoder::Close() noexcept { if (state_) state_->Close(); }
} // namespace huxerui::media
