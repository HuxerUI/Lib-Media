#include "format.h"
#include "../av1.h"
#include "../hevc.h"
#include "../vorbis.h"
#include "container/container_io.h"
#include "format/platform_video_storage.h"

#include <array>

#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

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
      case CodecId::Alac: {
#if defined(__APPLE__) && (TARGET_OS_OSX || TARGET_OS_IOS)
        const auto info = ParseAlac(config.initialization_data, ContainerErrorCode::InvalidArgument);
        if (info.frame_length > UINT32_MAX / (output.BytesPerSample() * output.ChannelCount()))
          return R::Failure(Error(CodecErrorCode::Unsupported, "ALAC frames exceed the platform PCM buffer limit"));
        return matches(info.sample_rate, info.channels, info.frame_length);
#else
        return R::Failure(Error(CodecErrorCode::Unsupported, "ALAC decoding is unavailable on this platform"));
#endif
      }
      case CodecId::Opus: {
        const auto info = ParseOpus(config.initialization_data, ContainerErrorCode::Unsupported);
        return matches(48000, info.channels, 5760);
      }
      case CodecId::Vorbis: {
        const VorbisHeaders info(config.initialization_data, ContainerErrorCode::InvalidArgument);
        return matches(info.Rate(), info.Channels(), info.MaxSamples());
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
      case CodecId::Alac:
        return R::Success(AlacPacketSamples(bytes, ParseAlac(config.initialization_data, ContainerErrorCode::InvalidData),
                                            ContainerErrorCode::InvalidData));
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

Bytes Mp3DrainFrame(std::span<const std::byte> packet) {
  Bytes frame(packet.size());
  std::ranges::copy(packet.first(4), frame.begin());
  frame[1] |= std::byte{1};
  return frame;
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
CodecResult<unsigned> Vp9Level(std::span<const std::byte> bytes) {
  using R = CodecResult<unsigned>;
  unsigned level = 0, seen = 0;
  while (!bytes.empty()) {
    if (bytes.size() < 2)
      return R::Failure(Error(CodecErrorCode::InvalidArgument, "Truncated VP9 codec feature header"));
    const auto id = std::to_integer<unsigned>(bytes[0]), size = std::to_integer<unsigned>(bytes[1]);
    bytes = bytes.subspan(2);
    if (size > bytes.size())
      return R::Failure(Error(CodecErrorCode::InvalidArgument, "Truncated VP9 codec feature payload"));
    if (id >= 1 && id <= 4) {
      if (size != 1 || (seen & (1u << id)))
        return R::Failure(Error(CodecErrorCode::InvalidArgument, "Invalid or duplicate VP9 codec feature"));
      seen |= 1u << id;
      const auto value = std::to_integer<unsigned>(bytes.front());
      if ((id == 1 && value != 0) || (id == 3 && value != 8) || (id == 4 && value > 1))
        return R::Failure(Error(CodecErrorCode::Unsupported, "VP9 requires profile 0, eight-bit 4:2:0"));
      if (id == 2) {
        constexpr unsigned levels[]{10, 11, 20, 21, 30, 31, 40, 41, 50, 51, 52, 60, 61, 62};
        if (std::ranges::find(levels, value) == std::end(levels))
          return R::Failure(Error(CodecErrorCode::InvalidArgument, "Invalid VP9 codec level"));
        level = value;
      }
    }
    bytes = bytes.subspan(size);
  }
  return R::Success(level);
}

CodecResult<bool> Vp9PacketDisplays(std::span<const std::byte> bytes) {
  using R = CodecResult<bool>;
  const auto displays = [](std::span<const std::byte> frame) {
    if (frame.empty() || (std::to_integer<unsigned>(frame.front()) & 0xf0) != 0x80)
      return R::Failure(Error(CodecErrorCode::InvalidData, "VP9 requires a profile 0 frame header"));
    // show_existing_frame and show_frame both produce a picture for presentation.
    return R::Success((std::to_integer<unsigned>(frame.front()) & 0x0a) != 0);
  };
  if (bytes.empty()) return displays(bytes);
  const auto marker = std::to_integer<unsigned>(bytes.back());
  if ((marker & 0xe0) != 0xc0) return displays(bytes);
  const auto count = (marker & 7) + 1, width = ((marker >> 3) & 3) + 1;
  const auto index_size = 2 + count * width;
  if (bytes.size() < index_size || bytes[bytes.size() - index_size] != bytes.back()) return displays(bytes);
  const auto index = bytes.last(index_size);
  auto frames = bytes.first(bytes.size() - index_size);
  bool visible = false;
  for (unsigned i = 0; i < count; ++i) {
    std::uint32_t size = 0;
    for (unsigned j = 0; j < width; ++j)
      size |= std::to_integer<std::uint32_t>(index[1 + i * width + j]) << (8 * j);
    if (!size || size > frames.size())
      return R::Failure(Error(CodecErrorCode::InvalidData, "VP9 superframe index exceeds its packet"));
    auto frame = displays(frames.first(size));
    if (!frame.Succeeded()) return frame;
    if (frame.Value() && visible)
      return R::Failure(Error(CodecErrorCode::Unsupported, "VP9 packets may contain at most one visible frame"));
    visible |= frame.Value();
    frames = frames.subspan(size);
  }
  if (!frames.empty())
    return R::Failure(Error(CodecErrorCode::InvalidData, "VP9 superframe sizes do not cover its packet"));
  return R::Success(visible);
}

CodecResult<bool> ValidatePacket(const EncodedPacket& packet, const DecoderConfig& config, bool needs_key) {
  using R = CodecResult<bool>;
  auto invalid = [](const char* text) { return R::Failure(Error(CodecErrorCode::InvalidData, text)); };
  if (packet.data.AsBytes().empty() || !packet.pts.Rescale(1000000)
      || (packet.dts && !packet.dts->Rescale(1000000))
      || (packet.duration && (!packet.duration->Rescale(1000000) || packet.duration->ticks < 0)))
    return invalid("Empty packet or invalid/unrepresentable timing");
  if (const auto* audio = std::get_if<AudioDecoderConfig>(&config); audio && audio->codec != CodecId::Aac) {
    if (audio->codec == CodecId::Vorbis) return R::Success(!needs_key);
    const auto samples = AudioPacketSamples(*audio, packet.data.AsBytes());
    if (!samples.Succeeded()) return R::Failure(samples.Error());
  }
  if (const auto* video = std::get_if<VideoDecoderConfig>(&config)) {
    if (video->codec == CodecId::ProRes) {
      const auto header = ValidateProResFrame(packet.data.AsBytes(), video->width, video->height);
      if (!header.Succeeded()) return R::Failure(header.Error());
      if (needs_key && !packet.key_frame) return invalid("ProRes requires a marked frame after creation/reset");
      return R::Success(true);
    }
    if (video->codec == CodecId::Vp8) {
      const auto bytes = packet.data.AsBytes();
      const auto tag = std::to_integer<unsigned>(bytes.front());
      if (bytes.size() < 3 || (tag & 0x0e) > 6) return invalid("Invalid VP8 frame tag");
      if (needs_key && (!packet.key_frame || (tag & 1) || bytes.size() < 10 ||
                        bytes[3] != std::byte{0x9d} || bytes[4] != std::byte{0x01} || bytes[5] != std::byte{0x2a}))
        return invalid("VP8 requires a key frame after creation/reset");
      return R::Success((tag & 0x10) != 0);
    }
    if (video->codec == CodecId::Vp9) {
      const auto bytes = packet.data.AsBytes();
      const auto header = std::to_integer<unsigned>(bytes.front());
      if (needs_key && (!packet.key_frame || (header & 0x0c) || bytes.size() < 4 ||
                        bytes[1] != std::byte{0x49} || bytes[2] != std::byte{0x83} || bytes[3] != std::byte{0x42}))
        return invalid("VP9 requires a key frame after creation/reset");
      return Vp9PacketDisplays(bytes);
    }
    if (video->codec == CodecId::Av1) {
      try {
        const auto info = InspectAv1Packet(packet.data.AsBytes());
        if ((needs_key || packet.key_frame) && (!packet.key_frame || !info.sync))
          return invalid("AV1 requires a marked key packet with an in-band sequence header");
        if (info.pictures > 1 || info.spatial_layers || (info.sequence &&
            (!info.sequence->sdr_420 || info.sequence->width > video->width || info.sequence->height > video->height)))
          return R::Failure(Error(CodecErrorCode::Unsupported, "AV1 requires single-layer 8-bit SDR 4:2:0 within the configured bounds"));
        return R::Success(info.pictures != 0);
      } catch (const std::invalid_argument& error) {
        return invalid(error.what());
      }
    }
    if (video->codec == CodecId::Hevc) {
      const auto length = (std::to_integer<unsigned>(video->initialization_data[21]) & 3u) + 1;
      try {
        const bool idr = IsHevcIdr(packet.data.AsBytes(), length, true, true);
        if (needs_key && (!packet.key_frame || !idr))
          return invalid("HEVC requires an IDR access unit after creation/reset");
      } catch (const std::invalid_argument& error) {
        return invalid(error.what());
      }
      return R::Success(true);
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
        if (!supported.Succeeded()) return R::Failure(supported.Error());
      }
      if (!type || type > 12 || (std::to_integer<unsigned>(bytes[i]) & 128)) return invalid("Unsupported AVC NAL type");
      i += size;
    }
    if (!picture) return invalid("AVC access unit contains no picture");
    if (needs_key && (!packet.key_frame || !idr)) return invalid("AVC requires an IDR access unit after creation/reset");
  }
  return R::Success(true);
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
    if (platform_storage && (!PlatformVideoSupported || video.output != PlatformVideoLayout))
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
    } else if (video.codec == CodecId::Av1) {
      try {
        const auto description = ParseAv1Configuration(video.initialization_data);
        if (platform_storage || !eight_bit || description.profile != 0 || !description.eight_bit_420 ||
            (description.sequence && (!description.sequence->sdr_420 || description.sequence->width > video.width ||
                                      description.sequence->height > video.height)))
          return R::Failure(Error(CodecErrorCode::Unsupported, "AV1 requires 8-bit SDR 4:2:0 with CPU NV12 or I420 output"));
      } catch (const std::invalid_argument& error) {
        return R::Failure(Error(CodecErrorCode::InvalidArgument, error.what()));
      }
    } else if (video.codec == CodecId::ProRes) {
#if defined(__APPLE__) && (TARGET_OS_OSX || TARGET_OS_IOS)
      if (!video.initialization_data.empty())
        return R::Failure(Error(CodecErrorCode::InvalidArgument, "ProRes does not use initialization data"));
      const bool p210 = TARGET_OS_OSX && video.output == VideoPixelFormat::P210;
      if (platform_storage || (!eight_bit && video.output != VideoPixelFormat::P010 && !p210))
        return R::Failure(Error(CodecErrorCode::Unsupported, "Unsupported ProRes CPU output layout"));
#else
      return R::Failure(Error(CodecErrorCode::Unsupported, "ProRes decoding is unavailable on this platform"));
#endif
    } else if (video.codec == CodecId::Vp8 || video.codec == CodecId::Vp9) {
      if (!eight_bit)
        return R::Failure(Error(CodecErrorCode::Unsupported, "VP8 and VP9 profile 0 require eight-bit output"));
      if (video.codec == CodecId::Vp8 && !video.initialization_data.empty())
        return R::Failure(Error(CodecErrorCode::InvalidArgument, "VP8 does not use initialization data"));
      if (video.codec == CodecId::Vp9) {
        if (video.initialization_data.size() > 65536)
          return R::Failure(Error(CodecErrorCode::InvalidArgument, "VP9 initialization data exceeds 64 KiB"));
        const auto level = Vp9Level(video.initialization_data);
        if (!level.Succeeded()) return R::Failure(level.Error());
      }
#if defined(__APPLE__)
      if (video.codec == CodecId::Vp8)
        return R::Failure(Error(CodecErrorCode::Unsupported, "VP8 decoding is unavailable on this platform"));
#endif
#if defined(__APPLE__) || defined(_WIN32)
      if (platform_storage)
        return R::Failure(Error(CodecErrorCode::Unsupported, "This VP8/VP9 adapter provides CPU output"));
#endif
    } else {
      return R::Failure(Error(CodecErrorCode::InvalidArgument, "Unsupported video decoder codec"));
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
