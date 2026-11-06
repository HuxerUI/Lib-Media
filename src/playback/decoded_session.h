#pragma once

#include "session.h"
#include "video_scheduler.h"

namespace huxerui::media::detail {

class DecodedSession {
public:
  static PlaybackResult<std::shared_ptr<DecodedSession>> Create(
      const DecodedPlaybackConfig& config, PlaybackSession::Factory factory = MakePlaybackDevice);

  PlaybackEpoch Epoch() const;
  PlaybackResult<SubmitStatus> SubmitAudio(const PlaybackEpoch& epoch, const AudioFrame& frame);
  PlaybackResult<SubmitStatus> SubmitVideo(const PlaybackEpoch& epoch, const VideoFrame& frame);
  PlaybackResult<void> FinishAudio(const PlaybackEpoch& epoch);
  PlaybackResult<void> FinishVideo(const PlaybackEpoch& epoch);
  PlaybackResult<VideoPresentation> PresentVideo(const PlaybackEpoch& epoch,
                                                 const std::function<void(const VideoFrame&)>& publish);
  PlaybackResult<void> RequestPlay(bool play);
  PlaybackResult<void> SetVolume(double volume);
  PlaybackResult<void> SetMuted(bool muted);
  PlaybackResult<PlaybackEpoch> Reset();
  PlaybackSnapshot Snapshot() const;
  void Close() noexcept;

private:
  std::optional<PlaybackError> ControlError(const PlaybackEpoch* epoch = nullptr) const;
  PlaybackResult<void> MarkVideoReady();
  void RetireVideo() noexcept;

  std::mutex presentation_mutex_;
  mutable std::mutex mutex_;
  std::shared_ptr<PlaybackSession> audio_;
  std::optional<VideoScheduler> video_;
  PlaybackEpoch epoch_;
  VideoScheduler::Epoch video_epoch_ = 0;
  std::optional<MediaTimestamp> video_position_;
  bool closed_ = false, play_ = false, audio_anchored_ = false;
  bool video_ready_ = false, video_sealed_ = false, ended_ = false;
};

} // namespace huxerui::media::detail
