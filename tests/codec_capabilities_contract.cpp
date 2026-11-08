#include "codec/capabilities.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <iostream>
#include <thread>

using namespace huxerui::media;
using namespace huxerui::media::detail;
namespace {
void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class T> T Wait(std::future<T>& result) {
  Check(result.wait_for(std::chrono::seconds(5)) == std::future_status::ready, "Capability query completion timed out");
  return result.get();
}
struct Control {
  std::mutex mutex;
  std::condition_variable changed;
  bool entered_query = false, release_query = false, entered = false, release = false;
  std::atomic<bool> exited{false}, query_capture_released{false};
  std::thread::id worker;
};
struct WorkerExit {
  std::shared_ptr<Control> control;
  ~WorkerExit() {
    std::unique_lock lock(control->mutex);
    control->entered = true; control->changed.notify_one();
    control->changed.wait(lock, [&] { return control->release; });
    control->exited = true;
  }
};
struct QueryCapture {
  std::shared_ptr<Control> control;
  ~QueryCapture() { control->query_capture_released = true; }
};
struct Unblock {
  std::shared_ptr<Control> control;
  ~Unblock() {
    std::lock_guard lock(control->mutex);
    control->release_query = control->release = true; control->changed.notify_one();
  }
};
void WorkerLifetime() {
  auto control = std::make_shared<Control>();
  Unblock unblock{control};
  auto capture = std::make_shared<QueryCapture>(); capture->control = control;
  auto completed = std::make_shared<std::promise<bool>>(); auto result = completed->get_future();
  const auto caller = std::this_thread::get_id();
  RunCapabilitiesQuery(CapabilityKind::AudioDecoder, [control, completed, caller](auto value) {
    completed->set_value(value.Succeeded() && control->exited && control->query_capture_released &&
        std::this_thread::get_id() != caller && std::this_thread::get_id() != control->worker);
  }, [control, capture](auto kind) {
    static thread_local WorkerExit exit;
    exit.control = control; control->worker = std::this_thread::get_id();
    std::unique_lock lock(control->mutex);
    control->entered_query = true; control->changed.notify_one();
    control->changed.wait(lock, [&] { return control->release_query; });
    return CapabilityResult::Success(DefaultCapabilities(kind));
  });
  capture.reset();
  {
    std::unique_lock lock(control->mutex);
    Check(control->changed.wait_for(lock, std::chrono::seconds(5), [&] { return control->entered_query; }), "Query worker did not begin discovery");
  }
  auto independent = std::make_shared<std::promise<CapabilityResult>>(); auto other = independent->get_future();
  RunCapabilitiesQuery(CapabilityKind::VideoEncoder, [independent](auto value) { independent->set_value(std::move(value)); },
      [](auto kind) { return CapabilityResult::Success(DefaultCapabilities(kind)); });
  Check(Wait(other).Succeeded(), "One query blocked an independent query");
  {
    std::unique_lock lock(control->mutex);
    control->release_query = true; control->changed.notify_one();
    Check(control->changed.wait_for(lock, std::chrono::seconds(5), [&] { return control->entered; }), "Query worker did not reach TLS teardown");
  }
  Check(result.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready, "Completion preceded worker exit");
  {
    std::lock_guard lock(control->mutex);
    control->release = true; control->changed.notify_one();
  }
  Check(Wait(result), "Callback ran before capture cleanup/TLS exit or on the wrong thread");
}
void Failures() {
  for (int failure = 0; failure < 3; ++failure) {
    auto completed = std::make_shared<std::promise<CapabilityResult>>(); auto result = completed->get_future();
    RunCapabilitiesQuery(CapabilityKind::AudioEncoder, [completed](auto value) { completed->set_value(std::move(value)); },
        [failure](auto) -> CapabilityResult {
          if (failure == 0) throw std::bad_alloc();
          if (failure == 1) throw std::runtime_error("Injected discovery failure");
          return CapabilityResult::Failure(Error(CodecErrorCode::Unsupported, "Injected platform error"));
        });
    auto value = Wait(result);
    Check(!value.Succeeded(), "Discovery failure was reported as an empty successful snapshot");
    Check(value.Error().code == (failure == 0 ? CodecErrorCode::ResourceExhausted :
        failure == 1 ? CodecErrorCode::PlatformFailure : CodecErrorCode::Unsupported), "Discovery error changed category");
  }
}
void UnknownBounds() {
  auto snapshot = DefaultCapabilities(CapabilityKind::VideoEncoder);
  const auto& unknown = std::get<VideoEncoderCapabilities>(snapshot);
  Check(unknown.availability == CodecSupport::Unknown && !unknown.platform_limits.width &&
      !unknown.platform_limits.maximum_frame_rate && !unknown.platform_limits.bitrate,
      "Library bounds were substituted for unknown platform bounds");
  Check(unknown.profiles[1].bounded_b_frames == CodecSupport::Unknown && !unknown.profiles[1].maximum_b_frames,
      "An unknown B-picture ceiling became zero");
  SetUnavailable(snapshot, "Missing component");
  const auto& unavailable = std::get<VideoEncoderCapabilities>(snapshot);
  Check(unavailable.availability == CodecSupport::Unsupported && !unavailable.platform_limits.width &&
      unavailable.profiles[1].bounded_b_frames == CodecSupport::Unsupported && unavailable.profiles[1].maximum_b_frames == 0,
      "Unavailable and unknown discovery were conflated");
}
}
int main() {
  try {
    UnknownBounds(); WorkerLifetime(); Failures();
    std::cout << "Codec capability worker and evidence contracts passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
