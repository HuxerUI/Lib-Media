#pragma once
#include "backend.h"
#include <cstring>
#include <algorithm>
#include <cmath>
#include <numeric>

namespace huxerui::media::detail {
inline CodecResult<AudioFrame> CopyAudio(const AudioFrame& source, AudioFormat requested) {
  if (!source.Validate() || source.format.sample_rate != requested.sample_rate
      || source.format.channel_layout != requested.channel_layout)
    return CodecResult<AudioFrame>::Failure(Error(CodecErrorCode::InvalidData, "Invalid native PCM format"));
  const auto channels = requested.ChannelCount(), bytes = requested.BytesPerSample();
  const auto count = requested.packing == AudioPacking::Planar ? channels : 1;
  std::vector<Bytes> storage(count, Bytes(std::size_t(source.sample_count) * channels * bytes / count));
  for (std::size_t sample = 0; sample < source.sample_count; ++sample) {
    for (unsigned channel = 0; channel < channels; ++channel) {
      const auto planar = source.format.packing == AudioPacking::Planar;
      const auto input = source.planes[planar ? channel : 0].AsBytes().data()
          + (planar ? sample : sample * channels + channel) * source.format.BytesPerSample();
      float value;
      if (source.format.sample_format == AudioSampleFormat::F32) std::memcpy(&value, input, sizeof(value));
      else { std::int16_t integer; std::memcpy(&integer, input, sizeof(integer)); value = integer / 32768.0f; }
      auto* output = storage[count == 1 ? 0 : channel].data() + (count == 1 ? sample * channels + channel : sample) * bytes;
      if (requested.sample_format == AudioSampleFormat::F32) std::memcpy(output, &value, sizeof(value));
      else {
        const auto integer = static_cast<std::int16_t>(std::clamp(std::isfinite(value) ? std::round(value * 32768.0f) : 0.0f, -32768.0f, 32767.0f));
        std::memcpy(output, &integer, sizeof(integer));
      }
    }
  }
  AudioFrame result{requested, source.pts, source.sample_count, {}};
  for (auto& plane : storage) result.planes.push_back(Own(std::move(plane)));
  return CodecResult<AudioFrame>::Success(std::move(result));
}
inline CodecResult<VideoFrame> CopyVideo(VideoFormat format, MediaTimestamp pts,
    std::optional<MediaTimestamp> duration, std::vector<VideoPlane> planes, VideoPixelFormat requested) {
  VideoFrame source{format, pts, duration, std::move(planes)};
  if (!source.Validate()) return CodecResult<VideoFrame>::Failure(Error(CodecErrorCode::InvalidData, "Invalid native video planes"));
  format.pixel_format = requested;
  const auto aspect_divisor = std::gcd(format.aspect_width, format.aspect_height);
  format.aspect_width /= aspect_divisor;
  format.aspect_height /= aspect_divisor;
  VideoFrame output{format, pts, duration, {}};
  const auto cw = (std::size_t(format.width) + 1) / 2, ch = (std::size_t(format.height) + 1) / 2;
  for (unsigned p = 0; p < (requested == VideoPixelFormat::I420 ? 3u : 2u); ++p) {
    const auto width = p ? cw : format.width, height = p ? ch : format.height;
    const std::size_t components = p && requested == VideoPixelFormat::NV12 ? 2 : 1;
    Bytes bytes(width * height * components);
    for (std::size_t y = 0; y < height; ++y) {
      for (std::size_t x = 0; x < width; ++x) {
        for (std::size_t c = 0; c < components; ++c) {
          const auto channel = p ? requested == VideoPixelFormat::I420 ? p - 1 : c : 0;
          const auto source_plane = !p ? 0 : source.format.pixel_format == VideoPixelFormat::I420 ? channel + 1 : 1;
          const auto& plane = source.planes[source_plane];
          const auto component = p && source.format.pixel_format == VideoPixelFormat::NV12 ? channel : 0;
          bytes[(y * width + x) * components + c] = plane.data.AsBytes()[y * plane.row_stride + x * plane.pixel_stride + component];
        }
      }
    }
    output.planes.push_back({Own(std::move(bytes)), width * components, components});
  }
  return CodecResult<VideoFrame>::Success(std::move(output));
}
} // namespace huxerui::media::detail
