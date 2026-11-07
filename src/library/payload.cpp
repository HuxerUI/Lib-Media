#include "payload.h"

#include <limits>

namespace huxerui::media::detail {

MediaLibraryError LibraryError(const PlatformError& error) {
  auto code = MediaLibraryErrorCode::Unknown;
  if (error.code == "media/unsupported") code = MediaLibraryErrorCode::Unsupported;
  else if (error.code == "media/permission") code = MediaLibraryErrorCode::PermissionDenied;
  else if (error.code == "media/invalid-source") code = MediaLibraryErrorCode::InvalidSource;
  else if (error.code == "media/unavailable" || error.code.starts_with("huxerui/"))
    code = MediaLibraryErrorCode::Unavailable;
  else if (error.code == "media/io") code = MediaLibraryErrorCode::Io;
  return {code, error.message, error.code};
}

MediaLibraryCapabilities DecodeCapabilities(const PlatformPayload& payload) {
  const auto& fields = payload.AsObject();
  const auto limit = fields.at("maxSelectionCount").AsInteger();
  if (limit < 0 || limit > std::numeric_limits<std::uint32_t>::max())
    throw std::invalid_argument("Invalid platform media selection limit");
  return {static_cast<std::uint32_t>(limit), fields.at("canSave").AsBoolean()};
}

std::vector<FileReference> DecodeSelection(const PlatformPayload& payload, std::uint32_t limit) {
  if (payload.IsNull()) return {};
  const auto& values = payload.AsList();
  if (values.empty() || values.size() > limit) throw std::invalid_argument("Invalid platform media selection count");
  std::vector<FileReference> result;
  for (const auto& value : values) {
    const auto& file = value.AsFileReference();
    if (file.Type() != FileType::File) throw std::invalid_argument("Selected media is not an ordinary file");
    result.push_back(file);
  }
  return result;
}

void DecodeSave(const PlatformPayload& payload) {
  if (!payload.IsNull()) throw std::invalid_argument("Invalid platform media save result");
}

void ValidatePickerOptions(MediaPickerOptions options) {
  if (options.selection_limit == 0 || (options.filter != MediaPickerFilter::Images &&
      options.filter != MediaPickerFilter::Videos && options.filter != MediaPickerFilter::ImagesAndVideos))
    throw std::invalid_argument("Media selection requires a valid filter and a positive limit");
}

} // namespace huxerui::media::detail
