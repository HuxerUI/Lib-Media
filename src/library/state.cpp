#include "state.h"

#include <exception>
#include <utility>

namespace huxerui::media::detail {

namespace {
void CancelSafely(LibraryCancel cancel) noexcept {
  if (cancel) {
    try { cancel(); } catch (...) {}
  }
}
PlatformError Unavailable() {
  return {"media/unavailable", "The media library session is no longer available", {}};
}
} // namespace

MediaLibraryState::~MediaLibraryState() { Detach(); }

void MediaLibraryState::Attach(LibraryStart start, LibraryCancel close) {
  Detach();
  start_ = std::move(start);
  close_ = std::move(close);
}

void MediaLibraryState::Attach(PlatformChannel channel) {
  Attach([channel](std::string method, PlatformPayload args, LibraryCompletion completion) -> LibraryCancel {
    auto id = channel.Invoke(std::move(method), std::move(args), std::move(completion));
    return [channel, id] { channel.Cancel(id); };
  }, [channel] { channel.Close(); });
}

void MediaLibraryState::Detach() {
  start_ = {};
  auto pending = std::move(pending_);
  pending_.clear();
  for (auto& [id, request] : pending) {
    request->canceled = true;
    CancelSafely(std::move(request->cancel));
    request->completion(Unavailable());
  }
  CancelSafely(std::exchange(close_, {}));
}

LibraryCancel MediaLibraryState::Request(std::string method, PlatformPayload arguments, LibraryCompletion completion) {
  if (!start_) {
    completion(Unavailable());
    return {};
  }
  const auto id = ++next_id_;
  auto request = std::make_shared<Pending>();
  request->completion = std::move(completion);
  pending_.emplace(id, request);
  const auto weak = weak_from_this();
  auto start = start_;
  try {
    auto cancel = start(std::move(method), std::move(arguments), [weak, id](LibraryReply result) {
      if (auto state = weak.lock()) state->Complete(id, std::move(result));
    });
    if (request->canceled) CancelSafely(std::move(cancel));
    else if (pending_.contains(id)) request->cancel = std::move(cancel);
  } catch (const std::exception& error) {
    Complete(id, PlatformError{"media/unknown", error.what(), {}});
  }
  return [weak, id] { if (auto state = weak.lock()) state->Cancel(id); };
}

void MediaLibraryState::Complete(std::uint64_t id, LibraryReply result) {
  auto entry = pending_.extract(id);
  if (!entry.empty()) entry.mapped()->completion(std::move(result));
}

void MediaLibraryState::Cancel(std::uint64_t id) {
  auto entry = pending_.extract(id);
  if (entry.empty()) return;
  entry.mapped()->canceled = true;
  CancelSafely(std::move(entry.mapped()->cancel));
}

} // namespace huxerui::media::detail
