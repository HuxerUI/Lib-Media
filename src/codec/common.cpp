#include "common.h"
#include "container/container_io.h"

#include <array>

namespace huxerui::media::detail {
CodecError Error(CodecErrorCode code, std::string message, std::string platform) {
  return {code, std::move(message), std::move(platform)};
}
BufferReference Own(Bytes bytes) {
  auto owner = std::make_shared<Bytes>(std::move(bytes));
  return BufferReference(*owner, owner);
}
CodecResult<AacConfig> ParseAac(std::span<const std::byte> bytes) {
  using R = CodecResult<AacConfig>;
  if (bytes.size() != 2) return R::Failure(Error(CodecErrorCode::Unsupported, "AAC-LC requires a two-byte AudioSpecificConfig"));
  const auto bits = (std::to_integer<unsigned>(bytes[0]) << 8) | std::to_integer<unsigned>(bytes[1]);
  const auto object = bits >> 11, rate = (bits >> 7) & 15, channels = (bits >> 3) & 15;
  constexpr std::array<std::uint32_t, 13> rates{96000,88200,64000,48000,44100,32000,24000,22050,16000,12000,11025,8000,7350};
  if (object != 2 || rate >= rates.size() || (channels != 1 && channels != 2) || (bits & 7))
    return R::Failure(Error(CodecErrorCode::Unsupported, "Only 1024-sample AAC-LC mono/stereo is supported"));
  return R::Success({rates[rate], channels});
}
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
bool IsProRes4444(std::string_view entry) {
  return entry == "ap4h" || entry == "ap4x";
}
std::uint32_t ProResCodecType(std::string_view entry) {
  if (entry.empty()) return Tag("apcn");
  if (entry == "apco" || entry == "apcs" || entry == "apcn" || entry == "apch" || IsProRes4444(entry))
    return Tag(entry);
  return 0;
}
CodecResult<void> ValidateProResFrame(ByteView bytes, std::uint32_t width, std::uint32_t height, bool chroma444) {
  using R = CodecResult<void>;
  if (bytes.size() < 36 || Number<std::uint32_t>(bytes) != bytes.size() || Number<std::uint32_t>(bytes, 4) != Tag("icpf"))
    return R::Failure(Error(CodecErrorCode::InvalidData, "Invalid ProRes frame size or identifier"));
  const auto header = Number<std::uint16_t>(bytes, 8);
  if (header < 20 || header > bytes.size() - 16 || Number<std::uint16_t>(bytes, 16) != width ||
      Number<std::uint16_t>(bytes, 18) != height)
    return R::Failure(Error(CodecErrorCode::InvalidData, "Invalid ProRes frame header or dimensions"));
  const auto flags = Number<std::uint8_t>(bytes, 20);
  if (Number<std::uint8_t>(bytes, 11) > 1)
    return R::Failure(Error(CodecErrorCode::Unsupported, "Unsupported ProRes bitstream version"));
  const auto alpha = Number<std::uint8_t>(bytes, 25) & 0x0f;
  if (flags >> 6 != (chroma444 ? 3 : 2) || (flags & 0x0c) || alpha > (chroma444 ? 2 : 0))
    return R::Failure(Error(CodecErrorCode::Unsupported, "ProRes frame chroma or alpha differs from its progressive profile"));
  return R::Success();
}
} // namespace huxerui::media::detail
