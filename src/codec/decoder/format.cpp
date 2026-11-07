#include "format.h"

namespace huxerui::media::detail {
namespace {
class Bits {
public:
  explicit Bits(std::span<const std::byte> nal) {
    unsigned zeros = 0;
    for (std::size_t i = 1; i < nal.size(); ++i) {
      const auto value = std::to_integer<unsigned>(nal[i]);
      if (zeros >= 2 && value == 3) { zeros = 0; continue; }
      bytes_.push_back(value);
      zeros = value == 0 ? zeros + 1 : 0;
    }
  }
  unsigned Read(unsigned count) {
    if (count > 32 || count > bytes_.size() * 8 - position_) throw std::invalid_argument("Truncated AVC SPS");
    unsigned value = 0;
    for (unsigned i = 0; i < count; ++i, ++position_) value = (value << 1) | ((bytes_[position_ / 8] >> (7 - position_ % 8)) & 1);
    return value;
  }
  unsigned Unsigned() {
    unsigned zeros = 0;
    while (!Read(1)) if (++zeros > 30) throw std::invalid_argument("AVC Exp-Golomb value exceeds supported range");
    return ((1u << zeros) - 1) + Read(zeros);
  }
  int Signed() { const auto value = Unsigned(); return value & 1 ? int((value + 1) / 2) : -int(value / 2); }
private:
  std::vector<unsigned char> bytes_;
  std::size_t position_ = 0;
};
CodecResult<void> ValidateSps(const AvcConfig& avc, std::uint32_t maximum_width, std::uint32_t maximum_height) {
  try {
    for (const auto& nal : avc.parameters) {
      if ((std::to_integer<unsigned>(nal[0]) & 31) != 7) continue;
      Bits bits(nal);
      const auto profile = bits.Read(8);
      const auto constraints = bits.Read(8);
      const auto level = bits.Read(8);
      const auto id = bits.Unsigned();
      if ((constraints & 3) || !level || id > 31) throw std::invalid_argument("Invalid AVC SPS header");
      if (profile != 66 && profile != 77 && profile != 100)
        return CodecResult<void>::Failure(Error(CodecErrorCode::Unsupported, "Unsupported AVC SPS profile"));
      if (profile == 100) {
        const auto chroma = bits.Unsigned(), luma_depth = bits.Unsigned(), chroma_depth = bits.Unsigned();
        if (chroma != 1 || luma_depth || chroma_depth)
          return CodecResult<void>::Failure(Error(CodecErrorCode::Unsupported, "Only 8-bit 4:2:0 AVC is supported"));
        const auto bypass = bits.Read(1);
        if (bypass) return CodecResult<void>::Failure(Error(CodecErrorCode::Unsupported, "AVC transform bypass is unsupported"));
        if (bits.Read(1)) for (unsigned i = 0; i < 8; ++i) if (bits.Read(1)) {
          int last = 8, next = 8;
          for (unsigned j = 0; j < (i < 6 ? 16u : 64u); ++j) {
            if (next) next = ((last + bits.Signed()) % 256 + 256) % 256;
            if (next) last = next;
          }
        }
      }
      if (bits.Unsigned() > 12) throw std::invalid_argument("Invalid AVC frame number range");
      const auto order = bits.Unsigned();
      if (order == 0) {
        if (bits.Unsigned() > 12) throw std::invalid_argument("Invalid AVC picture order range");
      } else if (order == 1) {
        bits.Read(1);
        bits.Signed();
        bits.Signed();
        const auto cycle = bits.Unsigned();
        if (cycle > 255) throw std::invalid_argument("Invalid AVC picture order cycle");
        for (unsigned i = 0; i < cycle; ++i) bits.Signed();
      } else if (order != 2) throw std::invalid_argument("Invalid AVC picture order mode");
      if (bits.Unsigned() > 16) return CodecResult<void>::Failure(Error(CodecErrorCode::Unsupported, "AVC reference count exceeds the bounded decoder allowance"));
      const auto gaps = bits.Read(1);
      const auto width = bits.Unsigned(), height = bits.Unsigned();
      const auto progressive = bits.Read(1);
      if (!progressive || gaps) return CodecResult<void>::Failure(Error(CodecErrorCode::Unsupported, "Interlaced AVC and frame-number gaps are unsupported"));
      if (width >= maximum_width / 16 + (maximum_width % 16 != 0) || height >= maximum_height / 16 + (maximum_height % 16 != 0)
          || (width + 1) * 16 > maximum_width || (height + 1) * 16 > maximum_height)
        return CodecResult<void>::Failure(Error(CodecErrorCode::InvalidArgument, "AVC SPS coded dimensions exceed configuration"));
    }
  } catch (const std::invalid_argument& error) {
    return CodecResult<void>::Failure(Error(CodecErrorCode::InvalidArgument, error.what()));
  }
  return CodecResult<void>::Success();
}
} // namespace
CodecResult<AvcConfig> ParseAvc(std::span<const std::byte> bytes) {
  using R = CodecResult<AvcConfig>;
  auto invalid = [] { return R::Failure(Error(CodecErrorCode::InvalidArgument, "Malformed AVCDecoderConfigurationRecord")); };
  auto u = [&](std::size_t i) { return std::to_integer<unsigned>(bytes[i]); };
  if (bytes.size() < 7 || bytes.size() > 65536 || u(0) != 1 || (u(4) & 252) != 252 || (u(5) & 224) != 224) return invalid();
  if (u(1) != 66 && u(1) != 77 && u(1) != 100)
    return R::Failure(Error(CodecErrorCode::Unsupported, "Only 8-bit AVC Baseline, Main, and High profiles are supported"));
  AvcConfig result{(u(4) & 3) + 1, {}};
  if (result.length_size == 3) return invalid();
  std::size_t position = 6;
  auto read = [&](unsigned count, unsigned type) {
    if (!count) return false;
    for (unsigned i = 0; i < count; ++i) {
      if (position + 2 > bytes.size()) return false;
      const auto size = (u(position) << 8) | u(position + 1);
      position += 2;
      if (!size || size > bytes.size() - position || (u(position) & 31) != type) return false;
      result.parameters.emplace_back(bytes.begin() + position, bytes.begin() + position + size);
      position += size;
    }
    return true;
  };
  if (!read(u(5) & 31, 7) || position >= bytes.size()) return invalid();
  const auto pps_count = u(position++);
  if (!read(pps_count, 8)) return invalid();
  if (position != bytes.size()) {
    if (u(1) != 100 || bytes.size() - position != 4 || u(position) != 253
        || u(position + 1) != 248 || u(position + 2) != 248 || u(position + 3) != 0)
      return R::Failure(Error(CodecErrorCode::Unsupported, "Unsupported AVC chroma, depth, or configuration extension"));
  }
  return R::Success(std::move(result));
}
CodecResult<Bytes> AnnexB(const EncodedPacket& packet, unsigned length_size) {
  const auto data = packet.data.AsBytes();
  Bytes result;
  for (std::size_t i = 0; i < data.size();) {
    if (data.size() - i < length_size) return CodecResult<Bytes>::Failure(Error(CodecErrorCode::InvalidData, "Truncated AVC NAL length"));
    std::size_t size = 0;
    for (unsigned j = 0; j < length_size; ++j) size = (size << 8) | std::to_integer<unsigned>(data[i++]);
    if (!size || size > data.size() - i) return CodecResult<Bytes>::Failure(Error(CodecErrorCode::InvalidData, "Truncated AVC NAL payload"));
    result.insert(result.end(), {std::byte{0}, std::byte{0}, std::byte{0}, std::byte{1}});
    result.insert(result.end(), data.begin() + i, data.begin() + i + size);
    i += size;
  }
  return CodecResult<Bytes>::Success(std::move(result));
}
CodecResult<void> ValidatePacket(const EncodedPacket& packet, const DecoderConfig& config, bool needs_key) {
  auto invalid = [](const char* text) { return CodecResult<void>::Failure(Error(CodecErrorCode::InvalidData, text)); };
  if (packet.data.AsBytes().empty() || !packet.pts.Rescale(1000000)
      || (packet.dts && !packet.dts->Rescale(1000000))
      || (packet.duration && (!packet.duration->Rescale(1000000) || packet.duration->ticks < 0)))
    return invalid("Empty packet or invalid/unrepresentable timing");
  if (const auto* video = std::get_if<VideoDecoderConfig>(&config)) {
    const auto parsed = ParseAvc(video->initialization_data);
    const auto length = parsed.Value().length_size;
    auto bytes = packet.data.AsBytes();
    bool idr = false, picture = false;
    for (std::size_t i = 0; i < bytes.size();) {
      if (bytes.size() - i < length) return invalid("Truncated AVC NAL length");
      std::size_t size = 0;
      for (unsigned j = 0; j < length; ++j) size = (size << 8) | std::to_integer<unsigned>(bytes[i++]);
      if (!size || size > bytes.size() - i) return invalid("Truncated AVC NAL payload");
      const auto type = std::to_integer<unsigned>(bytes[i]) & 31;
      if (type == 5) idr = true;
      if (type == 1 || type == 5) picture = true;
      if (type == 7) {
        AvcConfig update{length, {Bytes(bytes.begin() + i, bytes.begin() + i + size)}};
        const auto supported = ValidateSps(update, video->width, video->height);
        if (!supported.Succeeded()) return supported;
      }
      if (!type || type > 12 || (std::to_integer<unsigned>(bytes[i]) & 128)) return invalid("Unsupported AVC NAL type");
      i += size;
    }
    if (!picture) return invalid("AVC access unit contains no picture");
    if (needs_key && (!packet.key_frame || !idr)) return invalid("AVC requires an IDR access unit after creation/reset");
  }
  return CodecResult<void>::Success();
}
CodecResult<std::size_t> ValidateConfig(const DecoderConfig& config) {
  using R = CodecResult<std::size_t>;
  std::size_t frame_bytes = 0, minimum = 4;
  if (const auto* audio = std::get_if<AudioDecoderConfig>(&config)) {
    const auto parsed = ParseAac(audio->initialization_data);
    if (!parsed.Succeeded()) return R::Failure(parsed.Error());
    if (parsed.Value().sample_rate != audio->output.sample_rate || parsed.Value().channels != audio->output.ChannelCount())
      return R::Failure(Error(CodecErrorCode::Unsupported, "PCM rate/layout must match AAC initialization data"));
    if (!audio->output.BytesPerSample() || (audio->output.packing != AudioPacking::Planar && audio->output.packing != AudioPacking::Interleaved))
      return R::Failure(Error(CodecErrorCode::InvalidArgument, "Invalid PCM representation"));
    frame_bytes = 1024 * audio->output.BytesPerSample() * audio->output.ChannelCount();
  } else {
    const auto& video = std::get<VideoDecoderConfig>(config);
    const auto parsed = ParseAvc(video.initialization_data);
    if (!parsed.Succeeded()) return R::Failure(parsed.Error());
    if (!video.width || !video.height || video.width > 8192 || video.height > 8192
        || (video.output != VideoPixelFormat::NV12 && video.output != VideoPixelFormat::I420))
      return R::Failure(Error(CodecErrorCode::InvalidArgument, "AVC dimensions must be in 1..8192 with NV12/I420 output"));
    const auto supported = ValidateSps(parsed.Value(), video.width, video.height);
    if (!supported.Succeeded()) return R::Failure(supported.Error());
    frame_bytes = std::size_t(video.width) * video.height + 2 * ((std::size_t(video.width) + 1) / 2) * ((std::size_t(video.height) + 1) / 2);
    minimum = 34;
  }
  const auto limits = std::visit([](const auto& c) { return c.limits; }, config);
  if (limits.input_items < 2 || !limits.input_bytes || limits.output_items < minimum || frame_bytes > limits.output_bytes / minimum)
    return R::Failure(Error(CodecErrorCode::InvalidArgument, "Queue limits cannot accommodate the decoder's bounded delay and maximum frames"));
  return R::Success(frame_bytes);
}
} // namespace huxerui::media::detail
