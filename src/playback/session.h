#pragma once

#include "device.h"

#ifndef __EMSCRIPTEN__
#include "codec/executor.h"
#include <mutex>

namespace huxerui::media::detail {

class PlaybackSession {
public:
  using Factory = std::function<std::unique_ptr<PlaybackDevice>()>;
  static PlaybackResult<std::shared_ptr<PlaybackSession>> Create(
      const AudioPlaybackConfig& config, Factory factory = MakePlaybackDevice);
  PlaybackResult<SubmitStatus> Submit(const AudioFrame& frame);
  PlaybackResult<void> RequestPlay(bool play, bool allow_ended = false);
  PlaybackResult<void> SetVolume(double volume);
  PlaybackResult<void> SetMuted(bool muted);
  PlaybackResult<void> Finish();
  PlaybackResult<void> Reset();
  PlaybackSnapshot Snapshot() const;
  void Close() noexcept;

private:
  PlaybackSession(const AudioPlaybackConfig& config, std::size_t capacity, std::size_t refill, Factory factory);
  void Run();
  bool Tick(std::uint64_t epoch);
  std::size_t Write(std::uint64_t epoch);
  void Open();
  void Release();
  void Cleanup();
  void SetState(PlaybackState state);
  std::optional<PlaybackError> ControlError() const;
  bool Current(std::uint64_t epoch) const noexcept;

  const AudioFormat format_;
  const std::size_t capacity_, refill_, channels_;
  const Factory factory_;
  mutable std::mutex mutex_;
  PlaybackState state_ = PlaybackState::Starting;
  std::uint64_t epoch_ = 0, accepted_ = 0, committed_ = 0, presented_ = 0, underruns_ = 0;
  std::size_t blocked_frames_ = 0;
  std::optional<std::int64_t> anchor_;
  std::optional<PlaybackError> error_;
  bool play_ = false, sealed_ = false, muted_ = false, dirty_ = false, cleaned_ = false;
  double volume_ = 1;
  std::vector<std::int16_t> queue_, scratch_;
  std::function<void()> wakeup_;
  std::function<void(PlaybackResult<void>)> cleanup_completed_;
  std::function<void()> completion_;
  std::shared_ptr<CodecWorker> worker_;

  // Device operations and run-local state belong to the worker.
  enum class RunState { Priming, Running, Paused };
  std::unique_ptr<PlaybackDevice> device_;
  std::uint64_t run_base_ = 0;
  RunState run_state_ = RunState::Priming;
  bool accepts_media_ = true;
  std::optional<PlaybackError> release_error_;
};
} // namespace huxerui::media::detail
#endif
