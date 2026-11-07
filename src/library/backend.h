#pragma once

#include <functional>
#include <huxerui/media_library.h>
#include <huxerui/platform_registry.h>

namespace huxerui::media::detail {

inline constexpr const char* media_library_type = "media/Library";
using LibraryReply = PlatformResult<PlatformPayload>;
using LibraryCompletion = std::function<void(LibraryReply)>;
using LibraryCancel = std::function<void()>;
using LibraryStart = std::function<LibraryCancel(std::string, PlatformPayload, LibraryCompletion)>;

void InstallMediaLibrary(ApplicationContext& context);

} // namespace huxerui::media::detail
