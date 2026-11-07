#pragma once

#include "backend.h"

#include <map>
#include <thread>
#include <huxerui/state.h>

namespace huxerui::media::detail {

class PlayerState : public std::enable_shared_from_this<PlayerState> {
public:
  using Post = std::function<void(std::function<void()>)>;
  explicit PlayerState(Post post);
  void CheckThread() const;
  void Attach(std::shared_ptr<PlayerBackend> value);
  void Detach() noexcept;
  void Receive(PlayerEvent event);
  void Publish(bool force_progress = false);
  void Report(MediaError error);
  void Poll();
  void Bind(VideoBinding binding);
  void Unbind();
  void StartSeek(MediaTime position);
  void CheckLoop();
  void ResetSource();
  void FinishEnded();

  MediaPlayerState value;
  State<MediaPlayerState> observed;
  State<std::shared_ptr<ExternalTexture>> observed_texture;
  std::shared_ptr<PlayerBackend> backend;
  std::optional<MediaSource> source;
  std::uint64_t generation = 0;
  std::uint64_t attachment = 0;
  std::uint64_t seek_serial = 0;
  std::uint64_t active_seek = 0;
  std::optional<MediaTime> pending_seek;
  bool loop_reported = false;
  bool loop_restart = false;
  std::uint64_t next_subscription = 0;
  std::map<std::uint64_t, std::function<void(const MediaProgress&)>> progress_handlers;
  std::map<std::uint64_t, std::function<void()>> ended_handlers;
  std::map<std::uint64_t, std::function<void(const MediaError&)>> error_handlers;
  Post post;

private:
  void NotifyProgress();
  std::uint64_t progress_notification_ = 0;
  std::thread::id owner_;
  MediaProgress published_progress_;
  VideoBinding video_binding_;
};

struct PlayerAccess {
  static const auto& Source(const MediaSource& source) {
    return source.value_;
  }
  static const auto& State(const MediaPlayer& player) {
    return player.state_;
  }
  static MediaPlayer Create(std::shared_ptr<PlayerState> state) {
    return MediaPlayer(std::move(state));
  }
  static void Bind(const MediaPlayer& player, VideoBinding binding) {
    player.state_->Bind(std::move(binding));
  }
  static void Unbind(const MediaPlayer& player) {
    player.state_->Unbind();
  }
};

} // namespace huxerui::media::detail
