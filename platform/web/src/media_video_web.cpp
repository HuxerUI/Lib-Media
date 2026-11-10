#include "media_video_web.h"

#include "codec/common.h"
#include "format/platform_video_storage.h"
#include "format/video_frame_transformer.h"

#include <huxerui/web/external_texture.h>
#include <emscripten/threading.h>

namespace huxerui::media::detail {
namespace {

struct WebVideoStorage final : PlatformVideoStorage {
  WebVideoStorage(const VideoFormat& format, BrowserVideoFrame image)
      : PlatformVideoStorage(format.width, format.height, format.pixel_format), image(std::move(image)) {}
  BrowserVideoFrame image;
};

} // namespace

VideoFrame RetainBrowserVideoFrame(BrowserVideoFrame image, VideoFormat format,
    MediaTimestamp pts, std::optional<MediaTimestamp> duration) {
  return VideoFrameAccess::FromPlatform(format, pts, duration, std::make_shared<WebVideoStorage>(format, std::move(image)));
}

const emscripten::val* BrowserFrame(const VideoFrame& frame) noexcept {
  const auto* storage = dynamic_cast<const WebVideoStorage*>(VideoFrameAccess::PlatformStorage(frame).get());
  return storage ? &storage->image.value : nullptr;
}

CodecResult<void> ValidateBrowserFrame(const emscripten::val& image, const VideoFormat& format) {
  using R = CodecResult<void>;
  using emscripten::val;
  const auto visible = image["visibleRect"];
  if (visible.isNull() || visible.isUndefined() || visible["width"].as<unsigned>() != format.width ||
      visible["height"].as<unsigned>() != format.height || image["displayWidth"].as<unsigned>() != format.width ||
      image["displayHeight"].as<unsigned>() != format.height ||
      format.visible != VideoRectangle{0, 0, format.width, format.height} || format.aspect_width != format.aspect_height ||
      format.rotation != VideoRotation::None)
    return R::Failure(Error(CodecErrorCode::InvalidArgument, "Browser frame geometry differs from the video format"));
  const auto rotation = image["rotation"], flip = image["flip"];
  if ((!rotation.isUndefined() && rotation.as<unsigned>() != 0) || (!flip.isUndefined() && flip.as<bool>()))
    return R::Failure(Error(CodecErrorCode::Unsupported, "Browser frames must have their orientation applied"));
  const auto layout = image["format"];
  if (!layout.isNull() && !layout.isUndefined()) {
    const auto name = layout.as<std::string>();
    if (name.ends_with("P10") || name.ends_with("P12"))
      return R::Failure(Error(CodecErrorCode::Unsupported, "Browser frame input requires eight-bit video"));
  }
  const auto color = image["colorSpace"], transfer = color["transfer"];
  if (color["primaries"].equals(val("bt2020")) || color["matrix"].equals(val("bt2020-ncl")) ||
      transfer.equals(val("pq")) || transfer.equals(val("hlg")))
    return R::Failure(Error(CodecErrorCode::Unsupported, "Browser frame input requires SDR color"));
  return R::Success();
}

CodecResult<VideoFrame> AcquirePlatformVideoFrame(const ExternalTexture& texture, VideoFormat format,
    MediaTimestamp pts, std::optional<MediaTimestamp> duration) {
  using R = CodecResult<VideoFrame>;
  if (!emscripten_is_main_browser_thread())
    return R::Failure(Error(CodecErrorCode::Unsupported, "Web video textures require the browser main thread"));
  const auto* source = dynamic_cast<const web::VideoFrameTexture*>(&texture);
  if (!source) return R::Failure(Error(CodecErrorCode::Unsupported, "Expected an SDK VideoFrame texture"));
  BrowserVideoFrame image(source->AcquireFrame());
  if (image.value.isUndefined()) return R::Failure(Error(CodecErrorCode::InvalidState, "The texture has no published video frame"));
  if (!image) return R::Failure(Error(CodecErrorCode::PlatformFailure, "Cannot acquire the browser video frame"));
  const auto valid = ValidateBrowserFrame(image.value, format);
  if (!valid.Succeeded()) return R::Failure(valid.Error());
  return R::Success(RetainBrowserVideoFrame(std::move(image), format, pts, duration));
}

CodecResult<std::shared_ptr<ExternalTexture>> MakePlatformVideoTexture(const VideoFrame& frame) {
  using R = CodecResult<std::shared_ptr<ExternalTexture>>;
  if (!emscripten_is_main_browser_thread())
    return R::Failure(Error(CodecErrorCode::Unsupported, "Web video textures require the browser main thread"));
  const auto* source = BrowserFrame(frame);
  if (!source) return R::Failure(Error(CodecErrorCode::Unsupported, "Expected a WebCodecs video frame"));

  struct TextureLease {
    TextureLease(const VideoFrame& frame, Size size)
        : source(VideoFrameAccess::PlatformStorage(frame)), texture(size) {}
    std::shared_ptr<const PlatformVideoStorage> source;
    web::VideoFrameTexture texture;
  };
  auto lease = std::make_shared<TextureLease>(frame, Size{(*source)["displayWidth"].as<float>(), (*source)["displayHeight"].as<float>()});
  lease->texture.Publish(*source);
  lease->texture.Finish();
  return R::Success(std::shared_ptr<ExternalTexture>(lease, &lease->texture));
}

} // namespace huxerui::media::detail
