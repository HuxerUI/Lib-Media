#pragma once

#include <huxerui/media_library.h>

namespace huxerui::media::detail {

struct MediaLibraryAccess {
  static Task<MediaLibraryResult<void>> ManageLimitedAccessAsync(const MediaLibrary& library);
};

} // namespace huxerui::media::detail
