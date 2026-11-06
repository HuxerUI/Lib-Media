#include "video_scheduler.h"

#include <algorithm>
#include <stdexcept>

namespace huxerui::media::detail {
namespace {
using Time = VideoScheduler::Time;

Time ToTime(MediaTimestamp timestamp) {
  const auto ticks = timestamp.Rescale(1000000000);
  if (!ticks) throw std::invalid_argument("Video scheduling timestamp is out of range");
  return Time(ticks->ticks);
}
} // namespace

VideoScheduler::Epoch VideoScheduler::Reset(std::optional<MediaTimestamp> audio_start) {
  if (closed_) throw std::logic_error("Video scheduler is closed");
  audio_start_ = audio_start ? std::optional(ToTime(*audio_start)) : std::nullopt;
  ++epoch_;
  queue_.clear();
  sealed_ = ended_ = audio_ended_ = running_ = false;
  display_remaining_ = Time::zero();
  position_.reset();
  audio_position_.reset();
  submitted_pts_.reset();
  displayed_end_.reset();
  last_tick_.reset();
  display_started_.reset();
  return epoch_;
}

void VideoScheduler::SetAudioStart(MediaTimestamp audio_start) {
  audio_start_ = ToTime(audio_start);
}

void VideoScheduler::Close() noexcept {
  closed_ = true;
  queue_.clear();
}

bool VideoScheduler::Current(Epoch epoch) const noexcept {
  return !closed_ && epoch != 0 && epoch == epoch_;
}

bool VideoScheduler::CanSubmit(Epoch epoch) const noexcept {
  // The second slot identifies an expired front frame that can be dropped even before EOS.
  return Current(epoch) && !sealed_ && queue_.size() < 2;
}

bool VideoScheduler::Submit(Epoch epoch, VideoFrame&& frame) {
  if (!CanSubmit(epoch)) return false;
  const auto pts = ToTime(frame.pts);
  const auto duration = frame.duration ? ToTime(*frame.duration) : Time::zero();
  if (duration <= Time::zero() || pts > Time::max() - duration || (submitted_pts_ && pts < *submitted_pts_))
    throw std::invalid_argument("Invalid video presentation interval");
  queue_.push_back({std::move(frame), pts, duration, pts + duration});
  submitted_pts_ = pts;
  return true;
}

bool VideoScheduler::Finish(Epoch epoch) noexcept {
  if (!Current(epoch)) return false;
  sealed_ = true;
  return true;
}

VideoScheduler::Update VideoScheduler::Tick(Epoch epoch, const PlaybackSnapshot* audio, bool paused, Clock::time_point now) {
  if (!Current(epoch)) return {};
  if (ended_) return {.position = position_, .ended = true};
  if (last_tick_ && now < *last_tick_) throw std::invalid_argument("Video scheduler clock regressed");

  const bool fallback = audio_ended_;
  if (!fallback && audio && audio->position) {
    const auto observed = ToTime(*audio->position);
    if (audio_position_ && observed < *audio_position_) throw std::invalid_argument("Audio playback clock regressed");
    audio_position_ = observed;
    if (audio_start_ && observed > *audio_start_) position_ = observed;
  }
  if (!fallback && (!audio || audio->state == PlaybackState::Ended)) {
    position_ = audio_position_ ? audio_position_ : audio_start_;
    audio_ended_ = true;
  }
  if (audio_ended_ && !position_ && !queue_.empty()) position_ = queue_.front().pts;
  if (audio_ended_ && sealed_ && queue_.empty() && !displayed_end_) {
    ended_ = true;
    return {.position = position_, .ended = true};
  }

  const bool running = !paused && position_ && (audio_ended_ || (audio && audio->state == PlaybackState::Playing));
  if (running && running_) {
    const auto elapsed = std::chrono::duration_cast<Time>(now - *last_tick_);
    if (display_started_) {
      const auto start = std::max(*last_tick_, *display_started_);
      if (now > start) display_remaining_ -= std::min(display_remaining_, std::chrono::duration_cast<Time>(now - start));
    }
    if (fallback) {
      *position_ = *position_ > Time::max() - elapsed ? Time::max() : *position_ + elapsed;
    }
  }
  running_ = running;
  last_tick_ = now;

  Update update{.position = position_};
  if (!running) return update;

  while (!queue_.empty() && queue_.front().pts <= *position_) {
    auto& front = queue_.front();
    if (front.end <= *position_) {
      if (queue_.size() > 1) {
        queue_.pop_front();
        ++update.dropped;
        continue;
      }
      if (!sealed_) break;
    }

    using Milliseconds = std::chrono::duration<double, std::milli>;
    update.frame = &front.frame;
    update.published_lag_ms = (Milliseconds(*position_) - Milliseconds(front.pts)).count();
    return update;
  }

  ended_ = audio_ended_ && sealed_ && queue_.empty() && display_remaining_ == Time::zero() &&
      (!displayed_end_ || *position_ >= *displayed_end_);
  update.ended = ended_;
  return update;
}

void VideoScheduler::ConfirmPublished(Clock::time_point now) noexcept {
  display_started_ = now;
  displayed_end_ = queue_.front().end;
  display_remaining_ = queue_.front().duration;
  queue_.pop_front();
}
} // namespace huxerui::media::detail
