#include "copy.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <numeric>
#include <type_traits>

namespace huxerui::media::detail {
CodecResult<AudioFrame> CopyAudio(const AudioFrame& source, AudioFormat requested) {
  if (!source.Validate() || source.format.sample_rate != requested.sample_rate
      || source.format.channel_layout != requested.channel_layout)
    return CodecResult<AudioFrame>::Failure(Error(CodecErrorCode::InvalidData, "Invalid platform PCM format"));
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
CodecResult<VideoFrame> CopyVideo(VideoFormat format, MediaTimestamp pts,
    std::optional<MediaTimestamp> duration, std::vector<VideoPlane> planes, VideoPixelFormat requested) {
  VideoFrame source{format, pts, duration, std::move(planes)};
  if (!source.Validate()) return CodecResult<VideoFrame>::Failure(Error(CodecErrorCode::InvalidData, "Invalid platform video planes"));
  if ((format.pixel_format == VideoPixelFormat::P010) != (requested == VideoPixelFormat::P010))
    return CodecResult<VideoFrame>::Failure(Error(CodecErrorCode::Unsupported, "Video bit-depth conversion is unavailable"));
  format.pixel_format = requested;
  const auto aspect_divisor = std::gcd(format.aspect_width, format.aspect_height);
  format.aspect_width /= aspect_divisor;
  format.aspect_height /= aspect_divisor;
  VideoFrame output{format, pts, duration, {}};
  const auto cw = (std::size_t(format.width) + 1) / 2, ch = (std::size_t(format.height) + 1) / 2;
  const std::size_t component_bytes = requested == VideoPixelFormat::P010 ? 2 : 1;
  for (unsigned p = 0; p < (requested == VideoPixelFormat::I420 ? 3u : 2u); ++p) {
    const auto width = p ? cw : format.width, height = p ? ch : format.height;
    const std::size_t components = p && requested != VideoPixelFormat::I420 ? 2 : 1;
    const auto pixel_bytes = components * component_bytes;
    Bytes bytes(width * height * pixel_bytes);
    for (std::size_t y = 0; y < height; ++y) {
      for (std::size_t x = 0; x < width; ++x) {
        for (std::size_t c = 0; c < components; ++c) {
          const auto channel = p ? requested == VideoPixelFormat::I420 ? p - 1 : c : 0;
          const auto source_plane = !p ? 0 : source.format.pixel_format == VideoPixelFormat::I420 ? channel + 1 : 1;
          const auto& plane = source.planes[source_plane];
          const auto component = p && source.format.pixel_format != VideoPixelFormat::I420 ? channel : 0;
          const auto at = y * plane.row_stride + x * plane.pixel_stride + component * component_bytes;
          const auto to = (y * width + x) * pixel_bytes + c * component_bytes;
          std::copy_n(plane.data.AsBytes().data() + at, component_bytes, bytes.data() + to);
        }
      }
    }
    output.planes.push_back({Own(std::move(bytes)), width * pixel_bytes, pixel_bytes});
  }
  return CodecResult<VideoFrame>::Success(std::move(output));
}
void RetainFrame(DecodedFrame& frame, const std::shared_ptr<const void>& credit) {
  struct Owner { BufferReference bytes; std::shared_ptr<const void> credit; };
  auto wrap = [&](BufferReference& bytes) {
    auto owner = std::make_shared<Owner>(Owner{bytes, credit});
    bytes = BufferReference(owner->bytes.AsBytes(), owner);
  };
  std::visit([&](auto& value) {
    for (auto& plane : value.planes) {
      if constexpr (std::is_same_v<std::decay_t<decltype(plane)>, BufferReference>) wrap(plane);
      else wrap(plane.data);
    }
  }, frame);
}
std::size_t FrameBytes(const DecodedFrame& frame) {
  return std::visit([](const auto& value) {
    std::size_t result = 0;
    for (const auto& plane : value.planes) {
      const auto size = [&] {
        if constexpr (std::is_same_v<std::decay_t<decltype(plane)>, BufferReference>) return plane.AsBytes().size();
        else return plane.data.AsBytes().size();
      }();
      if (size > std::numeric_limits<std::size_t>::max() - result) return std::numeric_limits<std::size_t>::max();
      result += size;
    }
    return result;
  }, frame);
}
} // namespace huxerui::media::detail
