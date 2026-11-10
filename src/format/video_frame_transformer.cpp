#include "video_frame_transformer.h"
#include "platform_video_storage.h"
#include "codec/common.h"

namespace huxerui::media::detail {

VideoSampling VideoSampleCoordinates(const VideoFormat& format) {
  const auto rect = format.visible;
  const auto x = float(rect.x) / float(format.width), y = float(rect.y) / float(format.height);
  const auto w = float(rect.width) / float(format.width), h = float(rect.height) / float(format.height);
  switch (format.rotation) {
    case VideoRotation::Clockwise90: return {{{x, y + h}, {0, -h}, {w, 0}}};
    case VideoRotation::Clockwise180: return {{{x + w, y + h}, {-w, 0}, {0, -h}}};
    case VideoRotation::Clockwise270: return {{{x + w, y}, {0, h}, {-w, 0}}};
    case VideoRotation::None: return {{{x, y}, {w, 0}, {0, h}}};
  }
  return {};
}

#if !defined(__ANDROID__) && !defined(_WIN32) && !(defined(__APPLE__) && (TARGET_OS_OSX || TARGET_OS_IOS))
std::unique_ptr<VideoFrameTransformBackend> MakeVideoFrameTransformBackend() { return {}; }

#ifndef __EMSCRIPTEN__
CodecResult<std::shared_ptr<ExternalTexture>> MakePlatformVideoTexture(const VideoFrame&) {
  return CodecResult<std::shared_ptr<ExternalTexture>>::Failure(Error(CodecErrorCode::Unsupported, "Platform video textures are unavailable"));
}
#endif
#endif

#if !defined(__ANDROID__) && !defined(__EMSCRIPTEN__) && !defined(_WIN32) && !(defined(__APPLE__) && (TARGET_OS_OSX || TARGET_OS_IOS))
CodecResult<VideoFrame> AcquirePlatformVideoFrame(const ExternalTexture&, VideoFormat,
    MediaTimestamp, std::optional<MediaTimestamp>) {
  return CodecResult<VideoFrame>::Failure(Error(CodecErrorCode::Unsupported, "SDK texture input is unavailable"));
}
#endif

} // namespace huxerui::media::detail

namespace huxerui::media {
namespace {

bool SupportsPlatformSdr(const VideoFormat& format) {
  return detail::PlatformVideoSupported && format.pixel_format == detail::PlatformVideoLayout &&
      format.primaries != ColorPrimaries::BT2020 && format.matrix != ColorMatrix::BT2020 &&
      format.transfer != ColorTransfer::PQ && format.transfer != ColorTransfer::HLG;
}

} // namespace

CodecResult<VideoFrame> MakeVideoFrame(std::shared_ptr<const ExternalTexture> texture, VideoFormat format,
    MediaTimestamp pts, std::optional<MediaTimestamp> duration) {
  using R = CodecResult<VideoFrame>;
  if (!texture || !format.Validate() || !pts.timescale || (duration && (!duration->timescale || duration->ticks <= 0)))
    return R::Failure(detail::Error(CodecErrorCode::InvalidArgument, "Invalid video texture, format, or timing"));
  if (!SupportsPlatformSdr(format))
    return R::Failure(detail::Error(CodecErrorCode::Unsupported, "Video texture input requires a supported SDR layout"));
  try {
    auto frame = detail::AcquirePlatformVideoFrame(*texture, format, pts, duration);
    if (frame.Succeeded()) detail::VideoFrameAccess::RetainPlatformStorage(frame.Value(), texture);
    return frame;
  } catch (const std::bad_alloc&) {
    return R::Failure(detail::Error(CodecErrorCode::ResourceExhausted, "Cannot retain the texture's video frame"));
  } catch (const std::exception& error) {
    return R::Failure(detail::Error(CodecErrorCode::PlatformFailure, error.what()));
  }
}

CodecResult<std::shared_ptr<ExternalTexture>> MakeExternalTexture(const VideoFrame& frame) {
  using R = CodecResult<std::shared_ptr<ExternalTexture>>;
  if (frame.Storage() != VideoStorage::Platform || !frame.Validate())
    return R::Failure(detail::Error(CodecErrorCode::InvalidArgument, "Expected a valid platform video frame"));
  if (!SupportsPlatformSdr(frame.format))
    return R::Failure(detail::Error(CodecErrorCode::Unsupported, "Video textures require a supported SDR layout"));
  try {
    auto coded = frame;
    coded.format.visible = {0, 0, coded.format.width, coded.format.height};
    coded.format.rotation = VideoRotation::None;
    return detail::MakePlatformVideoTexture(coded);
  } catch (const std::bad_alloc&) {
    return R::Failure(detail::Error(CodecErrorCode::ResourceExhausted, "Cannot allocate a video texture"));
  } catch (const std::exception& error) {
    return R::Failure(detail::Error(CodecErrorCode::PlatformFailure, error.what()));
  }
}

CodecResult<VideoFrame> detail::VideoFrameTransformer::Transform(const VideoFrame& frame, std::uint32_t width, std::uint32_t height) {
  using R = CodecResult<VideoFrame>;
  if (frame.Storage() != VideoStorage::Platform || !frame.Validate() || !width || !height ||
      width > 8192 || height > 8192 || ((width | height) & 1))
    return R::Failure(Error(CodecErrorCode::InvalidArgument, "GPU video transforms require a valid platform frame and even output dimensions"));
  auto format = frame.format;
  if (!SupportsPlatformSdr(format))
    return R::Failure(Error(CodecErrorCode::Unsupported, "GPU video transforms require a supported SDR layout"));
  if (format.rotation == VideoRotation::Clockwise90 || format.rotation == VideoRotation::Clockwise270)
    std::swap(format.aspect_width, format.aspect_height);
  format.rotation = VideoRotation::None;
  format.width = width;
  format.height = height;
  format.visible = {0, 0, width, height};
  try {
    if (!backend_) backend_ = MakeVideoFrameTransformBackend();
    if (!backend_) return R::Failure(Error(CodecErrorCode::Unsupported, "GPU video transforms are unavailable"));
    return backend_->Transform(frame, format);
  } catch (const std::bad_alloc&) {
    return R::Failure(Error(CodecErrorCode::ResourceExhausted, "Cannot allocate platform video storage"));
  } catch (const std::exception& error) {
    return R::Failure(Error(CodecErrorCode::PlatformFailure, error.what()));
  }
}

} // namespace huxerui::media
