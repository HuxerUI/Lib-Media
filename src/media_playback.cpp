#include "playback/decoded_session.h"
#include "playback/device.h"

#if !defined(__ANDROID__) && !defined(__APPLE__) && !defined(__EMSCRIPTEN__)
namespace huxerui::media::detail {
std::unique_ptr<PlaybackDevice> MakePlaybackDevice() { return {}; }
} // namespace huxerui::media::detail
#endif

namespace huxerui::media {
namespace {
template<class T = void> PlaybackResult<T> Closed() {
  return detail::PlaybackFailure<T>(PlaybackErrorCode::Closed, "Playback handle has no session");
}
} // namespace

DecodedPlayback::DecodedPlayback(std::shared_ptr<detail::DecodedSession> session) : session_(std::move(session)) {}
DecodedPlayback::DecodedPlayback(DecodedPlayback&&) noexcept = default;
DecodedPlayback& DecodedPlayback::operator=(DecodedPlayback&& other) noexcept {
  if (this != &other) { Close(); session_ = std::move(other.session_); }
  return *this;
}
DecodedPlayback::~DecodedPlayback() { Close(); }

PlaybackResult<DecodedPlayback> DecodedPlayback::Create(const DecodedPlaybackConfig& config) {
#if defined(__ANDROID__) || defined(__APPLE__) || defined(__EMSCRIPTEN__)
  auto session = detail::DecodedSession::Create(config);
  if (!session.Succeeded()) return PlaybackResult<DecodedPlayback>::Failure(std::move(session).Error());
  return PlaybackResult<DecodedPlayback>::Success(DecodedPlayback(std::move(session).Value()));
#else
  return detail::PlaybackFailure<DecodedPlayback>(PlaybackErrorCode::Unsupported, "Decoded playback is unavailable on this platform");
#endif
}

PlaybackResult<SubmitStatus> DecodedPlayback::TrySubmitAudio(const PlaybackEpoch& epoch, const AudioFrame& frame) {
  return session_ ? session_->SubmitAudio(epoch, frame) : Closed<SubmitStatus>();
}
PlaybackResult<SubmitStatus> DecodedPlayback::TrySubmitVideo(const PlaybackEpoch& epoch, const VideoFrame& frame) {
  return session_ ? session_->SubmitVideo(epoch, frame) : Closed<SubmitStatus>();
}
PlaybackResult<void> DecodedPlayback::FinishVideo(const PlaybackEpoch& epoch) {
  return session_ ? session_->FinishVideo(epoch) : Closed();
}
PlaybackResult<VideoPresentation> DecodedPlayback::PresentVideo(
    const PlaybackEpoch& epoch, const std::function<void(const VideoFrame&)>& publish) {
  return session_ ? session_->PresentVideo(epoch, publish) : Closed<VideoPresentation>();
}
PlaybackResult<void> DecodedPlayback::Play() { return session_ ? session_->RequestPlay(true) : Closed(); }
PlaybackResult<void> DecodedPlayback::Pause() { return session_ ? session_->RequestPlay(false) : Closed(); }
PlaybackResult<void> DecodedPlayback::SetVolume(double volume) { return session_ ? session_->SetVolume(volume) : Closed(); }
PlaybackResult<void> DecodedPlayback::SetMuted(bool muted) { return session_ ? session_->SetMuted(muted) : Closed(); }
PlaybackResult<void> DecodedPlayback::FinishAudio(const PlaybackEpoch& epoch) {
  return session_ ? session_->FinishAudio(epoch) : Closed();
}
PlaybackResult<PlaybackEpoch> DecodedPlayback::Reset() { return session_ ? session_->Reset() : Closed<PlaybackEpoch>(); }
PlaybackEpoch DecodedPlayback::Epoch() const { return session_ ? session_->Epoch() : PlaybackEpoch{}; }
PlaybackSnapshot DecodedPlayback::Snapshot() const { return session_ ? session_->Snapshot() : PlaybackSnapshot{}; }
void DecodedPlayback::Close() noexcept { if (session_) session_->Close(); }
} // namespace huxerui::media
