#include "executor.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#elif defined(_WIN32)
#include <windows.h>
#endif

namespace huxerui::media::detail {
#ifdef __EMSCRIPTEN__
void DispatchCodec(std::function<void()> work, unsigned delay_ms) {
  auto* pending = new std::function<void()>(std::move(work));
  emscripten_async_call([](void* context) {
    std::unique_ptr<std::function<void()>> callback(static_cast<std::function<void()>*>(context));
    (*callback)();
  }, pending, static_cast<int>(delay_ms));
}
#else
class CodecReaper {
  struct Queue {
    std::mutex mutex;
    std::condition_variable changed;
    bool stopping = false;
    std::shared_ptr<CodecWorker> retired;
  };
public:
  CodecReaper() : thread_([queue = queue_] { Run(*queue); }) {}
  ~CodecReaper() {
    {
      std::lock_guard lock(queue_->mutex);
      queue_->stopping = true;
    }
    queue_->changed.notify_one();
    thread_.join();
  }
  auto GetQueue() const { return queue_; }
  static void Retire(const std::shared_ptr<Queue>& queue, std::shared_ptr<CodecWorker> worker) {
    {
      std::lock_guard lock(queue->mutex);
      if (queue->stopping) {
        worker->thread_.detach();
        return;
      }
      worker->retired_next_ = std::move(queue->retired);
      queue->retired = std::move(worker);
    }
    queue->changed.notify_one();
  }
private:
  static bool Exited(CodecWorker& worker) {
#ifdef _WIN32
    // The native handle is signaled after thread-local resource destruction.
    return WaitForSingleObject(worker.thread_.native_handle(), 0) == WAIT_OBJECT_0;
#else
    return worker.exited_.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready;
#endif
  }

  static void Complete(std::shared_ptr<CodecWorker> worker) {
    worker->thread_.join();
    auto completion = std::move(worker->completion_);
    worker.reset();
    if (auto* codec = std::get_if<CodecWorker::CodecCompletion>(&completion)) {
      if (codec->callback) codec->callback(std::move(codec->result));
    } else if (auto& callback = std::get<1>(completion)) {
      callback();
    }
  }

  static void Run(Queue& queue) {
    using namespace std::chrono_literals;
    std::unique_lock lock(queue.mutex);
    while (!queue.stopping) {
      auto* next = &queue.retired;
      // An exiting worker may still be releasing thread-local native resources.
      while (*next && !Exited(**next))
        next = &(*next)->retired_next_;
      if (!*next) {
        if (queue.retired) queue.changed.wait_for(lock, 1ms);
        else queue.changed.wait(lock);
        continue;
      }
      auto worker = std::move(*next);
      *next = std::move(worker->retired_next_);
      lock.unlock();
      Complete(std::move(worker));
      lock.lock();
    }
    // Process shutdown must not wait for an unretired session or its thread-local destructors.
    auto pending = std::move(queue.retired);
    lock.unlock();
    while (pending) {
      auto worker = std::move(pending);
      pending = std::move(worker->retired_next_);
      worker->thread_.detach();
    }
  }
  std::shared_ptr<Queue> queue_ = std::make_shared<Queue>();
  std::thread thread_;
};

CodecWorker::CodecWorker(std::function<void(bool)> work) : work_(std::move(work)) {}

std::shared_ptr<CodecWorker> CodecWorker::Start(std::function<void(bool)> work) {
  static CodecReaper reaper;
  auto worker = std::shared_ptr<CodecWorker>(new CodecWorker(std::move(work)));
  worker->thread_ = std::thread([worker, queue = reaper.GetQueue()]() mutable {
    worker->Run();
    worker->work_ = {};
#ifndef _WIN32
    worker->exit_.set_value_at_thread_exit();
#endif
    CodecReaper::Retire(queue, std::move(worker));
  });
  return worker;
}

void CodecWorker::Schedule(unsigned delay_ms) {
  std::lock_guard lock(mutex_);
  if (finished_) return;
  const auto due = Clock::now() + std::chrono::milliseconds(delay_ms);
  if (!due_ || due < *due_) {
    due_ = due;
    changed_.notify_one();
  }
}

void CodecWorker::Signal() {
  std::lock_guard lock(mutex_);
  if (finished_) return;
  signaled_ = true;
  changed_.notify_one();
}

void CodecWorker::Finish(CodecResult<void> result, std::function<void(CodecResult<void>)> callback) {
  std::lock_guard lock(mutex_);
  completion_.emplace<CodecCompletion>(CodecCompletion{std::move(result), std::move(callback)});
  finished_ = true;
}

void CodecWorker::Finish(std::function<void()> completion) {
  std::lock_guard lock(mutex_);
  completion_ = std::move(completion);
  finished_ = true;
}

void CodecWorker::Run() {
  std::unique_lock lock(mutex_);
  while (!finished_) {
    if (!signaled_) {
      if (!due_) { changed_.wait(lock); continue; }
      const auto due = *due_;
      if (Clock::now() < due) { changed_.wait_until(lock, due); continue; }
    }
    due_.reset();
    const auto signaled = std::exchange(signaled_, false);
    lock.unlock();
    work_(signaled);
    lock.lock();
  }
}
#endif
} // namespace huxerui::media::detail
