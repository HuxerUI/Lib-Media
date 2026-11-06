#include "playback/device.h"

#include <aaudio/AAudio.h>
#include <dlfcn.h>

#include <algorithm>
#include <chrono>

namespace huxerui::media::detail {
namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

std::int64_t MonotonicNanoseconds() {
  timespec time{};
  clock_gettime(CLOCK_MONOTONIC, &time);
  return time.tv_sec * 1'000'000'000LL + time.tv_nsec;
}

class AndroidPlaybackDevice final : public PlaybackDevice {
public:
  PlaybackResult<void> Open(const AudioFormat& format, std::size_t capacity) override {
    // Keep the platform library loaded while any native service threads can still use it.
    static void* library = dlopen("libaaudio.so", RTLD_NOW | RTLD_LOCAL);
    library_ = library;
    if (!library_ || !Load())
      return PlaybackFailure(PlaybackErrorCode::Unsupported, "AAudio PCM playback requires Android API 26 or later");

    AAudioStreamBuilder* builder = nullptr;
    const auto created = create_builder_(&builder);
    if (created < 0) return Error(created, "Cannot create an AAudio stream builder");
    set_direction_(builder, AAUDIO_DIRECTION_OUTPUT);
    set_sharing_(builder, AAUDIO_SHARING_MODE_SHARED);
    set_performance_(builder, AAUDIO_PERFORMANCE_MODE_NONE);
    set_format_(builder, AAUDIO_FORMAT_PCM_I16);
    set_rate_(builder, std::int32_t(format.sample_rate));
    set_channels_(builder, std::int32_t(format.ChannelCount()));
    const auto requested_capacity = std::min(capacity, std::max<std::size_t>(1, format.sample_rate / 5));
    set_capacity_(builder, std::int32_t(requested_capacity));
    const auto opened = open_stream_(builder, &stream_);
    delete_builder_(builder);
    if (opened < 0) return Error(opened, "Cannot open AAudio output");
    channels_ = format.ChannelCount();
    if (rate_(stream_) != std::int32_t(format.sample_rate) || format_(stream_) != AAUDIO_FORMAT_PCM_I16 ||
        channels_ != std::size_t(channel_count_(stream_)))
      return PlaybackFailure(PlaybackErrorCode::Unsupported, "AAudio did not open the requested PCM format");
    const auto device_capacity = capacity_(stream_);
    const auto burst = burst_(stream_);
    if (device_capacity <= 0 || burst <= 0 || std::size_t(burst) > capacity)
      return PlaybackFailure(PlaybackErrorCode::Unsupported, "AAudio priming exceeds the playback queue bounds");
    quantum_ = std::min<std::size_t>(burst, capacity);
    native_capacity_ = std::min<std::size_t>(device_capacity, capacity);
    padding_limit_ = std::uint64_t(native_capacity_) * 3;
    silence_.resize(std::min(quantum_, native_capacity_) * channels_);
    return PlaybackResult<void>::Success();
  }

  PlaybackResult<std::size_t> Write(std::span<const std::int16_t> samples) override {
    auto result = WriteNative(samples);
    if (result.Succeeded()) media_written_ += result.Value();
    return result;
  }
  PlaybackResult<void> Start() override {
    epoch_time_ = MonotonicNanoseconds();
    last_progress_ = Clock::now();
    const auto status = start_(stream_);
    if (status < 0) return Error(status, "Cannot start AAudio output");
    return PlaybackResult<void>::Success();
  }

  PlaybackResult<void> Pause() override {
    const auto status = pause_(stream_);
    if (status < 0) return Error(status, "Cannot pause AAudio output");
    const auto deadline = Clock::now() + 500ms;
    auto state = state_(stream_);
    while (state != AAUDIO_STREAM_STATE_PAUSED && Clock::now() < deadline) {
      aaudio_stream_state_t next = state;
      const auto waited = wait_state_(stream_, state, &next, 10'000'000);
      if (waited != AAUDIO_ERROR_TIMEOUT && waited < 0) return Error(waited, "Cannot observe AAudio pause");
      state = next;
    }
    if (state != AAUDIO_STREAM_STATE_PAUSED)
      return PlaybackFailure(PlaybackErrorCode::DeviceFailure, "AAudio did not acknowledge pause");
    return PlaybackResult<void>::Success();
  }

