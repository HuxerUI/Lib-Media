#include "format.h"
#include "../hevc.h"
#include "container/container_io.h"
#include "format/platform_video_storage.h"

#include <array>

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
      bits.Read(1); // Allow the SPS declaration; callers must still provide a complete, continuous stream.
      const auto width = bits.Unsigned(), height = bits.Unsigned();
      const auto progressive = bits.Read(1);
      if (!progressive) return CodecResult<void>::Failure(Error(CodecErrorCode::Unsupported, "Interlaced AVC is unsupported"));
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

CodecResult<unsigned> AudioSampleLimit(const AudioDecoderConfig& config) {
  using R = CodecResult<unsigned>;
  const auto& output = config.output;
  if (!output.sample_rate || !output.ChannelCount() || !output.BytesPerSample() ||
      (output.packing != AudioPacking::Planar && output.packing != AudioPacking::Interleaved))
    return R::Failure(Error(CodecErrorCode::InvalidArgument, "Invalid PCM output format"));
  const auto matches = [&](unsigned rate, unsigned channels, unsigned samples) {
    if (rate == output.sample_rate && channels == output.ChannelCount()) return R::Success(samples);
    return R::Failure(Error(CodecErrorCode::Unsupported, "PCM rate and layout must match the coded stream"));
  };
  try {
    switch (config.codec) {
      case CodecId::Aac: {
        const auto info = ParseAac(config.initialization_data);
        if (!info.Succeeded()) return R::Failure(info.Error());
        return matches(info.Value().sample_rate, info.Value().channels, 1024);
      }
      case CodecId::Opus: {
        const auto info = ParseOpus(config.initialization_data, ContainerErrorCode::Unsupported);
        return matches(48000, info.channels, 5760);
      }
      case CodecId::Flac: {
        const auto info = ParseFlacStreamInfo(config.initialization_data, ContainerErrorCode::InvalidArgument);
        return matches(info.rate, info.channels, info.max_block);
      }
      case CodecId::Mp3: {
        constexpr std::array rates{8000u, 11025u, 12000u, 16000u, 22050u, 24000u, 32000u, 44100u, 48000u};
        if (!config.initialization_data.empty() || std::ranges::find(rates, output.sample_rate) == rates.end())
          return R::Failure(Error(CodecErrorCode::InvalidArgument, "Invalid MPEG Layer III configuration"));
        return R::Success(output.sample_rate >= 32000 ? 1152u : 576u);
      }
      default: return R::Failure(Error(CodecErrorCode::InvalidArgument, "Unsupported audio codec identifier"));
    }
  } catch (const ContainerError& error) {
    return R::Failure(Error(error.code == ContainerErrorCode::Unsupported ? CodecErrorCode::Unsupported :
                            CodecErrorCode::InvalidArgument, error.message));
  }
}

CodecResult<unsigned> AudioPacketSamples(const AudioDecoderConfig& config, std::span<const std::byte> bytes) {
  using R = CodecResult<unsigned>;
  try {
    switch (config.codec) {
      case CodecId::Aac: return R::Success(1024);
      case CodecId::Opus: return R::Success(OpusPacketSamples(bytes, ContainerErrorCode::InvalidData));
      case CodecId::Flac:
        return R::Success(FlacPacketSamples(bytes, ParseFlacStreamInfo(config.initialization_data, ContainerErrorCode::InvalidData)));
      case CodecId::Mp3: {
        const auto info = ParseMp3FrameHeader(bytes);
        if (info && info->length == bytes.size() && info->rate == config.output.sample_rate &&
            info->channels == config.output.ChannelCount()) return R::Success(info->samples);
        return R::Failure(Error(CodecErrorCode::InvalidData, "Invalid MPEG Layer III frame or configuration change"));
      }
      default: return R::Failure(Error(CodecErrorCode::InvalidArgument, "Unsupported audio codec identifier"));
    }
  } catch (const ContainerError& error) {
    return R::Failure(Error(CodecErrorCode::InvalidData, error.message));
  }
}

