#include "format.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace huxerui::media::detail {
CodecResult<void> ValidateAudioEncoderConfig(const AudioEncoderConfig& config) {
  const auto channels = config.input.ChannelCount();
  const auto& limits = config.limits;
  if (!channels || !config.input.sample_rate || !config.bitrate || config.bitrate > std::numeric_limits<std::int32_t>::max() ||
      !limits.input_items || limits.input_bytes < channels * 2 || limits.output_items < 16 ||
      limits.output_bytes < 16 * kAudioEncoderPacketBytes)
    return CodecResult<void>::Failure(Error(CodecErrorCode::InvalidArgument, "Invalid audio encoder configuration or queue limits"));
  if (config.input.sample_format != AudioSampleFormat::S16 || config.input.packing != AudioPacking::Interleaved ||
      (config.input.sample_rate != 44100 && config.input.sample_rate != 48000))
    return CodecResult<void>::Failure(Error(CodecErrorCode::Unsupported, "AAC encoding requires interleaved S16 mono/stereo at 44100 or 48000 Hz"));
  return CodecResult<void>::Success();
}
AudioEncodeFormat DescribeAac(const AudioEncoderConfig& config) {
  const auto index = config.input.sample_rate == 48000 ? 3u : 4u;
  const auto asc = (2u << 11) | (index << 7) | (config.input.ChannelCount() << 3);
  return {config.input.sample_rate, config.input.channel_layout, {std::byte(asc >> 8), std::byte(asc & 255)}};
}
CodecResult<void> ValidateAudioEncodeFormat(const AudioEncodeFormat& format, const AudioEncoderConfig& config) {
  auto parsed = ParseAac(format.initialization_data);
  if (!parsed.Succeeded() || format.sample_rate != config.input.sample_rate || format.channel_layout != config.input.channel_layout ||
      parsed.Value().sample_rate != config.input.sample_rate || parsed.Value().channels != config.input.ChannelCount())
    return CodecResult<void>::Failure(Error(CodecErrorCode::Unsupported, "Platform encoder changed the AAC-LC stream format"));
  return CodecResult<void>::Success();
}
CodecResult<AudioEncodeFormat> ReadAudioEncodeFormat(std::span<const std::byte> description, const AudioEncoderConfig& config) {
  using R = CodecResult<AudioEncodeFormat>;
  // Some platform encoders append an explicit SBR-absent sync extension to the LC base description.
  if ((description.size() != 2 && description.size() != 5) || (description.size() == 5 &&
      (description[2] != std::byte{0x56} || description[3] != std::byte{0xe5} || description[4] != std::byte{0})))
    return R::Failure(Error(CodecErrorCode::Unsupported, "Unsupported AAC encoder initialization data"));
  AudioEncodeFormat format{config.input.sample_rate, config.input.channel_layout, {description[0], description[1]}};
  auto valid = ValidateAudioEncodeFormat(format, config);
  return valid.Succeeded() ? R::Success(std::move(format)) : R::Failure(valid.Error());
}
namespace {
struct VideoSyntaxFailure { CodecError error; };
void VideoInvalid(const char* text) { throw VideoSyntaxFailure{Error(CodecErrorCode::InvalidData, text)}; }
void VideoUnsupported(const char* text) { throw VideoSyntaxFailure{Error(CodecErrorCode::Unsupported, text)}; }
class VideoBits {
public:
  explicit VideoBits(std::span<const std::byte> nal) {
    unsigned zeros = 0;
    for (std::size_t i = 1; i < nal.size(); ++i) {
      const auto value = std::to_integer<unsigned>(nal[i]);
      if (zeros >= 2 && value == 3) { zeros = 0; continue; }
      bytes_.push_back(static_cast<unsigned char>(value));
      zeros = value == 0 ? zeros + 1 : 0;
    }
  }
  unsigned Read(unsigned count) {
    if (count > 32 || count > bytes_.size() * 8 - position_) VideoInvalid("Truncated H.264 encoder syntax");
    unsigned result = 0;
    for (unsigned i = 0; i < count; ++i, ++position_)
      result = (result << 1) | ((bytes_[position_ / 8] >> (7 - position_ % 8)) & 1);
    return result;
  }
  unsigned Unsigned() {
    unsigned zeros = 0;
    while (!Read(1)) if (++zeros > 30) VideoInvalid("H.264 encoder syntax exceeds integer bounds");
    return ((1u << zeros) - 1) + Read(zeros);
  }
  int Signed() { const auto value = Unsigned(); return value & 1 ? int((value + 1) / 2) : -int(value / 2); }
private:
  std::vector<unsigned char> bytes_;
  std::size_t position_ = 0;
};
std::vector<std::span<const std::byte>> VideoNals(std::span<const std::byte> bytes, unsigned length_size) {
  std::vector<std::span<const std::byte>> result;
  auto append = [&](std::size_t first, std::size_t size) {
    if (!size || result.size() >= 1024) VideoInvalid("Empty H.264 NAL or too many NAL units");
    const auto header = std::to_integer<unsigned>(bytes[first]);
    const auto type = header & 31;
    if ((header & 128) || !type || (type > 12 && type != 14)) VideoInvalid("Unsupported H.264 NAL unit");
    if (type == 14 && (size < 4 || !(std::to_integer<unsigned>(bytes[first + 1]) & 128) ||
        std::to_integer<unsigned>(bytes[first + 2]) != 128 ||
        (std::to_integer<unsigned>(bytes[first + 3]) & 23) != 7))
      VideoUnsupported("H.264 prefix requires unsupported extension-layer decoding");
    result.push_back(bytes.subspan(first, size));
  };
  if (length_size) {
    if (length_size != 1 && length_size != 2 && length_size != 4) VideoInvalid("Invalid H.264 NAL length size");
    for (std::size_t i = 0; i < bytes.size();) {
      if (bytes.size() - i < length_size) VideoInvalid("Truncated H.264 NAL length");
      std::size_t size = 0;
      for (unsigned j = 0; j < length_size; ++j) size = (size << 8) | std::to_integer<unsigned>(bytes[i++]);
      if (size > bytes.size() - i) VideoInvalid("Truncated H.264 NAL payload");
      append(i, size); i += size;
    }
  } else {
    std::size_t first = 0, zeros = 0;
    bool found = false;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
      if (bytes[i] == std::byte{0}) { ++zeros; continue; }
      if (bytes[i] == std::byte{1} && zeros >= 2) {
        if (found) append(first, i - zeros - first);
        else if (i != zeros) VideoInvalid("Bytes precede the first H.264 start code");
        first = i + 1; found = true;
      }
      zeros = 0;
    }
    if (!found) VideoInvalid("Missing H.264 start code");
    append(first, bytes.size() - zeros - first);
  }
  if (result.empty()) VideoInvalid("Empty H.264 encoder data");
  return result;
}
unsigned VideoProfileId(H264Profile profile) {
  switch (profile) {
  case H264Profile::Baseline: return 66;
  case H264Profile::Main: return 77;
  case H264Profile::High: return 100;
  }
  return 0;
}
VideoEncodeFormat VideoGeometry(std::span<const std::byte> sps, const VideoEncoderConfig& config) {
  VideoBits bits(sps);
  if (bits.Read(8) != VideoProfileId(config.profile)) VideoUnsupported("H.264 encoder changed the requested profile");
  if (bits.Read(8) & 3) VideoInvalid("Invalid reserved H.264 constraint bits");
  if (!bits.Read(8) || bits.Unsigned() > 31) VideoInvalid("Invalid H.264 encoder SPS header");
  if (config.profile == H264Profile::High) {
    if (bits.Unsigned() != 1 || bits.Unsigned() || bits.Unsigned()) VideoUnsupported("H.264 output must be eight-bit 4:2:0");
    if (bits.Read(1)) VideoUnsupported("H.264 transform bypass is unsupported");
    if (bits.Read(1)) for (unsigned i = 0; i < 8; ++i) if (bits.Read(1)) {
      int last = 8, next = 8;
      for (unsigned j = 0; j < (i < 6 ? 16u : 64u); ++j) {
        if (next) next = ((last + bits.Signed()) % 256 + 256) % 256;
        if (next) last = next;
      }
    }
  }
  if (bits.Unsigned() > 12) VideoInvalid("Invalid H.264 frame-number width");
  const auto order = bits.Unsigned();
  if (order == 0) { if (bits.Unsigned() > 12) VideoInvalid("Invalid H.264 picture-order width"); }
  else if (order == 1) {
    bits.Read(1); bits.Signed(); bits.Signed();
    const auto count = bits.Unsigned();
    if (count > 255) VideoInvalid("Invalid H.264 picture-order cycle");
    for (unsigned i = 0; i < count; ++i) bits.Signed();
  } else if (order != 2) VideoInvalid("Invalid H.264 picture order");
  if (bits.Unsigned() > 16) VideoUnsupported("H.264 reference count exceeds decoder support");
  bits.Read(1); // The gaps-allowed declaration does not imply missing input pictures.
  const auto columns = bits.Unsigned(), rows = bits.Unsigned();
  if (columns >= 512 || rows >= 512 || !bits.Read(1)) VideoUnsupported("H.264 output is interlaced or exceeds coded bounds");
  bits.Read(1);
  VideoEncodeFormat format;
  format.width = (columns + 1) * 16; format.height = (rows + 1) * 16;
  unsigned left = 0, right = 0, top = 0, bottom = 0;
  if (bits.Read(1)) { left = bits.Unsigned(); right = bits.Unsigned(); top = bits.Unsigned(); bottom = bits.Unsigned(); }
  if (left > format.width / 2 || right > format.width / 2 - left ||
      top > format.height / 2 || bottom > format.height / 2 - top) VideoInvalid("Invalid H.264 output crop");
  format.visible = {left * 2, top * 2, format.width - (left + right) * 2, format.height - (top + bottom) * 2};
  if (format.visible.x || format.visible.y || format.visible.width != config.input.width ||
      format.visible.height != config.input.height) VideoUnsupported("H.264 encoder changed the visible picture");
  unsigned primaries = 2, transfer = 2, matrix = 2, full = 0;
  if (bits.Read(1)) {
    if (bits.Read(1)) {
      const auto aspect = bits.Read(8);
      if (aspect == 255) {
        const auto width = bits.Read(16), height = bits.Read(16);
        if (!width || width != height) VideoUnsupported("H.264 encoder changed the pixel aspect ratio");
      } else if (aspect != 0 && aspect != 1) VideoUnsupported("H.264 encoder changed the pixel aspect ratio");
    }
    if (bits.Read(1)) bits.Read(1);
    if (bits.Read(1)) {
      bits.Read(3); full = bits.Read(1);
      if (bits.Read(1)) { primaries = bits.Read(8); transfer = bits.Read(8); matrix = bits.Read(8); }
    }
  }
  const auto& input = config.input;
  const auto expected_primaries = input.primaries == ColorPrimaries::BT709 ? 1u :
      input.primaries == ColorPrimaries::BT601_525 ? 6u : 5u;
  const auto expected_transfer = input.transfer == ColorTransfer::BT709 ? 1u :
      input.transfer == ColorTransfer::SRGB ? 13u : 8u;
  if ((input.primaries != ColorPrimaries::Unknown && primaries != expected_primaries) ||
      (input.transfer != ColorTransfer::Unknown && transfer != expected_transfer) ||
      (input.matrix == ColorMatrix::BT709 && matrix != 1) ||
      (input.matrix == ColorMatrix::BT601 && matrix != 5 && matrix != 6) ||
      (input.range != ColorRange::Unknown && bool(full) != (input.range == ColorRange::Full)))
    VideoUnsupported("H.264 encoder did not preserve requested color signaling");
  return format;
}
}
std::size_t VideoEncoderWindow(const VideoEncoderConfig& config) {
  return std::min<std::size_t>({config.limits.output_items, config.limits.output_bytes / config.max_packet_bytes, 32});
}
std::uint32_t VideoEncoderBFrames(const VideoEncoderConfig& config) {
  if (config.profile == H264Profile::Baseline) return 0;
  return config.max_b_frames.value_or(static_cast<std::uint32_t>(std::min<std::size_t>(2, VideoEncoderWindow(config) - 2)));
}
CodecResult<std::size_t> ValidateVideoEncoderConfig(const VideoEncoderConfig& config) {
  using R = CodecResult<std::size_t>;
  const auto& input = config.input;
  if (!input.Validate() || input.width < 2 || input.height < 2 || input.width > 8192 || input.height > 8192 ||
      input.width % 2 || input.height % 2 || !config.frame_rate_numerator || !config.frame_rate_denominator ||
      std::uint64_t(config.frame_rate_numerator) > std::uint64_t(config.frame_rate_denominator) * 240 ||
      !config.bitrate || config.bitrate > INT32_MAX || !VideoProfileId(config.profile) || !config.key_frame_interval ||
      config.key_frame_interval > INT32_MAX || !config.max_packet_bytes || config.max_packet_bytes > INT32_MAX ||
      !config.limits.input_items || config.limits.output_items < 2 ||
      config.max_packet_bytes > config.limits.output_bytes / 2 ||
      (config.profile == H264Profile::Baseline && config.max_b_frames.value_or(0)))
    return R::Failure(Error(CodecErrorCode::InvalidArgument, "Invalid video encoder geometry, controls, or queue bounds"));
  if (input.visible != VideoRectangle{0, 0, input.width, input.height} ||
      input.aspect_width != input.aspect_height || input.rotation != VideoRotation::None ||
      input.primaries == ColorPrimaries::BT2020 || input.matrix == ColorMatrix::BT2020 ||
      input.transfer == ColorTransfer::PQ || input.transfer == ColorTransfer::HLG ||
      config.max_b_frames.value_or(0) > 16)
    return R::Failure(Error(CodecErrorCode::Unsupported, "H.264 encoding requires full SDR square-pixel pictures and bounded B frames"));
  const auto bytes = std::size_t(input.width) * input.height * 3 / 2;
  if (bytes > config.limits.input_bytes || (config.max_b_frames &&
      std::size_t(*config.max_b_frames) + 2 > VideoEncoderWindow(config)))
    return R::Failure(Error(CodecErrorCode::InvalidArgument, "Video encoder queues cannot fit a picture or the requested delay window"));
  return R::Success(bytes);
}
void CopyVideoEncoderPixels(const VideoFrame& frame, VideoPixelFormat layout, std::span<std::byte> target) {
  const auto width = frame.format.width, height = frame.format.height;
  if (target.size() < std::size_t(width) * height * 3 / 2) throw std::invalid_argument("Video encoder pixel destination is too small");
  const auto& y_plane = frame.planes[0];
  const auto y_bytes = y_plane.data.AsBytes();
  for (std::size_t y = 0; y < height; ++y) for (std::size_t x = 0; x < width; ++x)
    target[y * width + x] = y_bytes[y * y_plane.row_stride + x * y_plane.pixel_stride];
  const auto base = std::size_t(width) * height, chroma = base / 4;
  for (std::size_t y = 0; y < height / 2; ++y) for (std::size_t x = 0; x < width / 2; ++x) {
    std::byte u, v;
    if (frame.format.pixel_format == VideoPixelFormat::NV12) {
      const auto& plane = frame.planes[1];
      const auto bytes = plane.data.AsBytes();
      const auto offset = y * plane.row_stride + x * plane.pixel_stride;
      u = bytes[offset]; v = bytes[offset + 1];
    } else {
      const auto& up = frame.planes[1]; const auto& vp = frame.planes[2];
      u = up.data.AsBytes()[y * up.row_stride + x * up.pixel_stride];
      v = vp.data.AsBytes()[y * vp.row_stride + x * vp.pixel_stride];
    }
    const auto index = y * (width / 2) + x;
    if (layout == VideoPixelFormat::NV12) { target[base + index * 2] = u; target[base + index * 2 + 1] = v; }
    else { target[base + index] = u; target[base + chroma + index] = v; }
  }
}
CodecResult<VideoEncodeFormat> ReadVideoEncodeFormat(std::span<const std::byte> description, const VideoEncoderConfig& config) {
  using R = CodecResult<VideoEncodeFormat>;
  try {
    if (description.empty() || description.size() > 65536) VideoInvalid("H.264 configuration exceeds 64 KiB");
    AvcConfig parsed{4, {}};
    if (description[0] == std::byte{1}) {
      auto value = ParseAvc(description);
      if (!value.Succeeded()) return R::Failure(value.Error());
      parsed = std::move(value).Value();
    } else for (auto nal : VideoNals(description, 0)) {
      const auto type = std::to_integer<unsigned>(nal[0]) & 31;
      if (type == 7 || type == 8) parsed.parameters.emplace_back(nal.begin(), nal.end());
    }
    std::vector<std::span<const std::byte>> sps, pps;
    for (const auto& nal : parsed.parameters) {
      if ((std::to_integer<unsigned>(nal[0]) & 31) == 7) sps.emplace_back(nal);
      else pps.emplace_back(nal);
    }
    if (sps.empty() || sps.size() > 31 || pps.empty() || pps.size() > 255 || sps[0].size() < 4)
      VideoInvalid("H.264 encoder configuration needs bounded SPS and PPS data");
    auto format = VideoGeometry(sps[0], config);
    for (auto nal : sps) if (VideoGeometry(nal, config) != format) VideoUnsupported("H.264 parameter sets describe different pictures");
    Bytes data{std::byte{1}, sps[0][1], sps[0][2], sps[0][3], std::byte{255}, std::byte(224 | sps.size())};
    auto append = [&](const auto& nals) {
      for (auto nal : nals) {
        if (nal.size() > 65535 || data.size() + 2 + nal.size() > 65532) VideoInvalid("H.264 parameter sets exceed 64 KiB");
        data.push_back(std::byte(nal.size() >> 8)); data.push_back(std::byte(nal.size()));
        data.insert(data.end(), nal.begin(), nal.end());
      }
    };
    append(sps); data.push_back(std::byte(pps.size())); append(pps);
    if (config.profile == H264Profile::High) data.insert(data.end(), {std::byte{253}, std::byte{248}, std::byte{248}, std::byte{0}});
    format.initialization_data = std::move(data);
    return R::Success(std::move(format));
  } catch (const VideoSyntaxFailure& failure) { return R::Failure(failure.error); }
}
CodecResult<Bytes> NormalizeVideoEncoderPacket(std::span<const std::byte> bytes, unsigned length_size, std::size_t maximum, bool strip_headers) {
  using R = CodecResult<Bytes>;
  try {
    if (bytes.size() > maximum) return R::Failure(Error(CodecErrorCode::ResourceExhausted, "H.264 packet exceeds configured byte bound"));
    Bytes result;
    for (auto nal : VideoNals(bytes, length_size)) {
      const auto type = std::to_integer<unsigned>(nal[0]) & 31;
      // AVC base pictures decode independently of their optional, validated extension prefix.
      if (type == 14) continue;
      if (strip_headers && (type == 7 || type == 8 || type == 9)) continue;
      if (result.size() > maximum || maximum - result.size() < 4 || nal.size() > maximum - result.size() - 4)
        return R::Failure(Error(CodecErrorCode::ResourceExhausted, "Normalized H.264 packet exceeds configured byte bound"));
      const auto size = static_cast<std::uint32_t>(nal.size());
      for (auto shift : {24, 16, 8, 0}) result.push_back(std::byte(size >> shift));
      result.insert(result.end(), nal.begin(), nal.end());
    }
    return R::Success(std::move(result));
  } catch (const VideoSyntaxFailure& failure) { return R::Failure(failure.error); }
}
CodecResult<VideoEncoderPicture> InspectVideoEncoderPacket(const EncodedPacket& packet, const VideoEncodeFormat& format) {
  using R = CodecResult<VideoEncoderPicture>;
  try {
    const auto config = ParseAvc(format.initialization_data);
    if (!config.Succeeded()) return R::Failure(config.Error());
    VideoEncoderPicture picture;
    unsigned starts = 0, slices = 0;
    for (auto nal : VideoNals(packet.data.AsBytes(), 4)) {
      const auto type = std::to_integer<unsigned>(nal[0]) & 31;
      picture.headers |= type == 7 || type == 8 || type == 9 || type == 14;
      if (type == 7 || type == 8) {
        bool found = false;
        for (const auto& parameter : config.Value().parameters)
          if (std::ranges::equal(parameter, nal)) found = true;
        if (!found) VideoUnsupported("H.264 parameter sets changed inside the stream");
      }
      if (type != 1 && type != 5) continue;
      VideoBits bits(nal);
      if (!bits.Unsigned()) ++starts;
      const auto slice = bits.Unsigned();
      if (slice > 9 || slice % 5 > 2) VideoUnsupported("Unsupported H.264 picture coding type");
      picture.key_frame |= type == 5; picture.b_frame |= slice % 5 == 1; ++slices;
    }
    if (!slices || starts != 1 || (picture.key_frame && picture.b_frame)) VideoInvalid("H.264 access unit must contain exactly one complete picture");
    return R::Success(picture);
  } catch (const VideoSyntaxFailure& failure) { return R::Failure(failure.error); }
}
} // namespace huxerui::media::detail
