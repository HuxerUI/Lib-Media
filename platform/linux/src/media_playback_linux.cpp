#include "decoded_playback/device.h"

#include <pulse/pulseaudio.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <utility>

#define HUXERUI_MEDIA_DISCARD(expression) static_cast<void>(expression)

namespace huxerui::media::detail {
namespace {
using Clock = std::chrono::steady_clock;

class MainloopLock {
public:
  explicit MainloopLock(pa_threaded_mainloop* loop) : loop_(loop) { pa_threaded_mainloop_lock(loop_); }
  ~MainloopLock() { pa_threaded_mainloop_unlock(loop_); }
private:
  pa_threaded_mainloop* loop_;
};

template<class T, class Function> PlaybackResult<T> Guard(Function function) {
  try { return PlaybackResult<T>::Success(function()); }
  catch (PlaybackError& error) { return PlaybackResult<T>::Failure(std::move(error)); }
}

class PulsePlaybackDevice final : public PlaybackDevice {
public:
  ~PulsePlaybackDevice() override { try { HUXERUI_MEDIA_DISCARD(Close()); } catch (...) {} }

  PlaybackResult<bool> Open(const AudioFormat& format, std::size_t capacity, std::function<void()> output_lost) override {
    return Guard<bool>([&] {
      if (!loop_) {
        spec_ = {PA_SAMPLE_S16NE, format.sample_rate, std::uint8_t(format.ChannelCount())};
        if (!pa_sample_spec_valid(&spec_) || capacity > UINT32_MAX / pa_frame_size(&spec_))
          throw PlaybackError{PlaybackErrorCode::Unsupported, "The PCM format exceeds PulseAudio limits", {}};
        capacity_ = std::uint32_t(capacity * pa_frame_size(&spec_));
        wakeup_ = std::move(output_lost);
        loop_ = pa_threaded_mainloop_new();
        if (!loop_) throw PlaybackError{PlaybackErrorCode::ResourceExhausted, "Cannot allocate the PulseAudio mainloop", {}};
      }
      MainloopLock lock(loop_);
      if (!context_) {
        context_ = pa_context_new(pa_threaded_mainloop_get_api(loop_), "HuxerUI Media");
        if (!context_) throw PlaybackError{PlaybackErrorCode::ResourceExhausted, "Cannot allocate a PulseAudio context", {}};
        pa_context_set_state_callback(context_, ContextChanged, this);
        Require(pa_context_connect(context_, nullptr, PA_CONTEXT_NOAUTOSPAWN, nullptr), "Cannot connect to PulseAudio",
                PlaybackErrorCode::Unsupported);
        Require(pa_threaded_mainloop_start(loop_), "Cannot start the PulseAudio mainloop");
        loop_started_ = true;
      }
      const auto context_state = pa_context_get_state(context_);
      if (!PA_CONTEXT_IS_GOOD(context_state)) Fail("The PulseAudio connection failed", PlaybackErrorCode::Unsupported);
      if (context_state != PA_CONTEXT_READY) return false;
      if (!stream_) {
        stream_ = pa_stream_new(context_, "Decoded playback", &spec_, nullptr);
        if (!stream_) Fail("Cannot allocate a PulseAudio stream", PlaybackErrorCode::ResourceExhausted);
        pa_stream_set_state_callback(stream_, StreamChanged, this);
        pa_stream_set_underflow_callback(stream_, Underflow, this);
        pa_stream_set_started_callback(stream_, Started, this);
        pa_stream_set_moved_callback(stream_, Moved, this);
        const auto frame = std::uint32_t(pa_frame_size(&spec_));
        const auto target = std::min(capacity_, std::max(frame, spec_.rate / 10 * frame));
        // Positive prebuffering stops the read index from overtaking media during an underrun.
        const pa_buffer_attr buffer{capacity_, target, frame, std::max(frame, target / 4 / frame * frame), UINT32_MAX};
        const auto flags = pa_stream_flags_t(PA_STREAM_START_CORKED | PA_STREAM_ADJUST_LATENCY | PA_STREAM_DONT_MOVE);
        Require(pa_stream_connect_playback(stream_, nullptr, &buffer, flags, nullptr, nullptr),
                "PulseAudio rejected the PCM output", PlaybackErrorCode::Unsupported);
      }
      Check();
      return pa_stream_get_state(stream_) == PA_STREAM_READY;
    });
  }

  PlaybackResult<std::size_t> Write(std::span<const std::int16_t> samples) override {
    return Guard<std::size_t>([&] {
      MainloopLock lock(loop_);
      Check();
      if (underflow_) return std::size_t(0);
      const auto available = pa_stream_writable_size(stream_);
      if (available == std::size_t(-1)) Fail("Cannot query PulseAudio write capacity");
      const auto frame = pa_frame_size(&spec_);
      const auto frames = std::min({samples.size() / spec_.channels, available / frame, std::size_t(capacity_) / frame});
      if (!frames) return std::size_t(0);
      if (written_ > std::uint64_t(INT64_MAX) / frame - frames) Fail("PulseAudio media index overflow");
      Require(pa_stream_write(stream_, samples.data(), frames * frame, nullptr, 0, PA_SEEK_RELATIVE),
              "Cannot submit PulseAudio PCM");
      written_ += frames;
      return frames;
    });
  }

