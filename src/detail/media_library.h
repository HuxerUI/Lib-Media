#pragma once

#include <functional>
#include <map>
#include <stdexcept>
#include <type_traits>
#include <variant>
#include <huxerui/media_library.h>
#include <huxerui/platform_registry.h>

namespace huxerui::media::detail {

inline constexpr const char* media_library_type = "media/Library";
using LibraryReply = PlatformResult<PlatformPayload>;
using LibraryCompletion = std::function<void(LibraryReply)>;
using LibraryCancel = std::function<void()>;
using LibraryStart = std::function<LibraryCancel(std::string, PlatformPayload, LibraryCompletion)>;

class MediaLibraryState final : public std::enable_shared_from_this<MediaLibraryState> {
public:
  ~MediaLibraryState();
  void Attach(LibraryStart start, LibraryCancel close = {});
  void Attach(PlatformChannel channel);
  void Detach();
  LibraryCancel Request(std::string method, PlatformPayload arguments, LibraryCompletion completion);

private:
  struct Pending {
    LibraryCompletion completion;
    LibraryCancel cancel;
    bool canceled = false;
  };
  void Complete(std::uint64_t id, LibraryReply result);
  void Cancel(std::uint64_t id);
  LibraryStart start_;
  LibraryCancel close_;
  std::map<std::uint64_t, std::shared_ptr<Pending>> pending_;
  std::uint64_t next_id_ = 0;
};

MediaLibraryError LibraryError(const PlatformError& error);
MediaLibraryCapabilities DecodeCapabilities(const PlatformPayload& payload);
std::vector<FileReference> DecodeSelection(const PlatformPayload& payload, std::uint32_t limit);
void DecodeSave(const PlatformPayload& payload);
void ValidatePickerOptions(MediaPickerOptions options);
void InstallMediaLibrary(ApplicationContext& context);

template <class T, class Decode>
MediaLibraryResult<T> DecodeLibraryReply(const LibraryReply& reply, Decode decode) {
  using Result = MediaLibraryResult<T>;
  if (const auto* error = std::get_if<PlatformError>(&reply)) return Result::Failure(LibraryError(*error));
  try {
    if constexpr (std::is_void_v<T>) {
      decode(std::get<PlatformPayload>(reply));
      return Result::Success();
    } else {
      return Result::Success(decode(std::get<PlatformPayload>(reply)));
    }
  } catch (const std::exception& error) {
    return Result::Failure({MediaLibraryErrorCode::Unknown, error.what(), "media/invalid-result"});
  }
}

} // namespace huxerui::media::detail
