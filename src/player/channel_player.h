#pragma once

#include "backend.h"

namespace huxerui::media::detail {

/// Payload transport shared only by the Java and JavaScript implementations.
class ChannelPlayer : public PlayerBackend, public std::enable_shared_from_this<ChannelPlayer> {
public:
  explicit ChannelPlayer(PlatformChannel channel);
  ~ChannelPlayer() override;
  void Connect();
  void Load(const MediaSource& source, std::uint64_t generation) override;
  void Clear() noexcept override;
  void Play() override;
  void Pause() override;
  void Seek(MediaTime position, std::uint64_t seek_id) override;
  void SetVolume(double volume, bool muted) override;
  void Poll() override;
  void BindVideo(PlatformChannel video) override;
  void UnbindVideo() override;

protected:
  void Invoke(std::string method, PlatformPayload::Object arguments = {});
  PlatformChannel channel_;
  std::uint64_t generation_ = 0;

private:
  void Receive(const PlatformPayload& payload);
  PlatformChannel video_;
  std::uint64_t identity_ = 0;
};

struct EncodedVideoProperties {
  VideoSurfaceProperties value;
  bool operator==(const EncodedVideoProperties&) const = default;
  static PlatformPayload Encode(const EncodedVideoProperties& properties);
};

} // namespace huxerui::media::detail
