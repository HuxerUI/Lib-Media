#pragma once

#include <huxerui/external_texture.h>
#include <huxerui/media_codec.h>

#include <android/native_window.h>
#include <media/NdkImageReader.h>

namespace huxerui {
class ApplicationContext;
namespace android {
class GlTexture;
}
}

namespace huxerui::media::detail {

struct VideoTextureGeometry;

AHardwareBuffer* HardwareBuffer(const VideoFrame& frame) noexcept;
CodecResult<std::shared_ptr<AImageReader>> OpenVideoImageReader(const VideoDecoderConfig& config);
ANativeWindow* VideoImageReaderWindow(AImageReader* reader);
CodecResult<std::optional<VideoFrame>> ReadVideoImage(const std::shared_ptr<AImageReader>& reader,
    VideoFormat format, MediaTimestamp pts);

inline constexpr auto surface_preview_type = "Lib-Media.SurfacePreview";

struct SurfacePreviewOptions {
  VideoDecoderConfig config;
  Size size;
};

struct SurfacePreview {
  VideoDecoder decoder;
  std::shared_ptr<ExternalTexture> texture;
};

void InstallSurfacePreview(ApplicationContext& context);
CodecResult<void> PresentSurfaceFrame(const VideoFrame& frame);

class AndroidVideoRenderer {
public:
  explicit AndroidVideoRenderer(ANativeWindow* window = nullptr);
  ~AndroidVideoRenderer();
  AndroidVideoRenderer(const AndroidVideoRenderer&) = delete;
  AndroidVideoRenderer& operator=(const AndroidVideoRenderer&) = delete;

  void DrawToSurface(const VideoFrame& frame);
  CodecResult<VideoFrame> DrawToBuffer(const VideoFrame& source, VideoFormat target);
  void DrawToTexture(const VideoFrame& frame, huxerui::android::GlTexture& texture,
      VideoTextureOptions options = {}, const VideoTextureGeometry* geometry = nullptr);

private:
  struct Context;
  std::unique_ptr<Context> context_;
  friend CodecResult<std::shared_ptr<ExternalTexture>> PublishVideoPixels(Bytes pixels, VideoTextureOptions options);
};

} // namespace huxerui::media::detail