  PlaybackResult<PlaybackProgress> Observe(bool input_empty) override {
    using R = PlaybackResult<PlaybackProgress>;
    aaudio_stream_state_t state = AAUDIO_STREAM_STATE_UNKNOWN;
    const auto observed = wait_state_(stream_, state, &state, 0);
    if (observed < 0 && observed != AAUDIO_ERROR_TIMEOUT)
      return R::Failure(Error(observed, "Cannot query AAudio state").Error());
    if (state == AAUDIO_STREAM_STATE_DISCONNECTED)
      return PlaybackFailure<PlaybackProgress>(PlaybackErrorCode::DeviceFailure, "AAudio output disconnected");

    if (state == AAUDIO_STREAM_STATE_STARTED) {
      std::int64_t position = 0, time = 0;
      const auto result = timestamp_(stream_, CLOCK_MONOTONIC, &position, &time);
      if (result != AAUDIO_ERROR_INVALID_STATE && result != AAUDIO_ERROR_UNAVAILABLE && result < 0)
        return R::Failure(Error(result, "Cannot read AAudio timestamp").Error());
      const auto now = MonotonicNanoseconds();
      if (result == AAUDIO_OK && position >= 0 && std::uint64_t(position) <= native_written_ &&
          time >= epoch_time_ && time <= now && now - time <= 250'000'000) {
        const auto presented = std::min(std::uint64_t(position), media_written_);
        if (presented > presented_) {
          presented_ = presented;
          last_progress_ = Clock::now();
        }
      }
    }
    if (presented_ < media_written_ && Clock::now() - last_progress_ > 2s)
      return PlaybackFailure<PlaybackProgress>(PlaybackErrorCode::DeviceFailure, "AAudio presentation clock stalled");

    if ((input_empty || padded_) && presented_ < media_written_ && padded_ < padding_limit_) {
      const auto consumed = std::uint64_t(std::max<std::int64_t>(0, read_(stream_)));
      const auto queued = native_written_ - std::min(consumed, native_written_);
      const auto low_watermark = std::max<std::size_t>(1, quantum_ / 4);
      // Some legacy tracks report STARTED while still waiting for a full initial buffer.
      const bool priming = !consumed && !presented_;
      if (queued <= low_watermark || state == AAUDIO_STREAM_STATE_STARTING || priming || padded_) {
        const auto room = native_capacity_ - std::min<std::uint64_t>(queued, native_capacity_);
        const auto frames = std::min({std::uint64_t(silence_.size() / channels_), padding_limit_ - padded_, room});
        if (frames) {
          auto result = WriteNative(std::span(silence_).first(std::size_t(frames) * channels_));
          if (!result.Succeeded()) return R::Failure(std::move(result).Error());
          padded_ += result.Value();
        }
      }
    }
    return R::Success({presented_, padded_ == 0});
  }

  PlaybackResult<void> Close() override {
    auto result = PlaybackResult<void>::Success();
    if (stream_) {
      const auto closed = close_(stream_);
      if (closed < 0) result = Error(closed, "Cannot close AAudio output");
      stream_ = nullptr;
    }
    return result;
  }

private:
  PlaybackResult<std::size_t> WriteNative(std::span<const std::int16_t> samples) {
    const auto frames = std::int32_t(samples.size() / channels_);
    const auto result = write_(stream_, samples.data(), frames, 0);
    if (result < 0)
      return PlaybackResult<std::size_t>::Failure(Error(result, "AAudio write failed").Error());
    native_written_ += std::uint64_t(result);
    if (written_(stream_) != std::int64_t(native_written_))
      return PlaybackFailure<std::size_t>(PlaybackErrorCode::DeviceFailure, "AAudio changed the native write origin");
    return PlaybackResult<std::size_t>::Success(std::size_t(result));
  }
  static PlaybackResult<void> Error(aaudio_result_t status, const char* message) {
    return PlaybackFailure(PlaybackErrorCode::DeviceFailure, message, std::to_string(status));
  }

  template<class Function> bool Load(Function& function, const char* name) {
    function = reinterpret_cast<Function>(dlsym(library_, name));
    return function != nullptr;
  }

