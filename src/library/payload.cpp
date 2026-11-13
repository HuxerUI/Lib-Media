#include "payload.h"

#include <limits>

namespace huxerui::media::detail {

MediaLibraryError LibraryError(const PlatformError& error) {
  auto code = MediaLibraryErrorCode::Unknown;
  if (error.code == "media/unsupported") code = MediaLibraryErrorCode::Unsupported;
  else if (error.code == "media/permission") code = MediaLibraryErrorCode::PermissionDenied;
  else if (error.code == "media/invalid-source") code = MediaLibraryErrorCode::InvalidSource;
  else if (error.code == "media/not-found") code = MediaLibraryErrorCode::NotFound;
  else if (error.code == "media/stale-cursor") code = MediaLibraryErrorCode::StaleCursor;
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
  const auto browse = fields.find("canBrowse");
  return {static_cast<std::uint32_t>(limit), fields.at("canSave").AsBoolean(),
          browse != fields.end() && browse->second.AsBoolean()};
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

void ValidatePickerOptions(MediaFilter filter, std::uint32_t selection_limit) {
  ValidateFilter(filter);
  if (selection_limit == 0)
    throw std::invalid_argument("Media selection requires a valid filter and a positive limit");
}

void ValidateFilter(MediaFilter filter) {
  if (filter != MediaFilter::Images && filter != MediaFilter::Videos && filter != MediaFilter::ImagesAndVideos)
    throw std::invalid_argument("Invalid media filter");
}

std::vector<MediaAsset> DecodeAssets(const PlatformPayload& payload) {
  const auto& list = payload.AsList();
  std::vector<MediaAsset> assets;
  assets.reserve(list.size());
  for (const auto& value : list) {
    const auto& fields = value.AsObject();
    MediaAsset asset;
    asset.id.value = fields.at("id").AsString();
    if (asset.id.value.empty()) throw std::invalid_argument("Empty media asset ID");
    asset.kind = fields.at("video").AsBoolean() ? MediaAssetKind::Video : MediaAssetKind::Image;
    if (const auto& date = fields.at("date"); !date.IsNull()) {
      using Clock = std::chrono::system_clock;
      const std::chrono::milliseconds millis(date.AsInteger());
      if (millis >= std::chrono::duration_cast<std::chrono::milliseconds>(Clock::duration::min()) &&
          millis <= std::chrono::duration_cast<std::chrono::milliseconds>(Clock::duration::max()))
        asset.date = Clock::time_point(std::chrono::duration_cast<Clock::duration>(millis));
    }
    if (const auto& duration = fields.at("duration"); !duration.IsNull()) {
      const auto millis = duration.AsInteger();
      if (asset.kind != MediaAssetKind::Video || millis < 0) throw std::invalid_argument("Invalid asset duration");
      asset.duration = std::chrono::milliseconds(millis);
    }
    assets.push_back(std::move(asset));
  }
  return assets;
}

std::vector<MediaAlbum> DecodeAlbums(const PlatformPayload& payload) {
  std::vector<MediaAlbum> albums;
  for (const auto& value : payload.AsList()) {
    const auto& fields = value.AsObject();
    MediaAlbum album{
        .id = {std::string(fields.at("id").AsString())},
        .name = std::string(fields.at("name").AsString()),
        .cover_id = {std::string(fields.at("cover").AsString())},
    };
    if (album.id.value.empty() || album.cover_id.value.empty()) throw std::invalid_argument("Invalid media album");
    albums.push_back(std::move(album));
  }
  return albums;
}

MediaAssetPage DecodePage(const PlatformPayload& payload) {
  const auto& fields = payload.AsObject();
  MediaAssetPage page{DecodeAssets(fields.at("assets")), {}};
  const auto& cursor = fields.at("next");
  if (!cursor.IsNull()) {
    page.next_cursor = cursor.AsString();
    if (page.next_cursor->empty() || page.assets.empty()) throw std::invalid_argument("Invalid media continuation");
  }
  return page;
}

} // namespace huxerui::media::detail
