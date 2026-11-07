#pragma once

#include <huxerui/media_player.h>
#include <huxerui/external_texture.h>
#include <huxerui/platform_registry.h>

namespace huxerui::media::detail {

inline constexpr char player_type[] = "huxerui/media/player";
inline constexpr char video_type[] = "huxerui/media/video";

struct PlayerSnapshot {
  MediaProgress progress;
  MediaSeekability seekability = MediaSeekability::Unknown;
  std::optional<bool> has_audio;
  std::optional<bool> has_video;
  std::optional<Size> video_size;
  bool buffering = false;
};

enum class PlayerEventKind { Update, Ready, Playing, Paused, Seeked, Ended, Interrupted, Error };

struct PlayerEvent {
  std::uint64_t generation = 0;
  PlayerEventKind kind = PlayerEventKind::Update;
  PlayerSnapshot snapshot;
  std::uint64_t seek_id = 0;
  MediaError error;
};

/// Implementation-only boundary. Every adapter stops using a source before releasing its retained capability.
class PlayerBackend {
public:
  virtual ~PlayerBackend() = default;
  std::function<void(PlayerEvent)> emit;
  virtual void Load(const MediaSource& source, std::uint64_t generation) = 0;
  virtual void Clear() noexcept = 0;
  virtual void Play() = 0;
  virtual void Pause() = 0;
  virtual void Seek(MediaTime position, std::uint64_t seek_id) = 0;
  virtual void SetVolume(double volume, bool muted) = 0;
  virtual void Poll() = 0;
  virtual void BindVideo(PlatformChannel) {}
  virtual void UnbindVideo() {}
  virtual std::shared_ptr<ExternalTexture> Texture() {
    return {};
  }
};

using VideoBinding = std::function<void(const std::shared_ptr<PlayerBackend>&)>;

void InstallPlatform(ApplicationContext& context);

} // namespace huxerui::media::detail
