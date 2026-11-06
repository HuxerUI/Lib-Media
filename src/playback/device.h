#pragma once

#include <huxerui/media_playback.h>
#include <span>

namespace huxerui::media::detail {

struct PlaybackProgress {
  std::uint64_t presented = 0;
  // False when this device run must retire after its committed tail.
  bool accepts_media = true;
};

// One worker owns the device. Frame counts are relative to the current native run.
class PlaybackDevice {
public:
  virtual ~PlaybackDevice() = default;
  virtual PlaybackResult<void> Open(const AudioFormat& format, std::size_t capacity) = 0;
  virtual PlaybackResult<std::size_t> Write(std::span<const std::int16_t> samples) = 0;
  virtual PlaybackResult<void> Start() = 0;
  virtual PlaybackResult<void> Pause() = 0;
  virtual PlaybackResult<PlaybackProgress> Observe(bool input_empty) = 0;
  virtual PlaybackResult<void> Close() = 0;
};

std::unique_ptr<PlaybackDevice> MakePlaybackDevice();

template<class T = void>
PlaybackResult<T> PlaybackFailure(PlaybackErrorCode code, std::string message, std::string platform = {}) {
  return PlaybackResult<T>::Failure({code, std::move(message), std::move(platform)});
}
} // namespace huxerui::media::detail
