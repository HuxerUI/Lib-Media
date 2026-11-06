#include <huxerui/media_player.h>
#include "detail/media_library.h"

#include <algorithm>
#include <cmath>
#include <cctype>

#include <huxerui/task.h>

#include "detail/mediaplayer_internal.h"
#if defined(__ANDROID__)
#include "detail/channel_player.h"
#endif

namespace huxerui::media::detail {

PlayerState::PlayerState(Post dispatcher) : post(std::move(dispatcher)), owner_(std::this_thread::get_id()) {}

void PlayerState::CheckThread() const {
  if (owner_ != std::this_thread::get_id())
    throw std::logic_error("HuxerUI media player requires its owning UI thread");
}

void PlayerState::Attach(std::shared_ptr<NativePlayer> value_native) {
  CheckThread();
  if (native)
    throw std::logic_error("HuxerUI media player is already connected");
  native = std::move(value_native);
  const auto epoch = ++attachment;
  std::weak_ptr<PlayerState> weak = shared_from_this();
  native->emit = [weak, epoch, dispatch = post](NativeEvent event) {
    dispatch([weak, epoch, event = std::move(event)]() mutable {
      if (auto state = weak.lock(); state && state->native && state->attachment == epoch) {
        state->Receive(std::move(event));
      }
    });
  };
  native->SetVolume(value.volume, value.muted);
  if (video_binding_)
    video_binding_(native);
  Publish(true);
}

void PlayerState::Detach() noexcept {
  ++attachment;
  ++generation;
  if (native) {
    native->emit = {};
    if (video_binding_)
      video_binding_({});
    native->Clear();
    native.reset();
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
  native->Clear();
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
      if (!state || !state->native || state->generation != current_generation ||
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
  if (native && observed_texture.IsValid())
    observed_texture = native->Texture();
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
    native->Pause();
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
      if (!state || !state->native || state->generation != current_generation ||
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
  native->Seek(position, active_seek);
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
      if (!state || !state->native || state->generation != current_generation ||
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

void PlayerState::Receive(NativeEvent event) {
  CheckThread();
  if (!native || !source || event.generation != generation || value.status == MediaPlayerStatus::Failed)
    return;
  if (event.kind == NativeEventKind::Error) {
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
  if (event.kind == NativeEventKind::Seeked && event.seek_id != active_seek)
    return;
  if (event.kind == NativeEventKind::Seeked) {
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
  case NativeEventKind::Ready:
    if (value.status != MediaPlayerStatus::Loading)
      break;
    value.status = MediaPlayerStatus::Ready;
    if (value.play_when_ready)
      native->Play();
    break;
  case NativeEventKind::Playing:
    if (value.play_when_ready)
      value.status = MediaPlayerStatus::Playing;
    break;
  case NativeEventKind::Paused:
    if (!value.play_when_ready && value.status != MediaPlayerStatus::Ended)
      value.status = MediaPlayerStatus::Paused;
    break;
  case NativeEventKind::Seeked:
    if (value.status == MediaPlayerStatus::Ended)
      value.status = MediaPlayerStatus::Paused;
    if (loop_restart || value.play_when_ready) {
      loop_restart = false;
      native->Play();
    }
    break;
  case NativeEventKind::Interrupted:
    if (value.play_when_ready)
      value.status = MediaPlayerStatus::Interrupted;
    break;
  case NativeEventKind::Ended: {
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
  Publish(event.kind == NativeEventKind::Ready || event.kind == NativeEventKind::Seeked ||
          event.kind == NativeEventKind::Paused);
}

void PlayerState::Poll() {
  CheckThread();
  if (native && source && value.status != MediaPlayerStatus::Failed)
    native->Poll();
}

void PlayerState::Bind(VideoBinding binding) {
  CheckThread();
  if (video_binding_)
    throw std::logic_error("HuxerUI media player already has a video surface");
  video_binding_ = std::move(binding);
  if (native)
    video_binding_(native);
}

void PlayerState::Unbind() {
  CheckThread();
  if (video_binding_)
    video_binding_({});
  video_binding_ = {};
}

void ValidateVideoProperties(const VideoSurfaceProperties& properties) {
  if (properties.horizontal_alignment == HorizontalAlignment::Stretch ||
      properties.vertical_alignment == VerticalAlignment::Stretch) {
    throw std::invalid_argument("HuxerUI video content alignment cannot be Stretch");
  }
}

} // namespace huxerui::media::detail

namespace huxerui::media {

MediaSource::MediaSource(File file) : value_(std::move(file)) {}
MediaSource::MediaSource(FileReference reference) : value_(std::move(reference)) {}
MediaSource::MediaSource(MediaHttpSource source) : value_(std::move(source)) {
  const auto& http = std::get<MediaHttpSource>(value_);
  const auto uri = Uri::Parse(http.url);
  std::string scheme = uri ? std::string(uri->Scheme()) : std::string{};
  std::transform(scheme.begin(), scheme.end(), scheme.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (!uri || (scheme != "http" && scheme != "https") || !uri->Authority() || uri->Authority()->empty()) {
    throw std::invalid_argument("HuxerUI media source requires an absolute HTTP or HTTPS URL");
  }
  for (const auto& [name, value] : http.headers) {
    const bool valid_name = !name.empty() && std::all_of(name.begin(), name.end(), [](unsigned char c) {
      return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
             std::string_view("!#$%&'*+-.^_`|~").find(c) != std::string_view::npos;
    });
    if (!valid_name ||
        std::any_of(value.begin(), value.end(), [](unsigned char c) { return c == 0 || c == '\r' || c == '\n'; })) {
      throw std::invalid_argument("HuxerUI media HTTP header is malformed");
    }
  }
}

bool MediaPlayer::IsConnected() const noexcept {
  return state_ && static_cast<bool>(state_->native);
}

MediaPlayerState MediaPlayer::Snapshot() const {
  state_->CheckThread();
  return state_->observed.IsValid() ? state_->observed.Get() : state_->value;
}

bool MediaPlayer::Load(MediaSource source) const {
  state_->CheckThread();
  if (!IsConnected())
    return false;
  state_->ResetSource();
  state_->source = std::move(source);
  state_->value.status = MediaPlayerStatus::Loading;
  state_->Publish(true);
  state_->native->Load(*state_->source, state_->generation);
  return true;
}

bool MediaPlayer::Clear() const {
  state_->CheckThread();
  if (!IsConnected())
    return false;
  if (!state_->source && state_->value.status == MediaPlayerStatus::Empty)
    return true;
  state_->ResetSource();
  state_->Publish(true);
  return true;
}

bool MediaPlayer::Play() const {
  state_->CheckThread();
  auto& value = state_->value;
  if (!IsConnected() || value.status == MediaPlayerStatus::Empty || value.status == MediaPlayerStatus::Failed)
    return false;
  if (value.play_when_ready && value.status != MediaPlayerStatus::Interrupted)
    return true;
  if (value.status == MediaPlayerStatus::Ended &&
      (value.seekability != MediaSeekability::Seekable || !value.progress.duration))
    return false;
  value.play_when_ready = true;
  if (value.status == MediaPlayerStatus::Ended)
    state_->StartSeek(MediaTime{});
  else if (value.status != MediaPlayerStatus::Loading && !value.is_seeking)
    state_->native->Play();
  state_->Publish();
  return true;
}

bool MediaPlayer::Pause() const {
  state_->CheckThread();
  auto& value = state_->value;
  if (!IsConnected() || value.status == MediaPlayerStatus::Empty || value.status == MediaPlayerStatus::Failed)
    return false;
  if (!value.play_when_ready)
    return true;
  value.play_when_ready = false;
  state_->loop_restart = false;
  if (value.status != MediaPlayerStatus::Loading)
    state_->native->Pause();
  state_->Publish(true);
  return true;
}

bool MediaPlayer::SeekTo(MediaTime position) const {
  state_->CheckThread();
  if (!std::isfinite(position.count()) || position.count() < 0)
    throw std::invalid_argument("HuxerUI media seek time must be finite and nonnegative");
  auto& value = state_->value;
  if (!IsConnected() || value.status == MediaPlayerStatus::Failed || value.status == MediaPlayerStatus::Loading ||
      value.seekability != MediaSeekability::Seekable)
    return false;
  if (value.progress.duration)
    position = std::min(position, *value.progress.duration);
  state_->StartSeek(position);
  state_->Publish();
  return true;
}

bool MediaPlayer::SetLooping(bool looping) const {
  state_->CheckThread();
  if (!IsConnected())
    return false;
  state_->value.looping = looping;
  if (!looping)
    state_->loop_reported = false;
  state_->CheckLoop();
  state_->Publish();
  return true;
}

bool MediaPlayer::SetVolume(double volume) const {
  state_->CheckThread();
  if (!std::isfinite(volume) || volume < 0 || volume > 1)
    throw std::invalid_argument("HuxerUI media volume must be in [0, 1]");
  if (!IsConnected())
    return false;
  state_->value.volume = volume;
  state_->native->SetVolume(volume, state_->value.muted);
  state_->Publish();
  return true;
}

bool MediaPlayer::SetMuted(bool muted) const {
  state_->CheckThread();
  if (!IsConnected())
    return false;
  state_->value.muted = muted;
  state_->native->SetVolume(state_->value.volume, muted);
  state_->Publish();
  return true;
}

std::function<void()> MediaPlayer::ConnectProgress(std::function<void(const MediaProgress&)> handler) const {
  state_->CheckThread();
  const auto id = ++state_->next_subscription;
  state_->progress_handlers.emplace(id, std::move(handler));
  std::weak_ptr<detail::PlayerState> weak = state_;
  state_->post([weak, id] {
    if (auto state = weak.lock(); state && state->native) {
      if (auto found = state->progress_handlers.find(id); found != state->progress_handlers.end()) {
        auto current = found->second;
        const auto progress = state->value.progress;
        current(progress);
      }
    }
  });
  return [weak, id] {
    if (auto state = weak.lock())
      state->progress_handlers.erase(id);
  };
}

std::function<void()> MediaPlayer::ConnectEnded(std::function<void()> handler) const {
  const auto id = ++state_->next_subscription;
  state_->ended_handlers.emplace(id, std::move(handler));
  std::weak_ptr<detail::PlayerState> weak = state_;
  return [weak, id] {
    if (auto state = weak.lock())
      state->ended_handlers.erase(id);
  };
}

std::function<void()> MediaPlayer::ConnectError(std::function<void(const MediaError&)> handler) const {
  const auto id = ++state_->next_subscription;
  state_->error_handlers.emplace(id, std::move(handler));
  std::weak_ptr<detail::PlayerState> weak = state_;
  return [weak, id] {
    if (auto state = weak.lock())
      state->error_handlers.erase(id);
  };
}

MediaPlayer UseMediaPlayer() {
  auto tasks = UseTaskScope();
  auto observed = UseState(MediaPlayerState{});
  auto texture = UseState(std::shared_ptr<ExternalTexture>{});
  auto retained = UseState(std::make_shared<detail::PlayerState>(
      [tasks](std::function<void()> callback) { tasks.Post(std::move(callback)); }));
  auto state = retained.Get();
  state->observed = observed;
  state->observed_texture = texture;
  Lifecycle(
      [state, tasks] {
        state->Attach(OpenPlatformModule<std::shared_ptr<detail::NativePlayer>>(detail::player_type));
        auto polling = tasks.Launch([weak = std::weak_ptr(state)]() -> Task<void> {
          for (;;) {
            co_await Delay(std::chrono::milliseconds(250));
            auto current = weak.lock();
            if (!current || !current->native)
              co_return;
            current->Poll();
          }
        });
        return [state, polling] {
          polling.Cancel();
          state->Detach();
        };
      },
      state);
  return detail::PlayerAccess::Create(std::move(state));
}

void Install(ApplicationContext& context) {
  detail::InstallPlatform(context);
  detail::InstallMediaLibrary(context);
}

#if !defined(__linux__) || defined(__ANDROID__)
View VideoSurface(MediaPlayer player, VideoSurfaceProperties properties) {
  detail::ValidateVideoProperties(properties);
#if defined(__ANDROID__)
  return PlatformView(detail::video_type, detail::EncodedVideoProperties{properties}).Controller(std::move(player));
#else
  return PlatformView(detail::video_type, properties).Controller(std::move(player));
#endif
}
#endif

} // namespace huxerui::media
