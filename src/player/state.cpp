#include "state.h"

#include <stdexcept>

namespace huxerui::media::detail {

PlayerState::PlayerState(Post dispatcher) : post(std::move(dispatcher)), owner_(std::this_thread::get_id()) {}

void PlayerState::CheckThread() const {
  if (owner_ != std::this_thread::get_id())
    throw std::logic_error("HuxerUI media player requires its owning UI thread");
}

void PlayerState::Attach(std::shared_ptr<PlayerBackend> player) {
  CheckThread();
  if (backend)
    throw std::logic_error("HuxerUI media player is already connected");
  backend = std::move(player);
  const auto epoch = ++attachment;
  std::weak_ptr<PlayerState> weak = shared_from_this();
  backend->emit = [weak, epoch, dispatch = post](PlayerEvent event) {
    dispatch([weak, epoch, event = std::move(event)]() mutable {
      if (auto state = weak.lock(); state && state->backend && state->attachment == epoch) {
        state->Receive(std::move(event));
      }
    });
  };
  backend->SetVolume(value.volume, value.muted);
  if (video_binding_)
    video_binding_(backend);
  Publish(true);
}

void PlayerState::Detach() noexcept {
  ++attachment;
  ++generation;
  if (backend) {
    backend->emit = {};
    if (video_binding_)
      video_binding_({});
    backend->Clear();
    backend.reset();
  }
  source.reset();
  active_seek = 0;
  pending_seek.reset();
  value = {};
  if (observed.IsValid())
    observed = value;
  if (observed_texture.IsValid())
    observed_texture = std::shared_ptr<ExternalTexture>{};
}

void PlayerState::ResetSource() {
  ++generation;
  backend->Clear();
  source.reset();
  const auto volume = value.volume;
  const auto muted = value.muted;
  const auto looping = value.looping;
  value = {};
  value.volume = volume;
  value.muted = muted;
  value.looping = looping;
  active_seek = 0;
  pending_seek.reset();
  loop_reported = false;
  loop_restart = false;
  if (observed_texture.IsValid())
    observed_texture = std::shared_ptr<ExternalTexture>{};
}

void PlayerState::NotifyProgress() {
  const auto notification = ++progress_notification_;
  const auto current_generation = generation;
  const auto current_attachment = attachment;
  std::weak_ptr<PlayerState> weak = shared_from_this();
  std::vector<std::uint64_t> subscriptions;
  for (const auto& [id, handler] : progress_handlers)
    subscriptions.push_back(id);
  post([weak, notification, current_generation, current_attachment, subscriptions = std::move(subscriptions)] {
    for (auto id : subscriptions) {
      auto state = weak.lock();
      if (!state || !state->backend || state->generation != current_generation ||
          state->attachment != current_attachment || state->progress_notification_ != notification)
        return;
      auto found = state->progress_handlers.find(id);
      if (found != state->progress_handlers.end()) {
        auto handler = found->second;
        const auto progress = state->value.progress;
        handler(progress);
      }
    }
  });
}

void PlayerState::Publish(bool force_progress) {
  if (observed.IsValid())
    observed = value;
  if (backend && observed_texture.IsValid())
    observed_texture = backend->Texture();
  if (force_progress || published_progress_ != value.progress) {
    published_progress_ = value.progress;
    NotifyProgress();
  }
}

void PlayerState::Report(MediaError error) {
  if (error.fatal) {
    if (value.status == MediaPlayerStatus::Failed)
      return;
    value.status = MediaPlayerStatus::Failed;
    value.error = error;
    value.play_when_ready = false;
    value.is_buffering = false;
    value.is_seeking = false;
    active_seek = 0;
    pending_seek.reset();
    backend->Pause();
  }
  Publish();
  const auto current_generation = generation;
  const auto current_attachment = attachment;
  std::weak_ptr<PlayerState> weak = shared_from_this();
  std::vector<std::uint64_t> subscriptions;
  for (const auto& [id, handler] : error_handlers)
    subscriptions.push_back(id);
  post([weak, current_generation, current_attachment, subscriptions = std::move(subscriptions), error] {
    for (auto id : subscriptions) {
      auto state = weak.lock();
      if (!state || !state->backend || state->generation != current_generation ||
          state->attachment != current_attachment)
        return;
      auto found = state->error_handlers.find(id);
      if (found != state->error_handlers.end()) {
        auto handler = found->second;
        handler(error);
      }
    }
  });
}

void PlayerState::CheckLoop() {
  if (value.looping && !loop_reported && value.seekability == MediaSeekability::NotSeekable) {
    loop_reported = true;
    Report({MediaErrorCode::Unsupported, "HuxerUI media source does not support looping", {}, false});
  }
}

void PlayerState::StartSeek(MediaTime position) {
  value.is_seeking = true;
  if (active_seek) {
    pending_seek = position;
    return;
  }
  active_seek = ++seek_serial;
  backend->Seek(position, active_seek);
}

void PlayerState::FinishEnded() {
  if (value.status == MediaPlayerStatus::Ended)
    return;
  value.status = MediaPlayerStatus::Ended;
  value.play_when_ready = false;
  value.is_buffering = false;
  Publish(true);
  const auto current_generation = generation;
  const auto current_attachment = attachment;
  std::weak_ptr<PlayerState> weak = shared_from_this();
  std::vector<std::uint64_t> subscriptions;
  for (const auto& [id, handler] : ended_handlers)
    subscriptions.push_back(id);
  post([weak, current_generation, current_attachment, subscriptions = std::move(subscriptions)] {
    for (auto id : subscriptions) {
      auto state = weak.lock();
      if (!state || !state->backend || state->generation != current_generation ||
          state->attachment != current_attachment)
        return;
      auto found = state->ended_handlers.find(id);
      if (found != state->ended_handlers.end()) {
        auto handler = found->second;
        handler();
      }
    }
  });
}

void PlayerState::Receive(PlayerEvent event) {
  CheckThread();
  if (!backend || !source || event.generation != generation || value.status == MediaPlayerStatus::Failed)
    return;
  if (event.kind == PlayerEventKind::Error) {
    const bool failed_loop = event.error.code == MediaErrorCode::SeekFailed && loop_restart && !event.error.fatal;
    if (event.error.code == MediaErrorCode::SeekFailed) {
      if (event.seek_id && event.seek_id != active_seek)
        return;
      active_seek = 0;
      pending_seek.reset();
      value.is_seeking = false;
      loop_restart = false;
    }
    if (event.error.code == MediaErrorCode::PlaybackNotAllowed) {
      value.play_when_ready = false;
      value.status = MediaPlayerStatus::Paused;
    }
    Report(std::move(event.error));
    if (failed_loop)
      FinishEnded();
    return;
  }
  if (event.kind == PlayerEventKind::Seeked && event.seek_id != active_seek)
    return;
  if (event.kind == PlayerEventKind::Seeked) {
    active_seek = 0;
    if (pending_seek) {
      auto next = *pending_seek;
      pending_seek.reset();
      StartSeek(next);
      return;
    }
    value.is_seeking = false;
  }
  if (!value.is_seeking)
    value.progress.position = event.snapshot.progress.position;
  value.progress.duration = event.snapshot.progress.duration;
  value.progress.buffered_ranges = std::move(event.snapshot.progress.buffered_ranges);
  value.seekability = event.snapshot.seekability;
  value.has_audio = event.snapshot.has_audio;
  value.has_video = event.snapshot.has_video;
  value.video_size = event.snapshot.video_size;
  value.is_buffering = event.snapshot.buffering;
  switch (event.kind) {
  case PlayerEventKind::Ready:
    if (value.status != MediaPlayerStatus::Loading)
      break;
    value.status = MediaPlayerStatus::Ready;
    if (value.play_when_ready)
      backend->Play();
    break;
  case PlayerEventKind::Playing:
    if (value.play_when_ready)
      value.status = MediaPlayerStatus::Playing;
    break;
  case PlayerEventKind::Paused:
    if (!value.play_when_ready && value.status != MediaPlayerStatus::Ended)
      value.status = MediaPlayerStatus::Paused;
    break;
  case PlayerEventKind::Seeked:
    if (value.status == MediaPlayerStatus::Ended)
      value.status = MediaPlayerStatus::Paused;
    if (loop_restart || value.play_when_ready) {
      loop_restart = false;
      backend->Play();
    }
    break;
  case PlayerEventKind::Interrupted:
    if (value.play_when_ready)
      value.status = MediaPlayerStatus::Interrupted;
    break;
  case PlayerEventKind::Ended: {
    if (value.is_seeking || value.status == MediaPlayerStatus::Ended)
      return;
    if (value.looping && value.play_when_ready && value.seekability == MediaSeekability::Seekable &&
        value.progress.duration) {
      loop_restart = true;
      StartSeek(MediaTime{});
      Publish();
      return;
    }
    FinishEnded();
    return;
  }
  default:
    break;
  }
  CheckLoop();
  Publish(event.kind == PlayerEventKind::Ready || event.kind == PlayerEventKind::Seeked ||
          event.kind == PlayerEventKind::Paused);
}

void PlayerState::Poll() {
  CheckThread();
  if (backend && source && value.status != MediaPlayerStatus::Failed)
    backend->Poll();
}

void PlayerState::Bind(VideoBinding binding) {
  CheckThread();
  if (video_binding_)
    throw std::logic_error("HuxerUI media player already has a video surface");
  video_binding_ = std::move(binding);
  if (backend)
    video_binding_(backend);
}

void PlayerState::Unbind() {
  CheckThread();
  if (video_binding_)
    video_binding_({});
  video_binding_ = {};
}

} // namespace huxerui::media::detail
