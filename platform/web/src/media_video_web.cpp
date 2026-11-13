#include "media_video_web.h"

#include "codec/common.h"
#include "format/platform_video_storage.h"
#include "format/video_frame_transformer.h"

#include <huxerui/web/external_texture.h>
#include <emscripten.h>
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

namespace {

using emscripten::EM_VAL;

EM_JS(EM_VAL, RenderVideoTexture, (EM_VAL source, double timestamp, int width, int height, int rotation,
    double crop_x, double crop_y, double crop_width, double crop_height, double x, double y, double w, double h), {
  try {
    const frame = Emval.toValue(source);
    const canvas = typeof OffscreenCanvas === 'function' ? new OffscreenCanvas(width, height)
        : Object.assign(document.createElement('canvas'), {width, height});
    const context = canvas.getContext('2d', {alpha: false});
    if (!context) return Emval.toHandle('Canvas 2D is unavailable');
    context.fillStyle = 'black';
    context.fillRect(0, 0, width, height);
    context.translate(x + w / 2, y + h / 2);
    context.rotate(rotation * Math.PI / 180);
    if (rotation === 90 || rotation === 270) [w, h] = [h, w];
    context.drawImage(frame, crop_x, crop_y, crop_width, crop_height, -w / 2, -h / 2, w, h);
    return Emval.toHandle(new VideoFrame(canvas, {timestamp}));
  } catch (error) {
    return Emval.toHandle(String(error));
  }
});

EM_JS(EM_VAL, UploadVideoTexture, (const void* pixels, int width, int height), {
  try {
    const bytes = HEAPU8.subarray(pixels, pixels + width * height * 4);
    return Emval.toHandle(new VideoFrame(bytes, {format: 'RGBA', codedWidth: width, codedHeight: height, timestamp: 0}));
  } catch (error) {
    return Emval.toHandle(String(error));
  }
});

VideoTextureResult PublishTextureFrame(emscripten::val value, VideoTextureOptions options) {
  if (value.typeOf().as<std::string>() == "string")
    return VideoTextureResult::Failure(Error(CodecErrorCode::PlatformFailure, value.as<std::string>()));
  BrowserVideoFrame frame(std::move(value));
  auto texture = std::make_shared<web::VideoFrameTexture>(Size{float(options.width), float(options.height)});
  texture->Publish(frame.value);
  texture->Finish();
  return VideoTextureResult::Success(std::move(texture));
}

} // namespace

VideoTextureResult MakeFittedPlatformVideoTexture(const VideoFrame& frame,
    VideoTextureOptions options, const VideoTextureGeometry& geometry) {
  const auto* image = BrowserFrame(frame);
  if (!image || !emscripten_is_main_browser_thread())
    return VideoTextureResult::Failure(Error(CodecErrorCode::Unsupported, "Expected a browser video frame on the main thread"));
  const auto format = (*image)["format"];
  if (!format.isNull() && !format.isUndefined()) {
    const auto layout = format.as<std::string>();
    if (layout.ends_with("P10") || layout.ends_with("P12"))
      return VideoTextureResult::Failure(Error(CodecErrorCode::Unsupported, "Expected an eight-bit browser video frame"));
  }
  if (geometry.identity && ValidateBrowserFrame(*image, frame.format).Succeeded() &&
      (*image)["codedWidth"].as<unsigned>() == options.width && (*image)["codedHeight"].as<unsigned>() == options.height)
    return MakePlatformVideoTexture(frame);
  const auto native_rotation = (*image)["rotation"];
  const auto flipped = (*image)["flip"];
  if ((!native_rotation.isUndefined() && native_rotation.as<int>() != 0) ||
      (!flipped.isUndefined() && flipped.as<bool>()))
    return VideoTextureResult::Failure(Error(CodecErrorCode::Unsupported, "Expected a browser frame in coded orientation"));
  const auto crop = VideoSourceRectangle(frame.format, geometry.source);
  const auto visible = (*image)["visibleRect"];
  const auto left = visible["x"].as<unsigned>(), top = visible["y"].as<unsigned>();
  if (crop.x < left || crop.y < top || crop.x + crop.width > left + visible["width"].as<unsigned>() ||
      crop.y + crop.height > top + visible["height"].as<unsigned>())
    return VideoTextureResult::Failure(Error(CodecErrorCode::InvalidArgument, "The crop is outside the decoded browser image"));
  const double sx = (*image)["displayWidth"].as<double>() / visible["width"].as<double>();
  const double sy = (*image)["displayHeight"].as<double>() / visible["height"].as<double>();
  const auto& area = geometry.destination;
  int rotation = 0;
  switch (frame.format.rotation) {
    case VideoRotation::None: break;
    case VideoRotation::Clockwise90: rotation = 90; break;
    case VideoRotation::Clockwise180: rotation = 180; break;
    case VideoRotation::Clockwise270: rotation = 270; break;
  }
  return PublishTextureFrame(emscripten::val::take_ownership(RenderVideoTexture(image->as_handle(),
      (*image)["timestamp"].as<double>(), options.width, options.height, rotation,
      (crop.x - left) * sx, (crop.y - top) * sy, crop.width * sx, crop.height * sy,
      area.x, area.y, area.width, area.height)), options);
}

VideoTextureResult PublishVideoPixels(Bytes pixels, VideoTextureOptions options) {
  return PublishTextureFrame(emscripten::val::take_ownership(UploadVideoTexture(pixels.data(), options.width, options.height)), options);
}

} // namespace huxerui::media::detail
