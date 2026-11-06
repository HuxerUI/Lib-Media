#pragma once

#include <huxerui/media_playback.h>
#include <span>

namespace huxerui::media::detail {

struct PlaybackProgress {
  std::uint64_t presented = 0;
  // False when this device run must retire after its committed tail.
  bool accepts_media = true;
};

// A worker or event loop owns the device. Frame counts are relative to the current run.
// Control operations return false while awaiting acknowledgement and may be polled with the same arguments.
class PlaybackDevice {
public:
  virtual ~PlaybackDevice() = default;
  // Signal output loss from native callbacks. The worker then checks the device through Observe().
  virtual PlaybackResult<bool> Open(const AudioFormat& format, std::size_t capacity,
                                   std::function<void()> output_lost) = 0;
  virtual PlaybackResult<std::size_t> Write(std::span<const std::int16_t> samples) = 0;
  virtual PlaybackResult<bool> Start() = 0;
  // Pause can supersede a pending Start without discarding submitted PCM.
  virtual PlaybackResult<bool> Pause() = 0;
  virtual PlaybackResult<PlaybackProgress> Observe(bool input_empty) = 0;
  virtual PlaybackResult<bool> Close() = 0;
};

std::unique_ptr<PlaybackDevice> MakePlaybackDevice();

template<class T = void>
PlaybackResult<T> PlaybackFailure(PlaybackErrorCode code, std::string message, std::string platform = {}) {
  return PlaybackResult<T>::Failure({code, std::move(message), std::move(platform)});
}
} // namespace huxerui::media::detail