  PlaybackResult<bool> Start() override {
    return Guard<bool>([&] {
      MainloopLock lock(loop_);
      Check();
      if (!Cork(false)) return false;
      if (!triggered_) {
        if (!trigger_.handle) Begin(trigger_, pa_stream_trigger(stream_, Completed, &trigger_));
        if (!Complete(trigger_, "Cannot trigger PulseAudio playback")) return false;
        triggered_ = true;
      }
      if (!running_) progressed_ = Clock::now();
      running_ = true;
      return true;
    });
  }

  PlaybackResult<bool> Pause() override {
    return Guard<bool>([&] {
      MainloopLock lock(loop_);
      Check();
      Cancel(trigger_);
      if (!Cork(true)) return false;
      running_ = triggered_ = false;
      return true;
    });
  }

  PlaybackResult<PlaybackProgress> Observe(bool) override {
    return Guard<PlaybackProgress>([&] {
      MainloopLock lock(loop_);
      Check();
      if (!running_) return PlaybackProgress{presented_, !underflow_};
      if (timing_.handle && timing_.result >= 0) {
        if (Complete(timing_, "Cannot update PulseAudio timing")) {
          const auto* info = pa_stream_get_timing_info(stream_);
          pa_usec_t time = 0;
          if (info && !info->read_index_corrupt && !info->write_index_corrupt && pa_stream_get_time(stream_, &time) == 0) {
            if (time < clock_) Fail("The PulseAudio playback clock regressed");
            clock_ = time;
            const auto whole = time / PA_USEC_PER_SEC;
            if (whole > UINT64_MAX / spec_.rate) Fail("The PulseAudio playback clock overflowed");
            auto count = std::min(written_, whole * spec_.rate + time % PA_USEC_PER_SEC * spec_.rate / PA_USEC_PER_SEC);
            const auto tail = pa_bytes_to_usec(written_ * pa_frame_size(&spec_), &spec_);
            // PulseAudio times use whole microseconds; drain and the read index confirm the fractional final sample.
            if (drained_ && info->read_index >= 0 && std::uint64_t(info->read_index) >= written_ * pa_frame_size(&spec_) &&
                time >= tail) count = written_;
            if (count > presented_) {
              presented_ = count;
              progressed_ = Clock::now();
            }
          }
        }
      }
      if (!timing_.handle) Begin(timing_, pa_stream_update_timing_info(stream_, Completed, &timing_));
      if (written_ && presented_ == written_) underflow_ = true;
      if (underflow_ && presented_ < written_) PadTail();
      if (underflow_ && !drained_) {
        if (!drain_.handle) Begin(drain_, pa_stream_drain(stream_, Completed, &drain_));
        drained_ = Complete(drain_, "Cannot drain PulseAudio output");
      }
      if (presented_ < written_ && Clock::now() - progressed_ >= std::chrono::seconds(5))
        Fail("PulseAudio stopped confirming PCM presentation");
      return PlaybackProgress{presented_, !underflow_};
    });
  }