Bytes AudioDecoderHeader(const AudioDecoderConfig& config) {
  if (config.codec == CodecId::Flac) {
    Bytes header{std::byte{'f'}, std::byte{'L'}, std::byte{'a'}, std::byte{'C'},
                 std::byte{0x80}, std::byte{0}, std::byte{0}, std::byte{34}};
    Append(header, config.initialization_data);
    return header;
  }
  auto header = config.initialization_data;
  if (config.codec == CodecId::Opus) header[10] = header[11] = std::byte{};
  return header;
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
  if (const auto* audio = std::get_if<AudioDecoderConfig>(&config); audio && audio->codec != CodecId::Aac) {
    const auto samples = AudioPacketSamples(*audio, packet.data.AsBytes());
    if (!samples.Succeeded()) return CodecResult<void>::Failure(samples.Error());
  }
  if (const auto* video = std::get_if<VideoDecoderConfig>(&config)) {
    if (video->codec == CodecId::Hevc) {
      const auto length = (std::to_integer<unsigned>(video->initialization_data[21]) & 3u) + 1;
      try {
        const bool idr = IsHevcIdr(packet.data.AsBytes(), length, true, true);
        if (needs_key && (!packet.key_frame || !idr))
          return invalid("HEVC requires an IDR access unit after creation/reset");
      } catch (const std::invalid_argument& error) {
        return invalid(error.what());
      }
      return CodecResult<void>::Success();
    }
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
    const auto samples = AudioSampleLimit(*audio);
    if (!samples.Succeeded()) return R::Failure(samples.Error());
    frame_bytes = std::size_t(samples.Value()) * audio->output.BytesPerSample() * audio->output.ChannelCount();
  } else {
    const auto& video = std::get<VideoDecoderConfig>(config);
    if (!video.width || !video.height || video.width > 8192 || video.height > 8192)
      return R::Failure(Error(CodecErrorCode::InvalidArgument, "Video dimensions must be in 1..8192"));
    const bool platform_storage = video.output_storage == VideoStorage::Platform;
    if (video.output_storage != VideoStorage::Cpu && !platform_storage)
      return R::Failure(Error(CodecErrorCode::InvalidArgument, "Invalid video decoder storage"));
    if (platform_storage && (!PlatformVideoDecodeSupported || video.output != PlatformVideoLayout))
      return R::Failure(Error(CodecErrorCode::Unsupported, "Platform video output layout is unavailable"));
    const bool eight_bit = platform_storage || video.output == VideoPixelFormat::NV12 || video.output == VideoPixelFormat::I420;
    if (video.codec == CodecId::H264) {
      const auto parsed = ParseAvc(video.initialization_data);
      if (!parsed.Succeeded()) return R::Failure(parsed.Error());
      if (!eight_bit) return R::Failure(Error(CodecErrorCode::Unsupported, "AVC requires NV12 or I420 output"));
      const auto supported = ValidateSps(parsed.Value(), video.width, video.height);
      if (!supported.Succeeded()) return R::Failure(supported.Error());
    } else if (video.codec == CodecId::Hevc) {
      try {
        const auto description = ParseHevcConfiguration(video.initialization_data, true, false);
        if (description.chroma_format != 1 || description.luma_bits != description.chroma_bits ||
            !(eight_bit ? description.luma_bits == 8 :
                video.output == VideoPixelFormat::P010 && description.luma_bits == 10))
          return R::Failure(Error(CodecErrorCode::Unsupported, "HEVC output must match an 8-bit or 10-bit 4:2:0 stream"));
      } catch (const std::invalid_argument& error) {
        return R::Failure(Error(CodecErrorCode::InvalidArgument, error.what()));
      }
    } else {
      return R::Failure(Error(CodecErrorCode::InvalidArgument, "Video decoding requires H.264 or HEVC"));
    }
    frame_bytes = VideoImageBytes(video.width, video.height, video.output);
    minimum = 34;
  }
  const auto limits = std::visit([](const auto& c) { return c.limits; }, config);
  if (limits.input_items < 2 || !limits.input_bytes || limits.output_items < minimum || frame_bytes > limits.output_bytes / minimum)
    return R::Failure(Error(CodecErrorCode::InvalidArgument, "Queue limits cannot accommodate the decoder's bounded delay and maximum frames"));
  return R::Success(frame_bytes);
}
} // namespace huxerui::media::detail
