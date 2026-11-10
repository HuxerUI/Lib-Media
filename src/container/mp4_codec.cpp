#include "mp4.h"
#include "../codec/hevc.h"

namespace huxerui::media::detail {
unsigned HevcNalLength(ByteView config, bool hvc1, ContainerErrorCode error) {
  try {
    return ParseHevcConfiguration(config, hvc1, false).length;
  } catch (const std::invalid_argument& failure) {
    Fail(error, failure.what());
  }
}

bool HevcIdr(ByteView bytes, unsigned length_size, bool hvc1, ContainerErrorCode error) {
  try {
    return IsHevcIdr(bytes, length_size, hvc1, false);
  } catch (const std::invalid_argument& failure) {
    Fail(error, failure.what());
  }
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
