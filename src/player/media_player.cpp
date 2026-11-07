#include <huxerui/media_player.h>

#include "player/state.h"
#include "player/video_geometry.h"
#include "library/backend.h"
#if defined(__ANDROID__)
#include "player/channel_player.h"
#endif

#include <algorithm>
#include <cmath>
#include <cctype>
#include <huxerui/task.h>

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
  return state_ && static_cast<bool>(state_->backend);
}

MediaPlayerState MediaPlayer::Snapshot() const {
  state_->CheckThread();
  if (!state_->observed.IsValid()) return state_->value;
  MediaPlayerState snapshot = state_->observed;
  return snapshot;
}

bool MediaPlayer::Load(MediaSource source) const {
  state_->CheckThread();
  if (!IsConnected())
    return false;
  state_->ResetSource();
  state_->source = std::move(source);
  state_->value.status = MediaPlayerStatus::Loading;
  state_->Publish(true);
  state_->backend->Load(*state_->source, state_->generation);
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
    state_->backend->Play();
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
    state_->backend->Pause();
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
  state_->backend->SetVolume(volume, state_->value.muted);
  state_->Publish();
  return true;
}

bool MediaPlayer::SetMuted(bool muted) const {
  state_->CheckThread();
  if (!IsConnected())
    return false;
  state_->value.muted = muted;
  state_->backend->SetVolume(state_->value.volume, muted);
  state_->Publish();
  return true;
}

std::function<void()> MediaPlayer::ConnectProgress(std::function<void(const MediaProgress&)> handler) const {
  state_->CheckThread();
  const auto id = ++state_->next_subscription;
  state_->progress_handlers.emplace(id, std::move(handler));
  std::weak_ptr<detail::PlayerState> weak = state_;
  state_->post([weak, id] {
    if (auto state = weak.lock(); state && state->backend) {
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
  std::shared_ptr<detail::PlayerState> state = retained;
  state->observed = observed;
  state->observed_texture = texture;
  Lifecycle(
      [state, tasks] {
        state->Attach(OpenPlatformModule<std::shared_ptr<detail::PlayerBackend>>(detail::player_type));
        auto polling = tasks.Launch([weak = std::weak_ptr(state)]() -> Task<void> {
          for (;;) {
            co_await Delay(std::chrono::milliseconds(250));
            auto current = weak.lock();
            if (!current || !current->backend)
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
