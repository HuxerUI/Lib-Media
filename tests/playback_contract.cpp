#include "playback/session.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <thread>

using namespace huxerui;
using namespace huxerui::media;
using namespace huxerui::media::detail;
using namespace std::chrono_literals;

namespace {
void Check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
template<class Test> void Wait(Test test, const char* message) {
  const auto deadline = std::chrono::steady_clock::now() + 3s;
  while (std::chrono::steady_clock::now() < deadline) {
    if (test()) return;
    std::this_thread::sleep_for(1ms);
  }
  throw std::runtime_error(message);
}

class Gate {
public:
  void Arm() {
    std::lock_guard lock(mutex_);
    entered = false;
    blocked_ = true;
  }
  void Enter() {
    std::unique_lock lock(mutex_);
    entered = true;
    ready_.wait(lock, [&] { return !blocked_; });
  }
  void Release() {
    std::lock_guard lock(mutex_);
    blocked_ = false;
    ready_.notify_all();
  }
  std::atomic_bool entered = false;
private:
  std::mutex mutex_;
  std::condition_variable ready_;
  bool blocked_ = false;
};

struct Control {
  Gate observe, pause, close;
  std::atomic_bool fail_pause = false;
  std::atomic_bool fail_close = false;
  std::atomic_int opens = 0;
  std::atomic_int cleanup = 0;
  std::atomic_int submitted_sample = 0;
};
class Device final : public PlaybackDevice {
public:
  explicit Device(std::shared_ptr<Control> control) : control_(std::move(control)) {}
  PlaybackResult<void> Open(const AudioFormat&, std::size_t, std::function<void()>) override {
    ++control_->opens;
    return PlaybackResult<void>::Success();
  }
  PlaybackResult<void> Start() override { return PlaybackResult<void>::Success(); }
  PlaybackResult<void> Pause() override {
    control_->pause.Enter();
    if (control_->fail_pause) return PlaybackFailure(PlaybackErrorCode::DeviceFailure, "Injected pause failure");
    return PlaybackResult<void>::Success();
  }
  PlaybackResult<std::size_t> Write(std::span<const std::int16_t> samples) override {
    control_->submitted_sample = samples.front();
    written_ += samples.size() / 2;
    return PlaybackResult<std::size_t>::Success(samples.size() / 2);
  }
  PlaybackResult<PlaybackProgress> Observe(bool) override {
    control_->observe.Enter();
    return PlaybackResult<PlaybackProgress>::Success({written_});
  }
  PlaybackResult<void> Close() override {
    control_->close.Enter();
    if (control_->fail_close) return PlaybackFailure(PlaybackErrorCode::DeviceFailure, "Injected release failure");
    return PlaybackResult<void>::Success();
  }
private:
  std::shared_ptr<Control> control_;
  std::uint64_t written_ = 0;
};

AudioPlaybackConfig Config(const std::shared_ptr<Control>& control) {
  AudioPlaybackConfig config;
  config.format = {48000, AudioSampleFormat::S16, AudioPacking::Interleaved, AudioChannelLayout::Stereo};
  config.cleanup_completed = [control](PlaybackResult<void> result) {
    control->cleanup += result.Succeeded() ? 1 : 10;
  };
  return config;
}
std::shared_ptr<PlaybackSession> Create(const std::shared_ptr<Control>& control) {
  auto result = PlaybackSession::Create(Config(control), [control] { return std::make_unique<Device>(control); });
  Check(result.Succeeded(), "Cannot create playback session");
  return std::move(result).Value();
}
struct Session {
  std::shared_ptr<Control> control = std::make_shared<Control>();
  std::shared_ptr<PlaybackSession> state = Create(control);
  ~Session() {
    state->Close();
    control->observe.Release();
    control->pause.Release();
    control->close.Release();
  }
};

AudioFrame Frame(AudioFormat format, std::int64_t pts, std::uint32_t count) {
  auto samples = std::make_shared<std::vector<std::int16_t>>(count * format.ChannelCount(), 1200);
  return {format, {pts, format.sample_rate}, count, {BufferReference(std::as_bytes(std::span(*samples)), samples)}};
}

void BackpressureStartsBelowRefill() {
  Session session;
  auto& playback = *session.state;
  const auto format = Config(session.control).format;
  Wait([&] { return playback.Snapshot().state == PlaybackState::Paused; }, "Opening did not finish");
  Check(playback.Submit(Frame(format, 0, 1024)).Value() == SubmitStatus::Accepted, "Initial PCM was rejected");
  Check(playback.RequestPlay(true).Succeeded(), "Play failed");
  Wait([&] { return playback.Snapshot().state == PlaybackState::Buffering; }, "Playback did not wait for refill");
  const auto large = Frame(format, 1024, 12000);
  Check(playback.Submit(large).Value() == SubmitStatus::WouldBlock, "The full-capacity frame should wait for space");
  Wait([&] { return playback.Submit(large).Value() == SubmitStatus::Accepted; },
       "Playback stayed below the refill threshold while the producer was blocked");
  Check(playback.Finish().Succeeded(), "Cannot seal input");
  Wait([&] { return playback.Snapshot().state == PlaybackState::Ended; }, "The admitted input did not drain");
  Check(playback.Snapshot().position->ticks == 13024, "Backpressure recovery lost PCM");
}

void RefillKeepsDevice() {
  Session session;
  auto& playback = *session.state;
  const auto format = Config(session.control).format;
  Wait([&] { return playback.Snapshot().state == PlaybackState::Paused; }, "Opening did not finish");
  Check(playback.Submit(Frame(format, 0, 12000)).Value() == SubmitStatus::Accepted, "Initial PCM was rejected");
  Check(playback.RequestPlay(true).Succeeded(), "Play failed");
  Wait([&] {
    const auto snapshot = playback.Snapshot();
    return snapshot.state == PlaybackState::Buffering && snapshot.position->ticks == 12000;
  }, "Device completion stalled with PCM still in the software queue");
  Check(playback.Snapshot().audio_underruns == 1 && session.control->opens == 1,
        "A completed batch was counted as starvation or a writable device was reopened");
  Check(playback.Submit(Frame(format, 12000, 1024)).Value() == SubmitStatus::Accepted && playback.Finish().Succeeded(),
        "Cannot submit a short final refill");
  Wait([&] { return playback.Snapshot().state == PlaybackState::Ended; }, "Retained device could not resume");
  Check(playback.Snapshot().position->ticks == 13024 && session.control->opens == 1,
        "Resuming on a retained device changed its media origin");
}

void EndedWaitsForPause() {
  Session session;
  auto& playback = *session.state;
  Wait([&] { return playback.Snapshot().state == PlaybackState::Paused; }, "Opening did not finish");
  session.control->fail_pause = true;
  session.control->pause.Arm();
  Check(playback.Submit(Frame(Config(session.control).format, 0, 1024)).Value() == SubmitStatus::Accepted &&
        playback.Finish().Succeeded() && playback.RequestPlay(true).Succeeded(), "Cannot play a short sealed tail");
  Wait([&] { return session.control->pause.entered.load(); }, "Final device pause did not start");
  Check(playback.Snapshot().state == PlaybackState::Playing && playback.Snapshot().position->ticks == 1024,
        "Ended was published before the device paused");
  session.control->pause.Release();
  Wait([&] { return playback.Snapshot().state == PlaybackState::Failed; }, "Pause failure was not reported");
  Wait([&] { return session.control->cleanup == 1; }, "Operational failure did not complete device cleanup");
}

void Lifecycle() {
  Session first;
  auto& playback = *first.state;
  auto control = first.control;
  Wait([&] { return playback.Snapshot().state == PlaybackState::Paused; }, "Opening did not finish");
  auto samples = std::make_shared<std::vector<std::int16_t>>(2048, 1200);
  AudioFrame frame{Config(control).format, {-1024, 48000}, 1024,
                   {BufferReference(std::as_bytes(std::span(*samples)), samples)}};
  Check(playback.Submit(frame).Value() == SubmitStatus::Accepted, "PCM was not accepted");
  samples->front() = 2300;
  Check(playback.Snapshot().position->ticks == -1024, "Submission moved the clock");
  control->observe.Arm();
  Check(playback.Finish().Succeeded() && playback.RequestPlay(true).Succeeded(), "Cannot start a short sealed tail");
  Wait([&] { return control->observe.entered.load(); }, "Device did not observe playback");
  Check(control->submitted_sample == 1200, "Accepted PCM retained the producer's mutable samples");
  Check(playback.Reset().Succeeded(), "Cannot reset while observing device progress");
  Check(playback.Snapshot().state == PlaybackState::Resetting && !playback.Snapshot().position,
        "Reset did not retire the old epoch immediately");
  Check(playback.Submit(frame).Value() == SubmitStatus::WouldBlock, "Reset accepted new PCM before device recreation");
  control->observe.Release();
  Wait([&] { return playback.Snapshot().state == PlaybackState::Paused; }, "Reset did not reopen playback");
  Check(!playback.Snapshot().position, "Old device completion advanced the new timeline");

  control->close.Arm();
  control->fail_close = true;
  playback.Close();
  Wait([&] { return control->close.entered.load(); }, "Device cleanup did not start");
  Check(playback.Snapshot().state == PlaybackState::Closed && control->cleanup == 0,
        "Logical close and cleanup did not remain separate");
  Check(!playback.Submit(frame).Succeeded(), "Closed playback accepted input");

  Session second;
  Wait([&] { return second.state->Snapshot().state == PlaybackState::Paused; }, "Another session could not open");
  Check(second.state->Submit(frame).Value() == SubmitStatus::Accepted &&
        second.state->Finish().Succeeded() && second.state->RequestPlay(true).Succeeded(), "Second playback failed");
  Wait([&] { return second.state->Snapshot().state == PlaybackState::Ended; }, "Stuck cleanup delayed another session");
  Check(second.state->Snapshot().position->ticks == 0, "Short tail ended at the wrong sample");
  second.state->Close();
  Wait([&] { return second.control->cleanup == 1; }, "Stuck cleanup delayed another session's release");

  playback.Close();
  control->close.Release();
  Wait([&] { return control->cleanup == 10; }, "Cleanup failure was not reported");
}
} // namespace

int main() {
  try {
    BackpressureStartsBelowRefill();
    RefillKeepsDevice();
    EndedWaitsForPause();
    Lifecycle();
    std::cout << "PCM backpressure, device reuse, drain ordering, reset and cleanup passed\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
