#include <huxerui/media_library.h>

#include "library/state.h"
#include "library/payload.h"

#include <huxerui/lifecycle.h>
#include <huxerui/state.h>
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

namespace huxerui::media::detail {

std::shared_ptr<MediaLibraryState> UseMediaLibraryState() {
  auto retained = UseState(std::make_shared<MediaLibraryState>());
  std::shared_ptr<MediaLibraryState> state = retained;
  Lifecycle([state] {
#if defined(__ANDROID__) || (defined(__APPLE__) && TARGET_OS_IPHONE)
    state->Attach(OpenPlatformModule<PlatformChannel>(media_library_type));
#else
    state->Attach([](std::string method, PlatformPayload, LibraryCompletion completion) -> LibraryCancel {
      if (method == "capabilities") completion(PlatformPayload(PlatformPayload::Object{
          {"maxSelectionCount", 0}, {"canSave", false}}));
      else completion(PlatformError{"media/unsupported", "Platform media library access requires Android or iOS", {}});
      return {};
    });
#endif
    return [state] { state->Detach(); };
  }, state);
  return state;
}

template <class T, class Decode>
Task<MediaLibraryResult<T>> Invoke(std::shared_ptr<MediaLibraryState> state, std::string method,
    PlatformPayload arguments, Decode decode) {
  auto reply = co_await AwaitCallback<LibraryReply>(
      [state = std::move(state), method = std::move(method), arguments = std::move(arguments)]
      (CallbackCompletion<LibraryReply> completion) mutable {
        return state->Request(std::move(method), std::move(arguments),
            [completion](LibraryReply reply) { completion.Complete(std::move(reply)); });
      });
  co_return DecodeLibraryReply<T>(reply, std::move(decode));
}

Task<MediaLibraryResult<void>> Save(std::shared_ptr<MediaLibraryState> state, File source, bool video) {
  return Invoke<void>(std::move(state), "save", PlatformPayload::Object{
      {"path", source.Path()}, {"video", video}}, DecodeSave);
}

#if !defined(__ANDROID__) && !(defined(__APPLE__) && TARGET_OS_IPHONE)
void InstallMediaLibrary(ApplicationContext&) {}
#endif

} // namespace huxerui::media::detail

namespace huxerui::media {

MediaPicker UseMediaPicker() { return MediaPicker(detail::UseMediaLibraryState()); }
MediaLibrary UseMediaLibrary() { return MediaLibrary(detail::UseMediaLibraryState()); }

Task<MediaLibraryResult<MediaLibraryCapabilities>> MediaPicker::CapabilitiesAsync() const {
  return detail::Invoke<MediaLibraryCapabilities>(state_, "capabilities", {}, detail::DecodeCapabilities);
}
Task<MediaLibraryResult<MediaLibraryCapabilities>> MediaLibrary::CapabilitiesAsync() const {
  return detail::Invoke<MediaLibraryCapabilities>(state_, "capabilities", {}, detail::DecodeCapabilities);
}
Task<MediaLibraryResult<std::vector<FileReference>>> MediaPicker::PickAsync(MediaPickerOptions options) const {
  detail::ValidatePickerOptions(options);
  return detail::Invoke<std::vector<FileReference>>(state_, "pick", PlatformPayload::Object{
      {"filter", static_cast<int>(options.filter)}, {"limit", options.selection_limit}},
      [limit = options.selection_limit](const PlatformPayload& payload) { return detail::DecodeSelection(payload, limit); });
}
Task<MediaLibraryResult<void>> MediaLibrary::SaveImageAsync(File source) const {
  return detail::Save(state_, std::move(source), false);
}
Task<MediaLibraryResult<void>> MediaLibrary::SaveVideoAsync(File source) const {
  return detail::Save(state_, std::move(source), true);
}

} // namespace huxerui::media
