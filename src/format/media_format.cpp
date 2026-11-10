#include <huxerui/media_format.h>

#include <limits>

namespace huxerui::media {
namespace {
std::uint64_t Magnitude(std::int64_t value) noexcept {
  return value < 0 ? std::uint64_t(-(value + 1)) + 1 : std::uint64_t(value);
}
bool ValidTime(MediaTimestamp value) { return value.timescale != 0; }
bool ValidDuration(const std::optional<MediaTimestamp>& value) {
  return !value || (ValidTime(*value) && value->ticks >= 0);
}
bool PlaneFits(const VideoPlane& plane, std::size_t width, std::size_t height, std::size_t sample_bytes) {
  if (!width || !height || plane.pixel_stride < sample_bytes) return false;
  const auto limit = std::numeric_limits<std::size_t>::max();
  if (width - 1 > (limit - sample_bytes) / plane.pixel_stride) return false;
  const auto row_bytes = (width - 1) * plane.pixel_stride + sample_bytes;
  if (plane.row_stride < row_bytes || height - 1 > (limit - row_bytes) / plane.row_stride) return false;
  return plane.data.AsBytes().size() >= (height - 1) * plane.row_stride + row_bytes;
}
} // namespace

std::optional<MediaTimestamp> MediaTimestamp::Rescale(std::uint32_t scale) const noexcept {
  if (!timescale || !scale) return std::nullopt;
  const auto magnitude = Magnitude(ticks);
  const auto whole = magnitude / timescale;
  const auto fraction = (magnitude % timescale) * scale;
  auto rounded = fraction / timescale;
  if (fraction % timescale >= (std::uint64_t(timescale) + 1) / 2) ++rounded;
  const auto maximum = std::uint64_t(std::numeric_limits<std::int64_t>::max()) + (ticks < 0 ? 1 : 0);
  if (rounded > maximum || whole > (maximum - rounded) / scale) return std::nullopt;
  const auto total = whole * scale + rounded;
  const auto result = ticks >= 0 ? std::int64_t(total)
      : total == (std::uint64_t(1) << 63) ? std::numeric_limits<std::int64_t>::min() : -std::int64_t(total);
  return MediaTimestamp{result, scale};
}

std::optional<int> MediaTimestamp::Compare(MediaTimestamp other) const noexcept {
  if (!timescale || !other.timescale) return std::nullopt;
  if ((ticks < 0) != (other.ticks < 0)) return ticks < 0 ? -1 : 1;
  const auto a = Magnitude(ticks), b = Magnitude(other.ticks);
  const auto aw = a / timescale, bw = b / other.timescale;
  const auto ar = (a % timescale) * other.timescale, br = (b % other.timescale) * timescale;
  const auto comparison = aw != bw ? (aw < bw ? -1 : 1) : ar == br ? 0 : ar < br ? -1 : 1;
  return ticks < 0 ? -comparison : comparison;
}

std::uint32_t AudioFormat::ChannelCount() const noexcept {
  switch (channel_layout) {
    case AudioChannelLayout::Mono: return 1;
    case AudioChannelLayout::Stereo: return 2;
  }
  return 0;
}
std::uint32_t AudioFormat::BytesPerSample() const noexcept {
  switch (sample_format) {
    case AudioSampleFormat::S16: return 2;
    case AudioSampleFormat::F32: return 4;
  }
  return 0;
}
bool AudioFrame::Validate() const noexcept {
  const auto channels = format.ChannelCount(), bytes = format.BytesPerSample();
  if (!ValidTime(pts) || !format.sample_rate || !channels || !bytes || !sample_count) return false;
  if (format.packing != AudioPacking::Interleaved && format.packing != AudioPacking::Planar) return false;
  const auto plane_count = format.packing == AudioPacking::Planar ? channels : 1;
  if (planes.size() != plane_count) return false;
  const auto stride = bytes * channels / plane_count;
  if (sample_count > std::numeric_limits<std::size_t>::max() / stride) return false;
  for (const auto& plane : planes) {
    const auto data = plane.AsBytes();
    if (data.size() < std::size_t(sample_count) * stride || reinterpret_cast<std::uintptr_t>(data.data()) % bytes)
      return false;
  }
  return true;
}
bool VideoFormat::Validate() const noexcept {
  return width && height && visible.width && visible.height && visible.x < width && visible.y < height
      && visible.width <= width - visible.x && visible.height <= height - visible.y
      && aspect_width && aspect_height && (pixel_format == VideoPixelFormat::I420 ||
          pixel_format == VideoPixelFormat::NV12 || pixel_format == VideoPixelFormat::P010)
      && primaries >= ColorPrimaries::Unknown && primaries <= ColorPrimaries::BT2020
      && transfer >= ColorTransfer::Unknown && transfer <= ColorTransfer::HLG
      && matrix >= ColorMatrix::Unknown && matrix <= ColorMatrix::BT2020
      && range >= ColorRange::Unknown && range <= ColorRange::Full
      && rotation >= VideoRotation::None && rotation <= VideoRotation::Clockwise270;
}
bool VideoFrame::Validate() const noexcept {
  if (!format.Validate() || !ValidTime(pts) || !ValidDuration(duration)) return false;
  const auto planar = format.pixel_format == VideoPixelFormat::I420;
  const auto bytes = format.pixel_format == VideoPixelFormat::P010 ? 2u : 1u;
  if (planes.size() != (planar ? 3 : 2)) return false;
  if (!PlaneFits(planes[0], format.width, format.height, bytes)) return false;
  const auto cw = std::size_t(format.width) / 2 + format.width % 2;
  const auto ch = std::size_t(format.height) / 2 + format.height % 2;
  return PlaneFits(planes[1], cw, ch, planar ? 1 : 2 * bytes) && (!planar || PlaneFits(planes[2], cw, ch, 1));
}
} // namespace huxerui::media
