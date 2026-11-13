#include "video_frame_transformer.h"
#include "codec/common.h"

#include <algorithm>
#include <cmath>

namespace huxerui::media::detail {

VideoTextureGeometry FitVideoTexture(const VideoFormat& format, VideoTextureOptions options) {
  const bool rotated = format.rotation == VideoRotation::Clockwise90 || format.rotation == VideoRotation::Clockwise270;
  double width = double(format.visible.width) * format.aspect_width / format.aspect_height;
  double height = format.visible.height;
  if (rotated) std::swap(width, height);
  const double width_scale = options.width / width;
  const double height_scale = options.height / height;
  VideoTextureGeometry geometry{{0, 0, 1, 1}, {0, 0, float(options.width), float(options.height)},
      format.rotation == VideoRotation::None && format.aspect_width == format.aspect_height &&
          format.visible == VideoRectangle{0, 0, format.width, format.height} &&
          format.width == options.width && format.height == options.height};
  if (options.fit == ImageFit::Cover) {
    const auto scale = std::max(width_scale, height_scale);
    geometry.source.width = float(width_scale / scale);
    geometry.source.height = float(height_scale / scale);
    geometry.source.x = (1 - geometry.source.width) / 2;
    geometry.source.y = (1 - geometry.source.height) / 2;
  } else {
    const auto scale = std::min(width_scale, height_scale);
    const auto content_width = std::clamp(std::lround(width * scale), 1L, long(options.width));
    const auto content_height = std::clamp(std::lround(height * scale), 1L, long(options.height));
    geometry.destination = {float((options.width - content_width) / 2), float((options.height - content_height) / 2),
        float(content_width), float(content_height)};
  }
  return geometry;
}

VideoSampling VideoSampleCoordinates(const VideoFormat& format, Rect source) {
  auto uv = VideoSampleCoordinates(format);
  for (std::size_t axis = 0; axis < 2; ++axis) {
    uv[0][axis] += source.x * uv[1][axis] + source.y * uv[2][axis];
    uv[1][axis] *= source.width;
    uv[2][axis] *= source.height;
  }
  return uv;
}

VideoRectangle VideoSourceRectangle(const VideoFormat& format, Rect source) {
  const auto uv = VideoSampleCoordinates(format, source);
  const auto bounds = [&](std::size_t axis, unsigned size) {
    const auto first = uv[0][axis], last = first + uv[1][axis] + uv[2][axis];
    const auto begin = std::clamp(std::lround(std::min(first, last) * size), 0L, long(size - 1));
    const auto end = std::clamp(std::lround(std::max(first, last) * size), begin + 1, long(size));
    return std::pair{unsigned(begin), unsigned(end - begin)};
  };
  const auto [x, width] = bounds(0, format.width);
  const auto [y, height] = bounds(1, format.height);
  return {x, y, width, height};
}

namespace {

Bytes RasterizeVideoTexture(const VideoFrame& frame, VideoTextureOptions options, const VideoTextureGeometry& geometry) {
  const auto& format = frame.format;
  const auto uv = VideoSampleCoordinates(format, geometry.source);
  const auto& area = geometry.destination;
  Bytes pixels(std::size_t(options.width) * options.height * 4);
  for (std::size_t i = 3; i < pixels.size(); i += 4) pixels[i] = std::byte{255};
  const bool planar = format.pixel_format == VideoPixelFormat::I420;
  const bool full = format.range == ColorRange::Full;
  const double kr = format.matrix == ColorMatrix::BT709 ? 0.2126 : 0.299;
  const double kb = format.matrix == ColorMatrix::BT709 ? 0.0722 : 0.114;
  const auto sample = [](const VideoPlane& plane, double x, double y, unsigned channel, VideoRectangle bounds) {
    x = std::clamp(x, double(bounds.x), double(bounds.x + bounds.width - 1));
    y = std::clamp(y, double(bounds.y), double(bounds.y + bounds.height - 1));
    const auto x0 = unsigned(x), y0 = unsigned(y);
    const auto x1 = std::min(x0 + 1, bounds.x + bounds.width - 1);
    const auto y1 = std::min(y0 + 1, bounds.y + bounds.height - 1);
    const auto read = [&](unsigned column, unsigned row) {
      return std::to_integer<unsigned>(plane.data.AsBytes()[row * plane.row_stride + column * plane.pixel_stride + channel]);
    };
    return std::lerp(std::lerp(double(read(x0, y0)), read(x1, y0), x - x0),
        std::lerp(double(read(x0, y1)), read(x1, y1), x - x0), y - y0);
  };
  const auto visible = format.visible;
  const VideoRectangle chroma{visible.x / 2, visible.y / 2,
      (visible.x + visible.width + 1) / 2 - visible.x / 2, (visible.y + visible.height + 1) / 2 - visible.y / 2};
  const auto byte = [](double value) { return std::byte(std::lround(std::clamp(value, 0., 1.) * 255)); };
  for (unsigned y = 0; y < unsigned(area.height); ++y) {
    for (unsigned x = 0; x < unsigned(area.width); ++x) {
      const double dx = (x + 0.5) / area.width, dy = (y + 0.5) / area.height;
      const auto sx = (uv[0][0] + dx * uv[1][0] + dy * uv[2][0]) * format.width;
      const auto sy = (uv[0][1] + dx * uv[1][1] + dy * uv[2][1]) * format.height;
      const auto luma = (sample(frame.planes[0], sx - 0.5, sy - 0.5, 0, visible) - (full ? 0 : 16)) / (full ? 255 : 219);
      const auto u = (sample(frame.planes[1], sx / 2 - 0.5, sy / 2 - 0.5, 0, chroma) - 128) / (full ? 255 : 224);
      const auto v = (sample(frame.planes[planar ? 2 : 1], sx / 2 - 0.5, sy / 2 - 0.5, planar ? 0 : 1, chroma) - 128) /
          (full ? 255 : 224);
      const auto red = luma + 2 * (1 - kr) * v, blue = luma + 2 * (1 - kb) * u;
      const auto offset = (std::size_t(y + unsigned(area.y)) * options.width + x + unsigned(area.x)) * 4;
      pixels[offset] = byte(red);
      pixels[offset + 1] = byte((luma - kr * red - kb * blue) / (1 - kr - kb));
      pixels[offset + 2] = byte(blue);
    }
  }
  return pixels;
}
} // namespace

#if defined(__linux__) && !defined(__ANDROID__) && !defined(__EMSCRIPTEN__)
VideoTextureResult MakeFittedPlatformVideoTexture(const VideoFrame&, VideoTextureOptions, const VideoTextureGeometry&) {
  return VideoTextureResult::Failure(Error(CodecErrorCode::Unsupported, "Platform video output is unavailable"));
}
#endif

} // namespace huxerui::media::detail

