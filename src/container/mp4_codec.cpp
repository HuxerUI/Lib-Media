#include "mp4.h"

namespace huxerui::media::detail {
namespace {

unsigned HevcNalType(ByteView nal, ContainerErrorCode error) {
  if (nal.size() < 2) Fail(error, "Truncated HEVC NAL header");
  const auto header = Number<std::uint16_t>(nal);
  if ((header & 0x8000) || !(header & 7)) Fail(error, "Invalid HEVC NAL header");
  return (header >> 9) & 63;
}

} // namespace

unsigned HevcNalLength(ByteView config, bool hvc1, ContainerErrorCode error) {
  if (config.size() < 23 || config.size() > 65536 || Number<std::uint8_t>(config) != 1)
    Fail(error, "Invalid HEVCDecoderConfigurationRecord");
  const auto length = (Number<std::uint8_t>(config, 21) & 3u) + 1;
  if (length == 3) Fail(error, "Invalid HEVC NAL length size");

  auto arrays = config.subspan(23);
  std::uint64_t present = 0;
  for (unsigned i = 0, count = Number<std::uint8_t>(config, 22); i < count; ++i) {
    if (arrays.size() < 3) Fail(error, "Truncated HEVC configuration array");
    const auto flags = Number<std::uint8_t>(arrays);
    const auto type = flags & 63u;
    const auto nals = Number<std::uint16_t>(arrays, 1);
    const auto mask = std::uint64_t{1} << type;
    if (nals && (present & mask)) Fail(error, "Duplicate HEVC configuration array");
    if (nals) present |= mask;
    // hvc1 carries all parameter sets out of band. hev1 can also supply them in samples.
    if (hvc1 && type >= 32 && type <= 34 && !(flags & 128))
      Fail(error, "Incomplete hvc1 parameter-set array");
    arrays = arrays.subspan(3);
    for (unsigned j = 0; j < nals; ++j) {
      if (arrays.size() < 2) Fail(error, "Truncated HEVC configuration NAL length");
      const auto size = Number<std::uint16_t>(arrays);
      arrays = arrays.subspan(2);
      if (size > arrays.size() || HevcNalType(arrays.first(size), error) != type)
        Fail(error, "Invalid HEVC configuration NAL unit");
      arrays = arrays.subspan(size);
    }
  }
  constexpr auto ParameterSets = std::uint64_t{7} << 32;
  if (!arrays.empty() || (hvc1 && (present & ParameterSets) != ParameterSets))
    Fail(error, "Invalid or incomplete HEVC decoder configuration");
  return length;
}

bool HevcIdr(ByteView bytes, unsigned length_size, bool hvc1, ContainerErrorCode error) {
  std::uint64_t picture_layers = 0;
  bool idr = true;
  while (!bytes.empty()) {
    if (bytes.size() < length_size) Fail(error, "Truncated HEVC NAL length");
    std::uint32_t length = 0;
    for (const auto octet : bytes.first(length_size)) length = (length << 8) | std::to_integer<unsigned>(octet);
    bytes = bytes.subspan(length_size);
    if (length > bytes.size()) Fail(error, "Invalid HEVC sample NAL length");
    const auto nal = bytes.first(length);
    const auto type = HevcNalType(nal, error);
    if (hvc1 && type >= 32 && type <= 34) Fail(error, "hvc1 sample contains in-band parameter sets");
    if (type < 32) {
      if (nal.size() < 3) Fail(error, "Truncated HEVC slice header");
      const auto layer = (Number<std::uint16_t>(nal) >> 3) & 63;
      const auto mask = std::uint64_t{1} << layer;
      const bool first_slice = (Number<std::uint8_t>(nal, 2) & 128) != 0;
      const bool started = (picture_layers & mask) != 0;
      // An access unit can start one picture per layer. Remaining slices belong to that picture.
      if (first_slice && started) Fail(error, "HEVC sample contains more than one picture in a layer");
      if (!first_slice && !started) Fail(error, "HEVC sample begins inside a coded picture");
      if (first_slice) {
        picture_layers |= mask;
        idr &= type == 19 || type == 20;
      }
    }
    bytes = bytes.subspan(length);
  }
  if (!picture_layers) Fail(error, "HEVC sample has no coded picture");
  return idr;
}

bool ProResEntry(std::uint32_t tag) {
  switch (tag) {
    case Tag("apco"):
    case Tag("apcs"):
    case Tag("apcn"):
    case Tag("apch"):
    case Tag("ap4h"):
    case Tag("ap4x"): return true;
    default: return false;
  }
}

unsigned MovPcmBits(CodecId codec) {
  switch (codec) {
    case CodecId::PcmU8: return 8;
    case CodecId::PcmS16Le:
    case CodecId::PcmS16Be: return 16;
    case CodecId::PcmS24Le:
    case CodecId::PcmS24Be: return 24;
    case CodecId::PcmS32Le:
    case CodecId::PcmS32Be:
    case CodecId::PcmF32Le:
    case CodecId::PcmF32Be: return 32;
    default: return 0;
  }
}

bool MovPcmBigEndian(CodecId codec) {
  return codec == CodecId::PcmS16Be || codec == CodecId::PcmS24Be || codec == CodecId::PcmS32Be ||
         codec == CodecId::PcmF32Be;
}

} // namespace huxerui::media::detail
