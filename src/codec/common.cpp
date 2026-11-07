#include "common.h"

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
} // namespace huxerui::media::detail
