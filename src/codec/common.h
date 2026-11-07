#pragma once

#include <huxerui/media_codec.h>

namespace huxerui::media::detail {
struct AacConfig {
  std::uint32_t sample_rate;
  std::uint32_t channels;
};

CodecError Error(CodecErrorCode code, std::string message, std::string platform = {});
BufferReference Own(Bytes bytes);
// Reads the shared two-byte AAC-LC description used by encoder output and decoder input.
CodecResult<AacConfig> ParseAac(std::span<const std::byte> bytes);
} // namespace huxerui::media::detail