  PlaybackResult<bool> Close() override {
    if (!loop_) return PlaybackResult<bool>::Success(true);
    int error = 0;
    {
      MainloopLock lock(loop_);
      closing_ = true;
      Cancel(control_); Cancel(trigger_); Cancel(timing_); Cancel(drain_);
      if (stream_) {
        pa_stream_set_state_callback(stream_, nullptr, nullptr);
        pa_stream_set_underflow_callback(stream_, nullptr, nullptr);
        pa_stream_set_started_callback(stream_, nullptr, nullptr);
        pa_stream_set_moved_callback(stream_, nullptr, nullptr);
        if (pa_stream_get_state(stream_) == PA_STREAM_READY && pa_stream_disconnect(stream_) < 0)
          error = pa_context_errno(context_);
        pa_stream_unref(std::exchange(stream_, nullptr));
      }
      if (context_) {
        pa_context_set_state_callback(context_, nullptr, nullptr);
        pa_context_disconnect(context_);
        pa_context_unref(std::exchange(context_, nullptr));
      }
    }
    if (loop_started_) pa_threaded_mainloop_stop(loop_);
    pa_threaded_mainloop_free(std::exchange(loop_, nullptr));
    wakeup_ = {};
    if (error) return PlaybackFailure<bool>(PlaybackErrorCode::DeviceFailure, "Cannot disconnect PulseAudio output",
                                          pa_strerror(error));
    return PlaybackResult<bool>::Success(true);
  }

private:
  struct Operation {
    pa_operation* handle = nullptr;
    PulsePlaybackDevice* owner = nullptr;
    int result = -1;
  };
  void Signal() noexcept { if (wakeup_) { try { wakeup_(); } catch (...) {} } }
  static void ContextChanged(pa_context*, void* data) { static_cast<PulsePlaybackDevice*>(data)->Signal(); }
  static void StreamChanged(pa_stream*, void* data) { static_cast<PulsePlaybackDevice*>(data)->Signal(); }
  static void Started(pa_stream*, void* data) { static_cast<PulsePlaybackDevice*>(data)->started_ = true; }
  static void Underflow(pa_stream*, void* data) {
    auto& self = *static_cast<PulsePlaybackDevice*>(data);
    if (self.started_) { self.underflow_ = true; self.Signal(); }
  }
  static void Moved(pa_stream*, void* data) {
    auto& self = *static_cast<PulsePlaybackDevice*>(data);
    self.moved_ = true;
    self.Signal();
  }
  static void Completed(pa_stream*, int success, void* data) {
    auto& operation = *static_cast<Operation*>(data);
    operation.result = success;
    operation.owner->Signal();
  }
  [[noreturn]] void Fail(const char* message, PlaybackErrorCode code = PlaybackErrorCode::DeviceFailure) const {
    throw PlaybackError{code, message, context_ ? pa_strerror(pa_context_errno(context_)) : std::string{}};
  }
  void Require(int result, const char* message, PlaybackErrorCode code = PlaybackErrorCode::DeviceFailure) const {
    if (result < 0) Fail(message, code);
  }
  void Check() const {
    if (closing_ || moved_ || !PA_CONTEXT_IS_GOOD(pa_context_get_state(context_)) ||
        (stream_ && !PA_STREAM_IS_GOOD(pa_stream_get_state(stream_)))) Fail("The PulseAudio output route is unavailable");
  }
  static void Cancel(Operation& operation) {
    if (auto* handle = std::exchange(operation.handle, nullptr)) {
      pa_operation_cancel(handle);
      pa_operation_unref(handle);
    }
    operation.result = -1;
  }
  void Begin(Operation& operation, pa_operation* handle) {
    if (!handle) Fail("Cannot start a PulseAudio operation");
    operation.handle = handle;
    operation.owner = this;
    operation.result = -1;
  }
  bool Complete(Operation& operation, const char* message) {
    if (operation.result < 0) return false;
    const auto success = operation.result;
    Cancel(operation);
    if (!success) Fail(message);
    return true;
  }
  bool Cork(bool value) {
    bool superseded = false;
    if (control_.handle && requested_cork_ != value) { Cancel(control_); superseded = true; }
    if (!control_.handle) {
      if (corked_ == value && !superseded) return true;
      requested_cork_ = value;
      Begin(control_, pa_stream_cork(stream_, value, Completed, &control_));
    }
    if (!Complete(control_, "Cannot change PulseAudio cork state")) return false;
    corked_ = value;
    return true;
  }
  void PadTail() {
    const auto available = pa_stream_writable_size(stream_);
    if (available == std::size_t(-1)) Fail("Cannot query PulseAudio tail capacity");
    const auto frame = pa_frame_size(&spec_);
    const auto limit = std::max(std::uint64_t(1), std::uint64_t(spec_.rate) / 2);
    const auto frames = std::min({std::uint64_t(available / frame), limit - padded_, std::uint64_t(1024)});
    if (!frames) return;
    if (written_ > std::uint64_t(INT64_MAX) / frame - padded_ - frames) Fail("PulseAudio media index overflow");
    // Keep PulseAudio timing observable across sink latency without extending the media timeline.
    const std::array<std::int16_t, 2048> silence{};
    Require(pa_stream_write(stream_, silence.data(), frames * frame, nullptr, 0, PA_SEEK_RELATIVE),
            "Cannot submit PulseAudio tail padding");
    padded_ += frames;
  }

  pa_threaded_mainloop* loop_ = nullptr;
  pa_context* context_ = nullptr;
  pa_stream* stream_ = nullptr;
  pa_sample_spec spec_{};
  std::uint32_t capacity_ = 0;
  std::uint64_t written_ = 0, presented_ = 0, padded_ = 0;
  pa_usec_t clock_ = 0;
  Operation control_, trigger_, timing_, drain_;
  bool loop_started_ = false, closing_ = false, corked_ = true, requested_cork_ = true;
  bool running_ = false, started_ = false, triggered_ = false, underflow_ = false, drained_ = false, moved_ = false;
  Clock::time_point progressed_{};
  std::function<void()> wakeup_;
};
} // namespace

std::unique_ptr<PlaybackDevice> MakePlaybackDevice() { return std::make_unique<PulsePlaybackDevice>(); }
} // namespace huxerui::media::detail

#undef HUXERUI_MEDIA_DISCARD