  bool Load() {
    return Load(create_builder_, "AAudio_createStreamBuilder") &&
           Load(delete_builder_, "AAudioStreamBuilder_delete") &&
           Load(set_direction_, "AAudioStreamBuilder_setDirection") &&
           Load(set_sharing_, "AAudioStreamBuilder_setSharingMode") &&
           Load(set_performance_, "AAudioStreamBuilder_setPerformanceMode") &&
           Load(set_format_, "AAudioStreamBuilder_setFormat") &&
           Load(set_rate_, "AAudioStreamBuilder_setSampleRate") &&
           Load(set_channels_, "AAudioStreamBuilder_setChannelCount") &&
           Load(set_capacity_, "AAudioStreamBuilder_setBufferCapacityInFrames") &&
           Load(open_stream_, "AAudioStreamBuilder_openStream") &&
           Load(rate_, "AAudioStream_getSampleRate") &&
           Load(format_, "AAudioStream_getFormat") &&
           Load(channel_count_, "AAudioStream_getChannelCount") &&
           Load(capacity_, "AAudioStream_getBufferCapacityInFrames") &&
           Load(burst_, "AAudioStream_getFramesPerBurst") &&
           Load(write_, "AAudioStream_write") &&
           Load(written_, "AAudioStream_getFramesWritten") &&
           Load(read_, "AAudioStream_getFramesRead") &&
           Load(start_, "AAudioStream_requestStart") &&
           Load(pause_, "AAudioStream_requestPause") &&
           Load(state_, "AAudioStream_getState") &&
           Load(wait_state_, "AAudioStream_waitForStateChange") &&
           Load(timestamp_, "AAudioStream_getTimestamp") &&
           Load(close_, "AAudioStream_close");
  }

  void* library_ = nullptr;
  AAudioStream* stream_ = nullptr;
  std::size_t channels_ = 0, quantum_ = 0, native_capacity_ = 0;
  std::uint64_t media_written_ = 0, native_written_ = 0, presented_ = 0, padded_ = 0, padding_limit_ = 0;
  std::int64_t epoch_time_ = 0;
  Clock::time_point last_progress_;
  std::vector<std::int16_t> silence_;
  // Use the API 26 signatures without referencing symbols at the library's API 23 link baseline.
  aaudio_result_t (*create_builder_)(AAudioStreamBuilder**);
  aaudio_result_t (*delete_builder_)(AAudioStreamBuilder*);
  void (*set_direction_)(AAudioStreamBuilder*, aaudio_direction_t);
  void (*set_sharing_)(AAudioStreamBuilder*, aaudio_sharing_mode_t);
  void (*set_performance_)(AAudioStreamBuilder*, aaudio_performance_mode_t);
  void (*set_format_)(AAudioStreamBuilder*, aaudio_format_t);
  void (*set_rate_)(AAudioStreamBuilder*, std::int32_t);
  void (*set_channels_)(AAudioStreamBuilder*, std::int32_t);
  void (*set_capacity_)(AAudioStreamBuilder*, std::int32_t);
  aaudio_result_t (*open_stream_)(AAudioStreamBuilder*, AAudioStream**);
  std::int32_t (*rate_)(AAudioStream*);
  aaudio_format_t (*format_)(AAudioStream*);
  std::int32_t (*channel_count_)(AAudioStream*);
  std::int32_t (*capacity_)(AAudioStream*);
  std::int32_t (*burst_)(AAudioStream*);
  aaudio_result_t (*write_)(AAudioStream*, const void*, std::int32_t, std::int64_t);
  std::int64_t (*written_)(AAudioStream*);
  std::int64_t (*read_)(AAudioStream*);
  aaudio_result_t (*start_)(AAudioStream*);
  aaudio_result_t (*pause_)(AAudioStream*);
  aaudio_stream_state_t (*state_)(AAudioStream*);
  aaudio_result_t (*wait_state_)(AAudioStream*, aaudio_stream_state_t, aaudio_stream_state_t*, std::int64_t);
  aaudio_result_t (*timestamp_)(AAudioStream*, clockid_t, std::int64_t*, std::int64_t*);
  aaudio_result_t (*close_)(AAudioStream*);
};
} // namespace

std::unique_ptr<PlaybackDevice> MakePlaybackDevice() { return std::make_unique<AndroidPlaybackDevice>(); }
} // namespace huxerui::media::detail
