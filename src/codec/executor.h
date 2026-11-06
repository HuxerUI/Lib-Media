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
void DispatchCodec(std::function<void()> work, unsigned delay_ms = 0);
#else
class CodecReaper;

// A native backend stays on its own worker from Open through Close.
class CodecWorker {
public:
  static std::shared_ptr<CodecWorker> Start(std::function<void(bool)> work);
  void Schedule(unsigned delay_ms = 0);
  void NotifyCredit();
  void Finish(CodecResult<void> result, std::function<void(CodecResult<void>)> callback);
private:
  friend class CodecReaper;
  using Clock = std::chrono::steady_clock;
  struct Completion {
    CodecResult<void> result;
    std::function<void(CodecResult<void>)> callback;
  };
  explicit CodecWorker(std::function<void(bool)> work);
  void Run();
  std::mutex mutex_;
  std::condition_variable changed_;
  std::optional<Clock::time_point> due_;
  bool credit_available_ = false, finished_ = false;
  std::function<void(bool)> work_;
  std::optional<Completion> completion_;
  std::promise<void> exit_;
  std::future<void> exited_ = exit_.get_future();
  std::thread thread_;
  std::shared_ptr<CodecWorker> retired_next_;
};
#endif
} // namespace huxerui::media::detail
