#pragma once

#include <chrono>
#include <deque>

#include <huxerui/media_playback.h>

namespace huxerui::media::detail {

// The owner serializes calls and keeps a selected frame alive through publication.
// Tag each producer and clock observation with the epoch captured when its work was started.
class VideoScheduler {
public:
  using Epoch = std::uint64_t;
  using Clock = std::chrono::steady_clock;
  using Time = std::chrono::nanoseconds;

  struct Update {
    std::optional<Time> position;
    const VideoFrame* frame = nullptr;
    std::optional<double> published_lag_ms;
    std::size_t dropped = 0;
    bool ended = false;
  };

  // audio_start is the PTS of the first PCM sample accepted for this epoch.
  Epoch Reset(std::optional<MediaTimestamp> audio_start = {});
  void SetAudioStart(MediaTimestamp audio_start);
  void Suspend() noexcept { running_ = false; }
  void Close() noexcept;
  bool CanSubmit(Epoch epoch) const noexcept;
  // False preserves the frame if the epoch is stale or cannot accept input.
  bool Submit(Epoch epoch, VideoFrame&& frame);
  bool Finish(Epoch epoch) noexcept;
  // A selected frame stays in the queue until publication succeeds.
  Update Tick(Epoch epoch, const PlaybackSnapshot* audio, bool paused, Clock::time_point now);
  void ConfirmPublished(Clock::time_point now) noexcept;

private:
  struct QueuedFrame {
    VideoFrame frame;
    Time pts;
    Time duration;
    Time end;
  };

  bool Current(Epoch epoch) const noexcept;

  std::deque<QueuedFrame> queue_;
  Epoch epoch_ = 0;
  bool closed_ = false, sealed_ = false, ended_ = false, audio_ended_ = false, running_ = false;
  Time display_remaining_{};
  std::optional<Time> audio_start_, position_, audio_position_, submitted_pts_, displayed_end_;
  std::optional<Clock::time_point> last_tick_, display_started_;
};

} // namespace huxerui::media::detail
