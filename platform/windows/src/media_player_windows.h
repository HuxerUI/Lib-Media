#pragma once

#include <algorithm>

#include <huxerui/windows/platform_registry.h>
#include <dxgi1_2.h>
#include <mfmediaengine.h>
#include <wrl/client.h>
#include <cmath>

#include "player/state.h"
#include "player/video_geometry.h"

namespace huxerui::media::detail {

struct WindowsVideoRectangles {
  MFVideoNormalizedRect source;
  RECT destination;
};

inline std::optional<WindowsVideoRectangles> FitWindowsVideo(Size source, Size bounds,
                                                             const VideoSurfaceProperties& properties) {
  const auto fitted = FitVideo(source, bounds, properties);
  if (fitted.width <= 0 || fitted.height <= 0)
    return {};
  const float left = std::max(0.0F, fitted.x);
  const float top = std::max(0.0F, fitted.y);
  const float right = std::min(bounds.width, fitted.x + fitted.width);
  const float bottom = std::min(bounds.height, fitted.y + fitted.height);
  const RECT destination{std::lround(left), std::lround(top), std::lround(right), std::lround(bottom)};
  if (destination.right <= destination.left || destination.bottom <= destination.top)
    return {};

  // Compute cropping before pixel rounding so letterboxing cannot create out-of-range source coordinates.
  const MFVideoNormalizedRect crop{
      fitted.x >= 0 ? 0.0F : std::clamp((left - fitted.x) / fitted.width, 0.0F, 1.0F),
      fitted.y >= 0 ? 0.0F : std::clamp((top - fitted.y) / fitted.height, 0.0F, 1.0F),
      fitted.x + fitted.width <= bounds.width ? 1.0F : std::clamp((right - fitted.x) / fitted.width, 0.0F, 1.0F),
      fitted.y + fitted.height <= bounds.height ? 1.0F : std::clamp((bottom - fitted.y) / fitted.height, 0.0F, 1.0F)};
  return WindowsVideoRectangles{crop, destination};
}

struct VideoWindow : std::enable_shared_from_this<VideoWindow> {
  HWND window = nullptr;
  VideoSurfaceProperties properties;
  std::weak_ptr<PlayerBackend> player;
  Microsoft::WRL::ComPtr<IDXGISwapChain1> swapchain;
  UINT width = 0;
  UINT height = 0;
  bool dirty = true;
};

std::shared_ptr<PlayerBackend> CreateWindowsPlayer();
windows::PlatformViewFactory<VideoSurfaceProperties, VideoWindow, MediaPlayer> CreateWindowsVideoFactory();

} // namespace huxerui::media::detail
