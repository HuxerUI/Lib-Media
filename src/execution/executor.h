#pragma once

#include <huxerui/media_codec.h>

#ifndef __EMSCRIPTEN__
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <thread>
#endif

namespace huxerui::media::detail {
#ifdef __EMSCRIPTEN__
void DispatchMedia(std::function<void()> work, unsigned delay_ms = 0);
#else
class MediaReaper;

// Each desktop or mobile codec or playback session owns a worker through teardown and post-exit completion.
class MediaWorker {
public:
  static std::shared_ptr<MediaWorker> Start(std::function<void(bool)> work);
  void Schedule(unsigned delay_ms = 0);
  void Signal();
  void Finish(std::function<void()> completion);
  void Finish(CodecResult<void> result, std::function<void(CodecResult<void>)> callback);
private:
  friend class MediaReaper;
  using Clock = std::chrono::steady_clock;
  struct CodecCompletion {
    CodecResult<void> result;
    std::function<void(CodecResult<void>)> callback;
  };
  explicit MediaWorker(std::function<void(bool)> work);
  void Run();
  std::mutex mutex_;
  std::condition_variable changed_;
  std::optional<Clock::time_point> due_;
  bool signaled_ = false, finished_ = false;
  std::function<void(bool)> work_;
  std::variant<CodecCompletion, std::function<void()>> completion_{std::in_place_index<1>};
#ifndef _WIN32
  std::promise<void> exit_;
  std::future<void> exited_ = exit_.get_future();
#endif
  std::thread thread_;
  std::shared_ptr<MediaWorker> retired_next_;
};
#endif
} // namespace huxerui::media::detail
