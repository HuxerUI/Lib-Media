#pragma once

#include <huxerui/media_player.h>

namespace huxerui::media::detail {

Rect FitVideo(Size source, Size bounds, const VideoSurfaceProperties& properties);
void ValidateVideoProperties(const VideoSurfaceProperties& properties);

} // namespace huxerui::media::detail
