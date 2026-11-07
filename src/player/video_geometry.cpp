#include "video_geometry.h"

#include <algorithm>
#include <stdexcept>

namespace huxerui::media::detail {

Rect FitVideo(Size source, Size bounds, const VideoSurfaceProperties& properties) {
  if (source.width <= 0 || source.height <= 0 || bounds.width <= 0 || bounds.height <= 0)
    return {};
  float scale = 1;
  switch (properties.fit) {
  case ImageFit::Contain:
    scale = std::min(bounds.width / source.width, bounds.height / source.height);
    break;
  case ImageFit::Cover:
    scale = std::max(bounds.width / source.width, bounds.height / source.height);
    break;
  case ImageFit::ScaleDown:
    scale = std::min(1.0F, std::min(bounds.width / source.width, bounds.height / source.height));
    break;
  case ImageFit::Fill:
    return {0, 0, bounds.width, bounds.height};
  case ImageFit::None:
    break;
  }
  const float width = source.width * scale;
  const float height = source.height * scale;
  float x = (bounds.width - width) / 2;
  float y = (bounds.height - height) / 2;
  if (properties.horizontal_alignment == HorizontalAlignment::Start)
    x = 0;
  if (properties.horizontal_alignment == HorizontalAlignment::End)
    x = bounds.width - width;
  if (properties.vertical_alignment == VerticalAlignment::Start)
    y = 0;
  if (properties.vertical_alignment == VerticalAlignment::End)
    y = bounds.height - height;
  return {x, y, width, height};
}

void ValidateVideoProperties(const VideoSurfaceProperties& properties) {
  if (properties.horizontal_alignment == HorizontalAlignment::Stretch ||
      properties.vertical_alignment == VerticalAlignment::Stretch) {
    throw std::invalid_argument("HuxerUI video content alignment cannot be Stretch");
  }
}

} // namespace huxerui::media::detail
