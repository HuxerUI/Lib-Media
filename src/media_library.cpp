#include "detail/media_library.h"

#include <limits>
#include <huxerui/lifecycle.h>
#include <huxerui/state.h>
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

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
    throw std::invalid_argument("Invalid native media selection limit");
  return {static_cast<std::uint32_t>(limit), fields.at("canSave").AsBoolean()};
}

std::vector<FileReference> DecodeSelection(const PlatformPayload& payload, std::uint32_t limit) {
  if (payload.IsNull()) return {};
  const auto& values = payload.AsList();
  if (values.empty() || values.size() > limit) throw std::invalid_argument("Invalid native media selection count");
  std::vector<FileReference> result;
  for (const auto& value : values) {
    const auto& file = value.AsFileReference();
    if (file.Type() != FileType::File) throw std::invalid_argument("Selected media is not an ordinary file");
    result.push_back(file);
  }
  return result;
}

void DecodeSave(const PlatformPayload& payload) {
  if (!payload.IsNull()) throw std::invalid_argument("Invalid native media save result");
}

void ValidatePickerOptions(MediaPickerOptions options) {
  if (options.selection_limit == 0 || (options.filter != MediaPickerFilter::Images &&
      options.filter != MediaPickerFilter::Videos && options.filter != MediaPickerFilter::ImagesAndVideos))
    throw std::invalid_argument("Media selection requires a valid filter and a positive limit");
}

std::shared_ptr<MediaLibraryState> UseMediaLibraryState() {
  auto retained = UseState(std::make_shared<MediaLibraryState>());
  auto state = retained.Get();
  Lifecycle([state] {
#if defined(__ANDROID__) || (defined(__APPLE__) && TARGET_OS_IPHONE)
    state->Attach(OpenPlatformModule<PlatformChannel>(media_library_type));
#else
    state->Attach([](std::string method, PlatformPayload, LibraryCompletion completion) -> LibraryCancel {
      if (method == "capabilities") completion(PlatformPayload(PlatformPayload::Object{
          {"maxSelectionCount", 0}, {"canSave", false}}));
      else completion(PlatformError{"media/unsupported", "Native media library access requires Android or iOS", {}});
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
