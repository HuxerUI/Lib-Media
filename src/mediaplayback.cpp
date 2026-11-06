#include "playback/session.h"

#if !defined(__ANDROID__) && !defined(MEDIA_PLAYBACK_MACOS)
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

DecodedPlayback::DecodedPlayback(std::shared_ptr<detail::PlaybackSession> session) : session_(std::move(session)) {}
DecodedPlayback::DecodedPlayback(DecodedPlayback&&) noexcept = default;
DecodedPlayback& DecodedPlayback::operator=(DecodedPlayback&& other) noexcept {
  if (this != &other) { Close(); session_ = std::move(other.session_); }
  return *this;
}
DecodedPlayback::~DecodedPlayback() { Close(); }

#ifdef __EMSCRIPTEN__
PlaybackResult<DecodedPlayback> DecodedPlayback::Create(const AudioPlaybackConfig&) {
  return detail::PlaybackFailure<DecodedPlayback>(PlaybackErrorCode::Unsupported, "PCM playback is unavailable on Web");
}
PlaybackResult<SubmitStatus> DecodedPlayback::TrySubmitAudio(const AudioFrame&) { return Closed<SubmitStatus>(); }
PlaybackResult<void> DecodedPlayback::Play() { return Closed(); }
PlaybackResult<void> DecodedPlayback::Pause() { return Closed(); }
PlaybackResult<void> DecodedPlayback::SetVolume(double) { return Closed(); }
PlaybackResult<void> DecodedPlayback::SetMuted(bool) { return Closed(); }
PlaybackResult<void> DecodedPlayback::FinishAudio() { return Closed(); }
PlaybackResult<void> DecodedPlayback::Reset() { return Closed(); }
PlaybackSnapshot DecodedPlayback::Snapshot() const { return {}; }
void DecodedPlayback::Close() noexcept {}
#else
PlaybackResult<DecodedPlayback> DecodedPlayback::Create(const AudioPlaybackConfig& config) {
#if defined(__ANDROID__) || defined(MEDIA_PLAYBACK_MACOS)
  auto session = detail::PlaybackSession::Create(config);
  if (!session.Succeeded()) return PlaybackResult<DecodedPlayback>::Failure(std::move(session).Error());
  return PlaybackResult<DecodedPlayback>::Success(DecodedPlayback(std::move(session).Value()));
#else
  return detail::PlaybackFailure<DecodedPlayback>(PlaybackErrorCode::Unsupported, "PCM playback is unavailable on this platform");
#endif
}
PlaybackResult<SubmitStatus> DecodedPlayback::TrySubmitAudio(const AudioFrame& frame) { return session_ ? session_->Submit(frame) : Closed<SubmitStatus>(); }
PlaybackResult<void> DecodedPlayback::Play() { return session_ ? session_->RequestPlay(true) : Closed(); }
PlaybackResult<void> DecodedPlayback::Pause() { return session_ ? session_->RequestPlay(false) : Closed(); }
PlaybackResult<void> DecodedPlayback::SetVolume(double volume) { return session_ ? session_->SetVolume(volume) : Closed(); }
PlaybackResult<void> DecodedPlayback::SetMuted(bool muted) { return session_ ? session_->SetMuted(muted) : Closed(); }
PlaybackResult<void> DecodedPlayback::FinishAudio() { return session_ ? session_->Finish() : Closed(); }
PlaybackResult<void> DecodedPlayback::Reset() { return session_ ? session_->Reset() : Closed(); }
PlaybackSnapshot DecodedPlayback::Snapshot() const { return session_ ? session_->Snapshot() : PlaybackSnapshot{}; }
void DecodedPlayback::Close() noexcept { if (session_) session_->Close(); }
#endif
} // namespace huxerui::media
