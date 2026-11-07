#include "library/state.h"
#include "library/payload.h"
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace huxerui;
using namespace huxerui::media;
using namespace huxerui::media::detail;

namespace {
void Check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
template <class Function> void Invalid(Function function) {
  try { function(); } catch (const std::exception&) { return; }
  throw std::runtime_error("Expected an invalid platform result or argument to be rejected");
}
}

int main() {
  try {
    ValidatePickerOptions({MediaPickerFilter::Videos, 4});
    Invalid([] { ValidatePickerOptions({MediaPickerFilter::Images, 0}); });
    Invalid([] { ValidatePickerOptions({static_cast<MediaPickerFilter>(99), 1}); });
    const auto selection = [](const PlatformPayload& payload) { return DecodeSelection(payload, 1); };
    auto dismissal = DecodeLibraryReply<std::vector<FileReference>>(PlatformPayload{}, selection);
    Check(dismissal.Succeeded() && dismissal.Value().empty(), "Platform dismissal must be a successful empty selection");
    auto denied = DecodeLibraryReply<std::vector<FileReference>>(
        PlatformError{"media/permission", "denied", {}}, selection);
    Check(!denied.Succeeded() && denied.Error().code == MediaLibraryErrorCode::PermissionDenied,
          "Denied selection must remain a failure rather than an empty success");
    auto malformed = DecodeLibraryReply<std::vector<FileReference>>(PlatformPayload::List{}, selection);
    Check(!malformed.Succeeded() && malformed.Error().platform_code == "media/invalid-result",
          "An invalid platform empty list must not be mistaken for user dismissal");
    Invalid([] { DecodeSelection(PlatformPayload::List{}, 1); });
    Invalid([] { DecodeSelection(PlatformPayload::List{1, 2}, 1); });
    Invalid([] { DecodeSelection(PlatformPayload::List{1}, 1); });
    auto unsupported = DecodeLibraryReply<MediaLibraryCapabilities>(PlatformPayload::Object{
        {"maxSelectionCount", 0}, {"canSave", true}}, DecodeCapabilities);
    Check(unsupported.Succeeded() && !unsupported.Value().CanPick() && unsupported.Value().can_save,
          "Selection and saving availability must remain independent");
    auto supported = DecodeCapabilities(PlatformPayload::Object{{"maxSelectionCount", 3}, {"canSave", false}});
    Check(supported.CanPick() && supported.max_selection_count == 3 && !supported.can_save,
          "The platform selection count must determine picker support without a separate flag");
    Invalid([] { DecodeCapabilities(PlatformPayload::Object{
        {"maxSelectionCount", -1}, {"canSave", true}}); });
    Invalid([] { DecodeCapabilities(PlatformPayload::Object{
        {"maxSelectionCount", std::int64_t{std::numeric_limits<std::uint32_t>::max()} + 1}, {"canSave", true}}); });
    Invalid([] { DecodeCapabilities(PlatformPayload::Object{{"canSave", true}}); });
    auto published = DecodeLibraryReply<void>(PlatformPayload{}, DecodeSave);
    Check(published.Succeeded(), "Platform publication confirmation must produce void success");
    published.Value();
    auto failed_save = DecodeLibraryReply<void>(PlatformError{"media/io", "publication failed", {}}, DecodeSave);
    Check(!failed_save.Succeeded() && failed_save.Error().code == MediaLibraryErrorCode::Io,
          "Platform publication failure must preserve its error with a void result");
    auto invalid_save = DecodeLibraryReply<void>(PlatformPayload(true), DecodeSave);
    Check(!invalid_save.Succeeded() && invalid_save.Error().platform_code == "media/invalid-result",
          "Unexpected platform save payloads must not report publication success");
    Check(LibraryError({"media/permission", "denied", {}}).code == MediaLibraryErrorCode::PermissionDenied,
          "Permission denial must be distinguishable");
    Check(LibraryError({"media/unsupported", "unsupported", {}}).code == MediaLibraryErrorCode::Unsupported,
          "Unsupported capability must be distinguishable");

    auto state = std::make_shared<MediaLibraryState>();
    LibraryCompletion reply;
    int completed = 0, canceled = 0, closed = 0;
    state->Attach([&](std::string, PlatformPayload, LibraryCompletion callback) -> LibraryCancel {
      reply = std::move(callback);
      return [&] { ++canceled; };
    }, [&] { ++closed; });
    auto cancel = state->Request("pick", {}, [&](LibraryReply result) {
      Check(std::holds_alternative<PlatformPayload>(result), "Successful platform reply was lost");
      ++completed;
    });
    reply(PlatformPayload{});
    reply(PlatformPayload{});
    cancel();
    Check(completed == 1 && canceled == 0, "Completion must be delivered once and not canceled afterward");

    cancel = state->Request("pick", {}, [&](LibraryReply) { ++completed; });
    cancel();
    cancel();
    reply(PlatformPayload{});
    Check(completed == 1 && canceled == 1, "Task cancellation must suppress late delivery and cancel once");

    state->Request("save", {}, [&](LibraryReply result) {
      auto retired = DecodeLibraryReply<void>(result, DecodeSave);
      Check(!retired.Succeeded() && retired.Error().code == MediaLibraryErrorCode::Unavailable,
            "Session retirement must settle surviving application tasks");
      ++completed;
    });
    auto late = reply;
    state->Detach();
    late(PlatformPayload{});
    Check(completed == 2 && canceled == 2 && closed == 1, "Retirement must close platform work without late delivery");
    state->Request("pick", {}, [&](LibraryReply result) {
      Check(std::holds_alternative<PlatformError>(result), "A retired handle must reject new work");
      ++completed;
    });
    Check(completed == 3, "Retired request was left pending");

    state->Attach([&](std::string, PlatformPayload, LibraryCompletion callback) -> LibraryCancel {
      callback(PlatformPayload{});
      return [&] { ++canceled; };
    });
    state->Request("pick", {}, [&](LibraryReply) { ++completed; });
    state->Detach();
    Check(completed == 4 && canceled == 2, "Inline completion must not retain a cancellation action");

    state->Attach([&](std::string, PlatformPayload, LibraryCompletion) -> LibraryCancel {
      state->Detach();
      return [&] { ++canceled; };
    });
    state->Request("pick", {}, [&](LibraryReply) { ++completed; });
    Check(completed == 5 && canceled == 3, "Retirement during startup must cancel the returned platform operation");
    std::cout << "Media library contract checks passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
