#include "state.h"

#include <cmath>
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
  const auto weak = weak_from_this();
  channel.On("changed", [weak](const PlatformPayload&) {
    if (auto state = weak.lock()) state->NotifyChanged();
  });
  channel.On("progress", [weak](const PlatformPayload& payload) {
    if (auto state = weak.lock()) {
      const auto& fields = payload.AsObject();
      const auto id = fields.at("id").AsInteger();
      const double fraction = fields.at("fraction").AsDouble();
      const auto entry = state->pending_.find(static_cast<std::uint64_t>(id));
      if (entry != state->pending_.end() && std::isfinite(fraction) && fraction >= 0 && fraction <= 1) {
        auto request = entry->second;
        if (request->progress) request->progress(fraction);
      }
    }
  });
}

void MediaLibraryState::Detach() {
  start_ = {};
  observers_.clear();
  observing_ = false;
  auto pending = std::move(pending_);
  pending_.clear();
  for (auto& [id, request] : pending) {
    request->canceled = true;
    CancelSafely(std::move(request->cancel));
    request->completion(Unavailable());
  }
  CancelSafely(std::exchange(close_, {}));
}

LibraryCancel MediaLibraryState::Request(std::string method, PlatformPayload arguments, LibraryCompletion completion,
                                         std::function<void(double)> progress) {
  if (!start_) {
    completion(Unavailable());
    return {};
  }
  const auto id = ++next_id_;
  auto request = std::make_shared<Pending>();
  request->completion = std::move(completion);
  request->progress = std::move(progress);
  pending_.emplace(id, request);
  const auto weak = weak_from_this();
  auto start = start_;
  try {
    if (request->progress) {
      auto fields = arguments.AsObject();
      fields.insert_or_assign("progressId", id);
      arguments = PlatformPayload(std::move(fields));
    }
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

LibraryCancel MediaLibraryState::ConnectChanged(std::function<void()> handler) {
  if (!start_) return {};
  const auto id = ++next_id_;
  observers_.emplace(id, std::make_shared<std::function<void()>>(std::move(handler)));
  if (!std::exchange(observing_, true)) Request("observe", {}, [](LibraryReply) {});
  return [weak = weak_from_this(), id] { if (auto state = weak.lock()) state->observers_.erase(id); };
}

void MediaLibraryState::Invalidate() {
  if (start_) Request("invalidate", {}, [](LibraryReply) {});
}

void MediaLibraryState::NotifyChanged() {
  const auto observers = observers_;
  for (const auto& [id, callback] : observers)
    if (observers_.contains(id)) (*callback)();
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
