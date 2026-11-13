#include <huxerui/media_library.h>

#include "library/state.h"
#include "library/payload.h"
#include "library/access.h"

#include <huxerui/lifecycle.h>
#include <huxerui/state.h>
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

namespace huxerui::media::detail {

template <class T, class Decode>
Task<MediaLibraryResult<T>> Invoke(std::shared_ptr<MediaLibraryState> state, std::string method,
    PlatformPayload arguments, Decode decode, std::function<void(double)> progress = {}) {
  auto reply = co_await AwaitCallback<LibraryReply>(
      [state = std::move(state), method = std::move(method), arguments = std::move(arguments), progress = std::move(progress)]
      (CallbackCompletion<LibraryReply> completion) mutable {
        return state->Request(std::move(method), std::move(arguments),
            [completion](LibraryReply reply) { completion.Complete(std::move(reply)); }, std::move(progress));
      });
  co_return DecodeLibraryReply<T>(reply, std::move(decode));
}

Task<MediaLibraryResult<void>> Save(std::shared_ptr<MediaLibraryState> state, File source, bool video) {
  return Invoke<void>(std::move(state), "save", PlatformPayload::Object{
      {"path", source.Path()}, {"video", video}}, DecodeSave);
}

Task<MediaLibraryResult<void>> MediaLibraryAccess::ManageLimitedAccessAsync(const MediaLibrary& library) {
  return Invoke<void>(library.state_, "manageLimitedAccess", {}, DecodeSave);
}

void ValidateAssetId(const MediaAssetId& id) {
  if (id.value.empty()) throw std::invalid_argument("Media asset ID is empty");
}

#if !defined(__ANDROID__) && !(defined(__APPLE__) && TARGET_OS_IPHONE)
void InstallMediaLibrary(ApplicationContext&) {}
#endif

} // namespace huxerui::media::detail

namespace huxerui::media {

MediaLibrary UseMediaLibrary() {
  auto retained = UseState(std::make_shared<detail::MediaLibraryState>());
  auto state = retained.Get();
  Lifecycle([state] {
#if defined(__ANDROID__) || (defined(__APPLE__) && TARGET_OS_IPHONE)
    state->Attach(OpenPlatformModule<PlatformChannel>(detail::media_library_type));
#else
    state->Attach([](std::string method, PlatformPayload, detail::LibraryCompletion completion) -> detail::LibraryCancel {
      if (method == "capabilities") completion(PlatformPayload(PlatformPayload::Object{
          {"maxSelectionCount", 0}, {"canSave", false}}));
      else completion(PlatformError{"media/unsupported", "Platform media library access requires Android or iOS", {}});
      return {};
    });
#endif
    return [state] { state->Detach(); };
  }, state);
  UseApplication().OnLifecycleChanged([state](ApplicationLifecycleState value) {
    if (value == ApplicationLifecycleState::Active) state->Invalidate();
  }, state);
  return MediaLibrary(std::move(state));
}

Task<MediaLibraryResult<MediaLibraryCapabilities>> MediaLibrary::CapabilitiesAsync() const {
  return detail::Invoke<MediaLibraryCapabilities>(state_, "capabilities", {}, detail::DecodeCapabilities);
}

Task<MediaLibraryResult<std::vector<FileReference>>> MediaLibrary::PickAsync(SystemMediaPickerOptions options) const {
  detail::ValidatePickerOptions(options.filter, options.selection_limit);
  return detail::Invoke<std::vector<FileReference>>(state_, "pick", PlatformPayload::Object{
      {"filter", static_cast<int>(options.filter)}, {"limit", options.selection_limit}},
      [limit = options.selection_limit](const PlatformPayload& payload) { return detail::DecodeSelection(payload, limit); });
}

Task<MediaLibraryResult<std::vector<MediaAlbum>>> MediaLibrary::ListAlbumsAsync(MediaFilter filter) const {
  detail::ValidateFilter(filter);
  return detail::Invoke<std::vector<MediaAlbum>>(state_, "listAlbums",
      PlatformPayload::Object{{"filter", static_cast<int>(filter)}}, detail::DecodeAlbums);
}

Task<MediaLibraryResult<MediaAssetPage>> MediaLibrary::ListAssetsAsync(MediaAssetQuery query) const {
  detail::ValidateFilter(query.filter);
  if (query.page_size == 0 || query.page_size > 256 || (query.album_id && query.album_id->value.empty()) ||
      (query.cursor && query.cursor->empty())) throw std::invalid_argument("Invalid media page query");
  return detail::Invoke<MediaAssetPage>(state_, "listAssets", PlatformPayload::Object{
      {"filter", static_cast<int>(query.filter)}, {"limit", query.page_size},
      {"album", query.album_id ? PlatformPayload(std::move(query.album_id->value)) : PlatformPayload{}},
      {"cursor", query.cursor ? PlatformPayload(std::move(*query.cursor)) : PlatformPayload{}}}, detail::DecodePage);
}

Task<MediaLibraryResult<std::vector<MediaAsset>>> MediaLibrary::FindAssetsAsync(std::vector<MediaAssetId> ids) const {
  PlatformPayload::List values;
  values.reserve(ids.size());
  for (auto& id : ids) {
    detail::ValidateAssetId(id);
    values.emplace_back(std::move(id.value));
  }
  return detail::Invoke<std::vector<MediaAsset>>(state_, "findAssets", PlatformPayload::Object{
      {"count", ids.size()}, {"ids", std::move(values)}}, detail::DecodeAssets);
}

Task<MediaLibraryResult<std::shared_ptr<ExternalTexture>>> MediaLibrary::LoadImageAsync(
    MediaAssetId id, MediaImageRequest request, std::function<void(double)> on_progress) const {
  detail::ValidateAssetId(id);
  if (request.max_width == 0 || request.max_width > 4096 || request.max_height == 0 || request.max_height > 4096)
    throw std::invalid_argument("Invalid media image dimensions");
  return detail::Invoke<std::shared_ptr<ExternalTexture>>(state_, "loadImage", PlatformPayload::Object{
      {"id", std::move(id.value)}, {"width", request.max_width}, {"height", request.max_height},
      {"allowNetwork", request.allow_network}, {"progressId", 0}},
      [](const PlatformPayload& payload) -> std::shared_ptr<ExternalTexture> {
        if (payload.IsNull()) return {};
        const auto& texture = payload.AsExternalTexture();
        if (!texture) throw std::invalid_argument("Missing media image texture");
        return texture;
      }, std::move(on_progress));
}

Task<MediaLibraryResult<FileReference>> MediaLibrary::OpenAssetAsync(
    MediaAssetId id, std::function<void(double)> on_progress) const {
  detail::ValidateAssetId(id);
  return detail::Invoke<FileReference>(state_, "openAsset", PlatformPayload::Object{
      {"id", std::move(id.value)}, {"progressId", 0}},
      [](const PlatformPayload& payload) {
        const auto& file = payload.AsFileReference();
        if (file.Type() != FileType::File) throw std::invalid_argument("Media asset is not an ordinary file");
        return file;
      }, std::move(on_progress));
}

std::function<void()> MediaLibrary::ConnectChanged(std::function<void()> handler) const {
  return state_->ConnectChanged(std::move(handler));
}
Task<MediaLibraryResult<void>> MediaLibrary::SaveImageAsync(File source) const {
  return detail::Save(state_, std::move(source), false);
}
Task<MediaLibraryResult<void>> MediaLibrary::SaveVideoAsync(File source) const {
  return detail::Save(state_, std::move(source), true);
}

} // namespace huxerui::media
