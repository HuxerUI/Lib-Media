#pragma once

#include <huxerui/media_codec.h>

#include <array>

namespace huxerui::media::detail {

// Maps normalized top-left destination coordinates to normalized source coordinates.
using VideoSampling = std::array<std::array<float, 2>, 3>;
VideoSampling VideoSampleCoordinates(const VideoFormat& format);

class VideoFrameTransformBackend {
public:
  virtual ~VideoFrameTransformBackend() = default;
  virtual CodecResult<VideoFrame> Transform(const VideoFrame& source, VideoFormat target) = 0;
};

std::unique_ptr<VideoFrameTransformBackend> MakeVideoFrameTransformBackend();
CodecResult<std::shared_ptr<ExternalTexture>> MakePlatformVideoTexture(const VideoFrame& frame);

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
