#pragma once

#include "backend.h"

#include <map>

namespace huxerui::media::detail {

class MediaLibraryState final : public std::enable_shared_from_this<MediaLibraryState> {
public:
  ~MediaLibraryState();
  void Attach(LibraryStart start, LibraryCancel close = {});
  void Attach(PlatformChannel channel);
  void Detach();
  LibraryCancel Request(std::string method, PlatformPayload arguments, LibraryCompletion completion,
                        std::function<void(double)> progress = {});
  LibraryCancel ConnectChanged(std::function<void()> handler);
  void Invalidate();

private:
  struct Pending {
    LibraryCompletion completion;
    LibraryCancel cancel;
    std::function<void(double)> progress;
    bool canceled = false;
  };
  void Complete(std::uint64_t id, LibraryReply result);
  void Cancel(std::uint64_t id);
  LibraryStart start_;
  LibraryCancel close_;
  std::map<std::uint64_t, std::shared_ptr<Pending>> pending_;
  std::map<std::uint64_t, std::shared_ptr<std::function<void()>>> observers_;
  bool observing_ = false;
  std::uint64_t next_id_ = 0;
  void NotifyChanged();
};

} // namespace huxerui::media::detail
