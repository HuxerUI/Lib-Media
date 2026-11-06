#include "decoded_session.h"

#ifndef __EMSCRIPTEN__
#include <stdexcept>

namespace huxerui::media::detail {
namespace {

constexpr auto timeline_scale = 1000000000u;

bool ValidAudioInterval(const AudioFrame& frame) {
  const auto pts = frame.pts.Rescale(frame.format.sample_rate);
  if (!pts || pts->ticks > INT64_MAX - frame.sample_count || !pts->Rescale(timeline_scale)) return false;
  return MediaTimestamp{pts->ticks + frame.sample_count, pts->timescale}.Rescale(timeline_scale).has_value();
}

bool Opening(PlaybackState state) {
  return state == PlaybackState::Starting || state == PlaybackState::Resetting;
}
} // namespace

PlaybackResult<std::shared_ptr<DecodedSession>> DecodedSession::Create(
    const DecodedPlaybackConfig& config, PlaybackSession::Factory factory) {
  using R = PlaybackResult<std::shared_ptr<DecodedSession>>;
  if (!config.audio && !config.video)
    return PlaybackFailure<std::shared_ptr<DecodedSession>>(PlaybackErrorCode::InvalidArgument, "Enable an audio or video track");
  try {
    auto session = std::make_shared<DecodedSession>();
    session->epoch_ = PlaybackEpoch(std::make_shared<const char>());
    if (config.video) {
      session->video_.emplace();
      session->video_epoch_ = session->video_->Reset();
    }
    if (config.audio) {
      auto pcm_config = *config.audio;
      if (config.video) {
        pcm_config.cleanup_completed = [weak = std::weak_ptr(session), complete = std::move(pcm_config.cleanup_completed)](
                                      PlaybackResult<void> result) {
          if (auto owner = weak.lock()) owner->RetireVideo();
          if (complete) complete(std::move(result));
        };
      }
      auto pcm = PlaybackSession::Create(pcm_config, std::move(factory));
      if (!pcm.Succeeded()) return R::Failure(std::move(pcm).Error());
      session->audio_ = std::move(pcm).Value();
    }
    return R::Success(std::move(session));
  } catch (const std::bad_alloc&) {
    return PlaybackFailure<std::shared_ptr<DecodedSession>>(PlaybackErrorCode::ResourceExhausted, "Cannot allocate decoded playback");
  }
}

std::optional<PlaybackError> DecodedSession::ControlError(const PlaybackEpoch* epoch) const {
  if (closed_) return PlaybackError{PlaybackErrorCode::Closed, "Playback is closed", {}};
  if (epoch && *epoch != epoch_) return PlaybackError{PlaybackErrorCode::InvalidState, "Playback epoch is retired or foreign", {}};
  return audio_ ? audio_->Snapshot().error : std::nullopt;
}

PlaybackEpoch DecodedSession::Epoch() const {
  std::lock_guard lock(mutex_);
  return epoch_;
}

PlaybackResult<SubmitStatus> DecodedSession::SubmitAudio(const PlaybackEpoch& epoch, const AudioFrame& frame) {
  using R = PlaybackResult<SubmitStatus>;
  std::lock_guard lock(mutex_);
  if (auto error = ControlError(&epoch)) return R::Failure(std::move(*error));
  if (!audio_) return PlaybackFailure<SubmitStatus>(PlaybackErrorCode::InvalidState, "Audio input is disabled");
  if (video_ && !ValidAudioInterval(frame))
    return PlaybackFailure<SubmitStatus>(PlaybackErrorCode::InvalidArgument, "Audio interval exceeds the video timeline range");

  auto result = audio_->Submit(frame);
  if (video_ && !audio_anchored_ && result.Succeeded() && result.Value() == SubmitStatus::Accepted) {
    video_->SetAudioStart(*frame.pts.Rescale(frame.format.sample_rate));
    audio_anchored_ = true;
  }
  return result;
}

PlaybackResult<SubmitStatus> DecodedSession::SubmitVideo(const PlaybackEpoch& epoch, const VideoFrame& frame) {
  using R = PlaybackResult<SubmitStatus>;
  std::lock_guard lock(mutex_);
  if (auto error = ControlError(&epoch)) return R::Failure(std::move(*error));
  if (!video_ || video_sealed_)
    return PlaybackFailure<SubmitStatus>(PlaybackErrorCode::InvalidState, "Video input is disabled or sealed");
  if ((audio_ && Opening(audio_->Snapshot().state)) || !video_->CanSubmit(video_epoch_))
    return R::Success(SubmitStatus::WouldBlock);
  if (!frame.Validate())
    return PlaybackFailure<SubmitStatus>(PlaybackErrorCode::InvalidArgument, "Invalid decoded video frame");
  try {
    video_->Submit(video_epoch_, VideoFrame(frame));
  } catch (const std::invalid_argument& error) {
    return PlaybackFailure<SubmitStatus>(PlaybackErrorCode::InvalidArgument, error.what());
  } catch (const std::bad_alloc&) {
    return PlaybackFailure<SubmitStatus>(PlaybackErrorCode::ResourceExhausted, "Cannot retain decoded video frame");
  }
  auto ready = MarkVideoReady();
  if (!ready.Succeeded()) return R::Failure(std::move(ready).Error());
  return R::Success(SubmitStatus::Accepted);
}

PlaybackResult<void> DecodedSession::FinishAudio(const PlaybackEpoch& epoch) {
  std::lock_guard lock(mutex_);
  if (auto error = ControlError(&epoch)) return PlaybackResult<void>::Failure(std::move(*error));
  if (!audio_) return PlaybackFailure(PlaybackErrorCode::InvalidState, "Audio input is disabled");
  return audio_->Finish();
}

PlaybackResult<void> DecodedSession::FinishVideo(const PlaybackEpoch& epoch) {
  std::lock_guard lock(mutex_);
  if (auto error = ControlError(&epoch)) return PlaybackResult<void>::Failure(std::move(*error));
  if (!video_) return PlaybackFailure(PlaybackErrorCode::InvalidState, "Video input is disabled");
  video_->Finish(video_epoch_);
  video_sealed_ = true;
  return MarkVideoReady();
}

PlaybackResult<void> DecodedSession::MarkVideoReady() {
  if (!video_ready_ && audio_ && play_) {
    auto result = audio_->RequestPlay(true, true);
    if (!result.Succeeded()) {
      // This is the first video admission. No publication can own a queued frame yet.
      video_->Close();
      return result;
    }
  }
  video_ready_ = true;
  return PlaybackResult<void>::Success();
}

PlaybackResult<VideoPresentation> DecodedSession::PresentVideo(const PlaybackEpoch& epoch,
                                                               const std::function<void(const VideoFrame&)>& publish) {
  using R = PlaybackResult<VideoPresentation>;
  std::lock_guard presentation(presentation_mutex_);
  std::unique_lock lock(mutex_);
  if (auto error = ControlError(&epoch)) return R::Failure(std::move(*error));
  if (!video_) return PlaybackFailure<VideoPresentation>(PlaybackErrorCode::InvalidState, "Video input is disabled");
  if (!publish) return PlaybackFailure<VideoPresentation>(PlaybackErrorCode::InvalidArgument, "Missing video publisher");
  const auto audio = audio_ ? std::optional(audio_->Snapshot()) : std::nullopt;
  const auto update = video_->Tick(video_epoch_, audio ? &*audio : nullptr, !play_, VideoScheduler::Clock::now());
  if (update.position) video_position_ = MediaTimestamp{update.position->count(), timeline_scale};
  ended_ = update.ended;
  if (update.frame) {
    // Submission only appends. The presentation lock prevents retirement of this queued frame.
    lock.unlock();
    publish(*update.frame);
    const auto published = VideoScheduler::Clock::now();
    lock.lock();
    video_->ConfirmPublished(published);
  }
  return R::Success({update.published_lag_ms, update.dropped});
}

PlaybackResult<void> DecodedSession::RequestPlay(bool play) {
  std::lock_guard lock(mutex_);
  if (auto error = ControlError()) return PlaybackResult<void>::Failure(std::move(*error));
  if (!video_) return audio_->RequestPlay(play);
  if (ended_) return PlaybackFailure(PlaybackErrorCode::InvalidState, "Reset is required after Ended");
  if (audio_ && (!play || video_ready_)) {
    auto result = audio_->RequestPlay(play, true);
    if (!result.Succeeded()) return result;
  }
  if (!play) video_->Suspend();
  play_ = play;
  return PlaybackResult<void>::Success();
}

PlaybackResult<void> DecodedSession::SetVolume(double volume) {
  std::lock_guard lock(mutex_);
  if (auto error = ControlError()) return PlaybackResult<void>::Failure(std::move(*error));
  if (!audio_) return PlaybackFailure(PlaybackErrorCode::InvalidState, "Audio output is disabled");
  return audio_->SetVolume(volume);
}

PlaybackResult<void> DecodedSession::SetMuted(bool muted) {
  std::lock_guard lock(mutex_);
  if (auto error = ControlError()) return PlaybackResult<void>::Failure(std::move(*error));
  if (!audio_) return PlaybackFailure(PlaybackErrorCode::InvalidState, "Audio output is disabled");
  return audio_->SetMuted(muted);
}

PlaybackResult<PlaybackEpoch> DecodedSession::Reset() {
  using R = PlaybackResult<PlaybackEpoch>;
  std::lock_guard presentation(presentation_mutex_);
  std::lock_guard lock(mutex_);
  if (auto error = ControlError()) return R::Failure(std::move(*error));
  if (audio_ && audio_->Snapshot().state == PlaybackState::Resetting) return R::Success(epoch_);
  try {
    auto epoch = PlaybackEpoch(std::make_shared<const char>());
    if (audio_) {
      auto result = audio_->Reset();
      if (!result.Succeeded()) return R::Failure(std::move(result).Error());
    }
    if (video_) video_epoch_ = video_->Reset();
    epoch_ = std::move(epoch);
    play_ = audio_anchored_ = video_ready_ = video_sealed_ = ended_ = false;
    video_position_.reset();
    return R::Success(epoch_);
  } catch (const std::bad_alloc&) {
    return PlaybackFailure<PlaybackEpoch>(PlaybackErrorCode::ResourceExhausted, "Cannot allocate playback epoch");
  }
}

PlaybackSnapshot DecodedSession::Snapshot() const {
  std::lock_guard lock(mutex_);
  if (closed_) return !video_ && audio_ ? audio_->Snapshot() : PlaybackSnapshot{};
  auto snapshot = audio_ ? audio_->Snapshot() : PlaybackSnapshot{};
  if (!video_ || (audio_ && (Opening(snapshot.state) || snapshot.state == PlaybackState::Failed))) return snapshot;
  if ((!audio_ || snapshot.state == PlaybackState::Ended) && video_position_ &&
      (!snapshot.position || video_position_->Compare(*snapshot.position) == 1)) snapshot.position = video_position_;
  if (ended_) snapshot.state = PlaybackState::Ended;
  else if (!play_) {
    if (!audio_ || snapshot.state == PlaybackState::Ended) snapshot.state = PlaybackState::Paused;
  } else if (!audio_ || snapshot.state == PlaybackState::Ended) {
    snapshot.state = snapshot.position ? PlaybackState::Playing : PlaybackState::Buffering;
  } else if (snapshot.state == PlaybackState::Paused) {
    snapshot.state = PlaybackState::Buffering;
  }
  return snapshot;
}

void DecodedSession::RetireVideo() noexcept {
  std::lock_guard presentation(presentation_mutex_);
  std::lock_guard lock(mutex_);
  if (video_) video_->Close();
}

void DecodedSession::Close() noexcept {
  std::lock_guard presentation(presentation_mutex_);
  std::lock_guard lock(mutex_);
  if (closed_) return;
  closed_ = true;
  epoch_ = {};
  if (video_) video_->Close();
  if (audio_) audio_->Close();
}

} // namespace huxerui::media::detail
#endif