namespace huxerui::media {

CodecResult<std::shared_ptr<ExternalTexture>> MakeExternalTexture(const VideoFrame& frame, VideoTextureOptions options) {
  using R = CodecResult<std::shared_ptr<ExternalTexture>>;
  const auto& format = frame.format;
  if (!frame.Validate() || !options.width || !options.height || options.width > 8192 || options.height > 8192 ||
      (options.fit != ImageFit::Contain && options.fit != ImageFit::Cover))
    return R::Failure(detail::Error(CodecErrorCode::InvalidArgument, "Invalid video frame or texture options"));
  if (format.primaries == ColorPrimaries::BT2020 || format.matrix == ColorMatrix::BT2020 ||
      format.transfer == ColorTransfer::PQ || format.transfer == ColorTransfer::HLG ||
      (frame.Storage() == VideoStorage::Cpu && format.pixel_format != VideoPixelFormat::NV12 &&
          format.pixel_format != VideoPixelFormat::I420))
    return R::Failure(detail::Error(CodecErrorCode::Unsupported, "Video textures require eight-bit SDR"));

  try {
    const auto geometry = detail::FitVideoTexture(format, options);
    if (frame.Storage() == VideoStorage::Platform)
      return detail::MakeFittedPlatformVideoTexture(frame, options, geometry);
    return detail::PublishVideoPixels(detail::RasterizeVideoTexture(frame, options, geometry), options);
  } catch (const std::bad_alloc&) {
    return R::Failure(detail::Error(CodecErrorCode::ResourceExhausted, "Cannot allocate a video texture"));
  } catch (const std::exception& error) {
    return R::Failure(detail::Error(CodecErrorCode::PlatformFailure, error.what()));
  }
}

} // namespace huxerui::media
