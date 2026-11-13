#pragma once

#include "backend.h"

#include <stdexcept>
#include <type_traits>
#include <variant>

namespace huxerui::media::detail {

MediaLibraryError LibraryError(const PlatformError& error);
MediaLibraryCapabilities DecodeCapabilities(const PlatformPayload& payload);
std::vector<FileReference> DecodeSelection(const PlatformPayload& payload, std::uint32_t limit);
void DecodeSave(const PlatformPayload& payload);
void ValidatePickerOptions(MediaFilter filter, std::uint32_t selection_limit);
void ValidateFilter(MediaFilter filter);
std::vector<MediaAsset> DecodeAssets(const PlatformPayload& payload);
std::vector<MediaAlbum> DecodeAlbums(const PlatformPayload& payload);
MediaAssetPage DecodePage(const PlatformPayload& payload);

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
