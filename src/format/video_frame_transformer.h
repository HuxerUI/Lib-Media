#pragma once

#include <huxerui/media_codec.h>

#include <array>

namespace huxerui::media::detail {

// Maps normalized top-left destination coordinates to normalized source coordinates.
using VideoSampling = std::array<std::array<float, 2>, 3>;
VideoSampling VideoSampleCoordinates(const VideoFormat& format);

struct VideoTextureGeometry {
  // Source rectangle in normalized, display-oriented coordinates.
  Rect source;
  // Content rectangle in output pixels, measured from the top left.
  Rect destination;
  bool identity;
};

VideoTextureGeometry FitVideoTexture(const VideoFormat& format, VideoTextureOptions options);
VideoSampling VideoSampleCoordinates(const VideoFormat& format, Rect source);
VideoRectangle VideoSourceRectangle(const VideoFormat& format, Rect source);

using VideoTextureResult = CodecResult<std::shared_ptr<ExternalTexture>>;
VideoTextureResult MakeFittedPlatformVideoTexture(const VideoFrame& frame,
    VideoTextureOptions options, const VideoTextureGeometry& geometry);
VideoTextureResult PublishVideoPixels(Bytes pixels, VideoTextureOptions options);

class VideoFrameTransformBackend {
public:
  virtual ~VideoFrameTransformBackend() = default;
  virtual CodecResult<VideoFrame> Transform(const VideoFrame& source, VideoFormat target) = 0;
};

std::unique_ptr<VideoFrameTransformBackend> MakeVideoFrameTransformBackend();
CodecResult<std::shared_ptr<ExternalTexture>> MakePlatformVideoTexture(const VideoFrame& frame);
CodecResult<VideoFrame> AcquirePlatformVideoFrame(const ExternalTexture& texture, VideoFormat format,
    MediaTimestamp pts, std::optional<MediaTimestamp> duration);

// Reuses GPU resources across calls. Use and destroy the transformer on one thread.
class VideoFrameTransformer final {
public:
  // Crops the visible region, applies display rotation, and resizes to even dimensions up to 8192.
  // The result retains the input timing and owns the finished image independently of the transformer.
  CodecResult<VideoFrame> Transform(const VideoFrame& frame, std::uint32_t width, std::uint32_t height);

private:
  std::unique_ptr<VideoFrameTransformBackend> backend_;
};

} // namespace huxerui::media::detail
