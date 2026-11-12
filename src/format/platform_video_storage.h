#pragma once

#include <huxerui/media_format.h>

#include <utility>

#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

namespace huxerui::media::detail {

struct PlatformVideoStorage {
  PlatformVideoStorage(std::uint32_t width, std::uint32_t height, VideoPixelFormat layout)
      : width(width), height(height), layout(layout) {}
  virtual ~PlatformVideoStorage() = default;

  const std::uint32_t width, height;
  const VideoPixelFormat layout;
};

struct VideoFrameAccess {
  static VideoFrame FromPlatform(VideoFormat format, MediaTimestamp pts, std::optional<MediaTimestamp> duration,
      std::shared_ptr<const PlatformVideoStorage> storage) {
    VideoFrame frame{format, pts, duration, {}};
    frame.platform_storage_ = std::move(storage);
    return frame;
  }

  static const std::shared_ptr<const PlatformVideoStorage>& PlatformStorage(const VideoFrame& frame) noexcept {
    return frame.platform_storage_;
  }

  // Keep the reservation or source texture with the image through its last use.
  static void RetainPlatformStorage(VideoFrame& frame, const std::shared_ptr<const void>& dependency) {
    struct Owner {
      std::shared_ptr<const void> dependency;
      std::shared_ptr<const PlatformVideoStorage> image;
    };
    auto owner = std::make_shared<Owner>(Owner{dependency, frame.platform_storage_});
    frame.platform_storage_ = std::shared_ptr<const PlatformVideoStorage>(std::move(owner), frame.platform_storage_.get());
  }
};

#if defined(__ANDROID__) || defined(__EMSCRIPTEN__)
inline constexpr auto PlatformVideoLayout = VideoPixelFormat::Opaque;
#else
inline constexpr auto PlatformVideoLayout = VideoPixelFormat::NV12;
#endif

#if defined(__ANDROID__) || defined(__EMSCRIPTEN__) || defined(_WIN32) || (defined(__APPLE__) && (TARGET_OS_OSX || TARGET_OS_IOS))
inline constexpr bool PlatformVideoSupported = true;
#else
inline constexpr bool PlatformVideoSupported = false;
#endif

inline constexpr unsigned VideoComponentBytes(VideoPixelFormat layout) noexcept {
  return layout == VideoPixelFormat::P010 || layout == VideoPixelFormat::P210 ? 2 : 1;
}

inline constexpr std::uint32_t VideoChromaHeight(std::uint32_t height, VideoPixelFormat layout) noexcept {
  return layout == VideoPixelFormat::P210 ? height : height / 2 + height % 2;
}

// Call with coded dimensions already bounded by the codec configuration.
inline std::size_t VideoImageBytes(std::uint32_t width, std::uint32_t height, VideoPixelFormat layout) {
  const auto pixels = std::size_t(width) * height;
  if (layout == VideoPixelFormat::Opaque) return pixels * 4;
  if (layout == VideoPixelFormat::Ayuv16) return pixels * 8;
  const auto samples = pixels + 2 * ((std::size_t(width) + 1) / 2) * VideoChromaHeight(height, layout);
  return samples * VideoComponentBytes(layout);
}

} // namespace huxerui::media::detail
