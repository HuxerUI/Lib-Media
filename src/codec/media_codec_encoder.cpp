#include <huxerui/media_codec.h>

#include "codec/encoder/session.h"

namespace huxerui::media {
namespace {
auto AudioBackend(CodecId codec) {
#ifdef MEDIA_HAS_FLAC_ENCODER
  if (codec == CodecId::Flac) return detail::MakeFlacEncoderBackend;
#endif
#ifdef MEDIA_HAS_MP3_ENCODER
  if (codec == CodecId::Mp3) return detail::MakeMp3EncoderBackend;
#endif
  return detail::MakeAudioEncoderBackend;
}

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
        if (result.Succeeded()) { backend = AudioBackend(config.codec)(); result = backend->Open(config); }
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
  auto state = detail::AudioEncoderState::Create(config, AudioBackend(config.codec));
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
namespace {
auto VideoBackend(const VideoEncoderConfig& config) {
#ifdef MEDIA_HAS_AOM
  if (config.codec == CodecId::Av1) return detail::MakeAv1EncoderBackend;
#endif
  return detail::MakeVideoEncoderBackend;
}

struct VideoEncoderProbe : std::enable_shared_from_this<VideoEncoderProbe> {
  VideoEncoderConfig config;
  std::function<void(CodecResult<void>)> completion;
  std::unique_ptr<detail::VideoEncoderBackend> backend;
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
        auto valid = detail::ValidateVideoEncoderConfig(config);
        if (!valid.Succeeded()) result = CodecResult<void>::Failure(valid.Error());
        else { backend = VideoBackend(config)(); result = backend->Open(config); }
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
      auto closed = backend->Close(); backend.reset();
      if (!closed.Succeeded()) result = std::move(closed);
    }
#ifdef __EMSCRIPTEN__
    if (completion) completion(std::move(result));
#else
    worker->Finish(std::move(result), std::move(completion));
#endif
  }
};
template<class T> CodecResult<T> VideoClosed() {
  return CodecResult<T>::Failure(detail::Error(CodecErrorCode::Closed, "Video encoder handle has no session"));
}
}
VideoEncoder::VideoEncoder(std::shared_ptr<detail::VideoEncoderState> state) : state_(std::move(state)) {}
VideoEncoder::VideoEncoder(VideoEncoder&&) noexcept = default;
VideoEncoder& VideoEncoder::operator=(VideoEncoder&& other) noexcept {
  if (this != &other) { Close(); state_ = std::move(other.state_); }
  return *this;
}
VideoEncoder::~VideoEncoder() { Close(); }
CodecResult<VideoEncoder> VideoEncoder::Create(const VideoEncoderConfig& config) {
  auto state = detail::VideoEncoderState::Create(config, VideoBackend(config));
  if (!state.Succeeded()) return CodecResult<VideoEncoder>::Failure(state.Error());
  return CodecResult<VideoEncoder>::Success(VideoEncoder(std::move(state).Value()));
}
void VideoEncoder::Probe(const VideoEncoderConfig& config, std::function<void(CodecResult<void>)> completion) {
  auto probe = std::make_shared<VideoEncoderProbe>();
  probe->config = config; probe->config.wakeup = {}; probe->config.cleanup_completed = {};
  probe->completion = std::move(completion);
#ifndef __EMSCRIPTEN__
  probe->worker = detail::MediaWorker::Start([probe](bool) { probe->Run(); });
#endif
  probe->Schedule();
}
CodecResult<SubmitStatus> VideoEncoder::TrySubmit(const VideoFrame& frame) { return state_ ? state_->Submit(frame) : VideoClosed<SubmitStatus>(); }
CodecResult<std::optional<VideoEncodeOutput>> VideoEncoder::TryRead() { return state_ ? state_->Read() : VideoClosed<std::optional<VideoEncodeOutput>>(); }
CodecResult<void> VideoEncoder::RequestKeyFrame() { return state_ ? state_->RequestKeyFrame() : VideoClosed<void>(); }
CodecResult<void> VideoEncoder::Finish() { return state_ ? state_->Finish() : VideoClosed<void>(); }
CodecResult<void> VideoEncoder::Reset() { return state_ ? state_->Reset() : VideoClosed<void>(); }
void VideoEncoder::Close() noexcept { if (state_) state_->Close(); }
} // namespace huxerui::media
