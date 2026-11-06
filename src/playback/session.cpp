#include "session.h"

#ifndef __EMSCRIPTEN__
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace huxerui::media::detail {
namespace {

template<class T> T Require(PlaybackResult<T> result) {
  if (!result.Succeeded()) throw std::move(result).Error();
  if constexpr (!std::is_void_v<T>) return std::move(result).Value();
}

std::optional<std::size_t> Frames(std::chrono::milliseconds duration, std::uint32_t rate) {
  const auto count = duration.count();
  if (count <= 0 || count > INT32_MAX || !rate || rate > INT32_MAX) return {};
  const auto frames = (std::int64_t(count) * rate + 999) / 1000;
  if (frames > INT32_MAX) return {};
  return std::size_t(frames);
}
} // namespace

PlaybackSession::PlaybackSession(const AudioPlaybackConfig& config, std::size_t capacity, std::size_t refill,
                                 Factory factory)
    : format_(config.format), capacity_(capacity), refill_(refill), channels_(format_.ChannelCount()),
      factory_(std::move(factory)), queue_(capacity_ * channels_),
      scratch_(std::min(capacity_, std::max<std::size_t>(1, format_.sample_rate / 10)) * channels_),
      wakeup_(config.wakeup), cleanup_completed_(config.cleanup_completed) {}

PlaybackResult<std::shared_ptr<PlaybackSession>> PlaybackSession::Create(const AudioPlaybackConfig& config,
                                                                          Factory factory) {
  using R = PlaybackResult<std::shared_ptr<PlaybackSession>>;
  if (!factory) return PlaybackFailure<std::shared_ptr<PlaybackSession>>(PlaybackErrorCode::Unsupported, "PCM playback is unavailable");
  const auto capacity = Frames(config.buffer_capacity, config.format.sample_rate);
  const auto refill = Frames(config.refill_threshold, config.format.sample_rate);
  if (!capacity || !refill || config.refill_threshold > config.buffer_capacity || !config.format.ChannelCount() ||
      *capacity > std::vector<std::int16_t>().max_size() / config.format.ChannelCount())
    return PlaybackFailure<std::shared_ptr<PlaybackSession>>(PlaybackErrorCode::InvalidArgument, "Invalid playback format or queue bounds");
  if (config.format.sample_format != AudioSampleFormat::S16 || config.format.packing != AudioPacking::Interleaved)
    return PlaybackFailure<std::shared_ptr<PlaybackSession>>(PlaybackErrorCode::Unsupported, "Playback requires interleaved S16 PCM");
  try {
    auto session = std::shared_ptr<PlaybackSession>(new PlaybackSession(config, *capacity, *refill, std::move(factory)));
    std::function<void()> completion;
    if (session->cleanup_completed_) {
      completion = [session] {
        auto callback = std::move(session->cleanup_completed_);
        auto result = session->release_error_ ? PlaybackResult<void>::Failure(std::move(*session->release_error_))
                                              : PlaybackResult<void>::Success();
        callback(std::move(result));
      };
    }
    session->worker_ = CodecWorker::Start([session](bool) { session->Run(); });
    session->completion_ = std::move(completion);
    session->worker_->Schedule();
    return R::Success(std::move(session));
  } catch (const std::bad_alloc&) {
    return PlaybackFailure<std::shared_ptr<PlaybackSession>>(PlaybackErrorCode::ResourceExhausted, "Cannot allocate playback storage");
  } catch (const std::system_error& error) {
    return PlaybackFailure<std::shared_ptr<PlaybackSession>>(PlaybackErrorCode::ResourceExhausted, "Cannot start playback worker", error.what());
  }
}

std::optional<PlaybackError> PlaybackSession::ControlError() const {
  if (state_ == PlaybackState::Closed) return PlaybackError{PlaybackErrorCode::Closed, "Playback is closed", {}};
  if (state_ == PlaybackState::Failed) return error_;
  return {};
}

PlaybackResult<SubmitStatus> PlaybackSession::Submit(const AudioFrame& frame) {
  using R = PlaybackResult<SubmitStatus>;
  std::lock_guard lock(mutex_);
  if (auto error = ControlError()) return R::Failure(std::move(*error));
  if (state_ == PlaybackState::Starting || state_ == PlaybackState::Resetting) return R::Success(SubmitStatus::WouldBlock);
  if (sealed_) return PlaybackFailure<SubmitStatus>(PlaybackErrorCode::InvalidState, "Audio input is sealed");
  if (frame.format != format_ || frame.sample_count > capacity_ || !frame.Validate())
    return PlaybackFailure<SubmitStatus>(PlaybackErrorCode::InvalidArgument, "Invalid PCM frame for this playback queue");
  const auto pts = frame.pts.Rescale(format_.sample_rate);
  if (!pts || (anchor_ && pts->ticks != *anchor_ + std::int64_t(accepted_)) ||
      pts->ticks > INT64_MAX - frame.sample_count || accepted_ > std::uint64_t(INT64_MAX) - frame.sample_count)
    return PlaybackFailure<SubmitStatus>(PlaybackErrorCode::InvalidArgument, "PCM timestamps must be contiguous on the playback sample grid");
  if (frame.sample_count > capacity_ - (accepted_ - presented_)) {
    if (blocked_frames_ != frame.sample_count) {
      blocked_frames_ = frame.sample_count;
      worker_->Schedule();
    }
    return R::Success(SubmitStatus::WouldBlock);
  }

  const auto start = std::size_t(accepted_ % capacity_);
  const auto first = std::min<std::size_t>(frame.sample_count, capacity_ - start) * channels_;
  const auto remaining = frame.sample_count * channels_ - first;
  const auto bytes = frame.planes[0].AsBytes();
  std::memcpy(queue_.data() + start * channels_, bytes.data(), first * sizeof(std::int16_t));
  if (remaining) std::memcpy(queue_.data(), bytes.data() + first * sizeof(std::int16_t), remaining * sizeof(std::int16_t));
  if (!anchor_) anchor_ = pts->ticks;
  accepted_ += frame.sample_count;
  blocked_frames_ = 0;
  worker_->Schedule();
  return R::Success(SubmitStatus::Accepted);
}

PlaybackResult<void> PlaybackSession::RequestPlay(bool play, bool allow_ended) {
  std::lock_guard lock(mutex_);
  if (auto error = ControlError()) return PlaybackResult<void>::Failure(std::move(*error));
  if (state_ == PlaybackState::Ended) {
    if (allow_ended) return PlaybackResult<void>::Success();
    return PlaybackFailure(PlaybackErrorCode::InvalidState, "Reset is required after Ended");
  }
  play_ = play;
  worker_->Schedule();
  return PlaybackResult<void>::Success();
}
PlaybackResult<void> PlaybackSession::SetVolume(double volume) {
  std::lock_guard lock(mutex_);
  if (auto error = ControlError()) return PlaybackResult<void>::Failure(std::move(*error));
  if (!std::isfinite(volume) || volume < 0 || volume > 1)
    return PlaybackFailure(PlaybackErrorCode::InvalidArgument, "Volume must be in [0, 1]");
  volume_ = volume;
  return PlaybackResult<void>::Success();
}
PlaybackResult<void> PlaybackSession::SetMuted(bool muted) {
  std::lock_guard lock(mutex_);
  if (auto error = ControlError()) return PlaybackResult<void>::Failure(std::move(*error));
  muted_ = muted;
  return PlaybackResult<void>::Success();
}
PlaybackResult<void> PlaybackSession::Finish() {
  std::lock_guard lock(mutex_);
  if (auto error = ControlError()) return PlaybackResult<void>::Failure(std::move(*error));
  if (state_ == PlaybackState::Starting || state_ == PlaybackState::Resetting)
    return PlaybackFailure(PlaybackErrorCode::InvalidState, "Playback input is not ready");
  sealed_ = true;
  worker_->Schedule();
  return PlaybackResult<void>::Success();
}
PlaybackResult<void> PlaybackSession::Reset() {
  std::lock_guard lock(mutex_);
  if (auto error = ControlError()) return PlaybackResult<void>::Failure(std::move(*error));
  if (state_ == PlaybackState::Starting)
    return PlaybackFailure(PlaybackErrorCode::InvalidState, "Playback is still opening");
  if (state_ == PlaybackState::Resetting) return PlaybackResult<void>::Success();
  ++epoch_;
  play_ = sealed_ = false;
  accepted_ = committed_ = presented_ = underruns_ = 0;
  blocked_frames_ = 0;
  anchor_.reset();
  SetState(PlaybackState::Resetting);
  worker_->Schedule();
  return PlaybackResult<void>::Success();
}
PlaybackSnapshot PlaybackSession::Snapshot() const {
  std::lock_guard lock(mutex_);
  const auto pending = state_ == PlaybackState::Closed || state_ == PlaybackState::Failed ? 0 : accepted_ - presented_;
  return {state_, anchor_ ? std::optional(MediaTimestamp{*anchor_ + std::int64_t(presented_), format_.sample_rate}) : std::nullopt,
          {std::int64_t(pending), format_.sample_rate}, underruns_, sealed_, error_};
}
void PlaybackSession::Close() noexcept {
  std::lock_guard lock(mutex_);
  if (state_ == PlaybackState::Closed) return;
  ++epoch_;
  state_ = PlaybackState::Closed;
  error_.reset();
  if (!cleaned_) worker_->Schedule();
}
void PlaybackSession::SetState(PlaybackState state) {
  if (state_ != state) { state_ = state; dirty_ = true; }
}
bool PlaybackSession::Current(std::uint64_t epoch) const noexcept {
  return epoch == epoch_ && state_ != PlaybackState::Closed && state_ != PlaybackState::Failed;
}

void PlaybackSession::Open() {
  device_ = factory_();
  if (!device_) throw PlaybackError{PlaybackErrorCode::Unsupported, "PCM playback is unavailable", {}};
  Require(device_->Open(format_, capacity_));
}
void PlaybackSession::Release() {
  if (device_) {
    auto result = device_->Close();
    device_.reset();
    if (!result.Succeeded() && !release_error_) release_error_ = std::move(result).Error();
  }
  run_state_ = RunState::Priming;
  accepts_media_ = true;
}

std::size_t PlaybackSession::Write(std::uint64_t epoch) {
  std::size_t requested;
  double gain;
  {
    std::lock_guard lock(mutex_);
    if (!Current(epoch) || !play_ || !accepts_media_) return 0;
    requested = std::min<std::uint64_t>(accepted_ - committed_, scratch_.size() / channels_);
    if (!requested) return 0;
    const auto start = std::size_t(committed_ % capacity_);
    const auto first = std::min(requested, capacity_ - start) * channels_;
    std::copy_n(queue_.data() + start * channels_, first, scratch_.data());
    std::copy_n(queue_.data(), requested * channels_ - first, scratch_.data() + first);
    gain = muted_ ? 0 : volume_;
  }
  auto samples = std::span(scratch_).first(requested * channels_);
  if (gain != 1) for (auto& sample : samples) sample = std::int16_t(sample * gain);
  const auto written = Require(device_->Write(samples));
  if (written > requested) throw PlaybackError{PlaybackErrorCode::DeviceFailure, "Invalid device write count", {}};
  std::lock_guard lock(mutex_);
  if (Current(epoch)) committed_ += written;
  return written;
}

bool PlaybackSession::Tick(std::uint64_t epoch) {
  PlaybackState state;
  bool observe, input_empty;
  {
    std::lock_guard lock(mutex_);
    state = state_;
    if (!Current(epoch) || state == PlaybackState::Ended) return false;
    observe = play_ && run_state_ == RunState::Running;
    input_empty = committed_ == accepted_;
  }
  if (state == PlaybackState::Starting || state == PlaybackState::Resetting) {
    Release();
    if (release_error_) throw *release_error_;
    run_base_ = 0;
    Open();
    std::lock_guard lock(mutex_);
    if (!Current(epoch)) return false;
    SetState(PlaybackState::Paused);
    return play_;
  }

  if (observe) {
    const auto progress = Require(device_->Observe(input_empty));
    std::lock_guard lock(mutex_);
    if (!Current(epoch)) return false;
    if (progress.presented > committed_ - run_base_)
      throw PlaybackError{PlaybackErrorCode::DeviceFailure, "Device progress exceeds committed PCM", {}};
    const auto count = run_base_ + progress.presented;
    if (count > presented_) {
      presented_ = count;
      dirty_ = true;
    }
    accepts_media_ = progress.accepts_media;
  }

  auto next = PlaybackState::Playing;
  {
    std::lock_guard lock(mutex_);
    if (!Current(epoch)) return false;
    const auto pending = accepted_ - presented_;
    if (sealed_ && !pending) {
      next = PlaybackState::Ended;
    } else if (run_state_ == RunState::Running && presented_ == committed_ &&
               (!accepts_media_ || !pending)) {
      next = PlaybackState::Buffering;
    } else if (!play_) {
      next = PlaybackState::Paused;
    } else if (run_state_ == RunState::Priming) {
      // A producer blocked by the queue must be able to make progress below the usual refill threshold.
      const bool blocked = blocked_frames_ > capacity_ - pending;
      if (!pending || (!sealed_ && !blocked && pending < refill_)) {
        SetState(PlaybackState::Buffering);
        return false;
      }
    }
    if (!device_) run_base_ = presented_;
  }

  if (next != PlaybackState::Playing) {
    if (next == PlaybackState::Buffering && !accepts_media_) {
      Release();
      if (release_error_) throw *release_error_;
    } else if (run_state_ == RunState::Running) {
      Require(device_->Pause());
      run_state_ = RunState::Paused;
    }
    std::lock_guard lock(mutex_);
    if (!Current(epoch)) return false;
    if (next == PlaybackState::Buffering) {
      ++underruns_;
      run_state_ = RunState::Priming;
      if (!play_) next = PlaybackState::Paused;
    }
    SetState(next);
    return next != PlaybackState::Ended && play_;
  }

  if (!device_) Open();
  const auto written = Write(epoch);
  if (run_state_ != RunState::Running) {
    {
      std::lock_guard lock(mutex_);
      if (!Current(epoch) || !play_) return false;
      const auto queued = committed_ - presented_;
      if (!queued || (run_state_ == RunState::Priming && written &&
                      queued < std::min<std::uint64_t>(refill_, accepted_ - presented_)))
        return true;
    }
    Require(device_->Start());
    run_state_ = RunState::Running;
    std::lock_guard lock(mutex_);
    if (Current(epoch)) SetState(PlaybackState::Playing);
  }
  return true;
}

void PlaybackSession::Cleanup() {
  Release();
  {
    std::lock_guard lock(mutex_);
    cleaned_ = true;
    std::vector<std::int16_t>().swap(queue_);
    std::vector<std::int16_t>().swap(scratch_);
  }
  wakeup_ = {};
  worker_->Finish(std::move(completion_));
}
void PlaybackSession::Run() {
  std::uint64_t epoch;
  {
    std::lock_guard lock(mutex_);
    if (cleaned_) return;
    epoch = epoch_;
  }
  bool poll = false;
  std::optional<PlaybackError> failure;
  try {
    poll = Tick(epoch);
  } catch (PlaybackError& error) {
    failure = std::move(error);
  } catch (const std::bad_alloc&) {
    failure = PlaybackError{PlaybackErrorCode::ResourceExhausted, "Cannot allocate device buffers", {}};
  } catch (const std::exception& error) {
    failure = PlaybackError{PlaybackErrorCode::DeviceFailure, error.what(), {}};
  }

  bool notify;
  {
    std::lock_guard lock(mutex_);
    if (failure && Current(epoch)) {
      error_ = std::move(failure);
      SetState(PlaybackState::Failed);
    }
    notify = dirty_ && state_ != PlaybackState::Closed;
    dirty_ = false;
  }
  if (notify && wakeup_) wakeup_();
  {
    std::lock_guard lock(mutex_);
    if (state_ != PlaybackState::Closed && state_ != PlaybackState::Failed) {
      if (poll) worker_->Schedule(4);
      return;
    }
  }
  Cleanup();
}
} // namespace huxerui::media::detail
#endif
