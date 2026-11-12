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
struct AvcConfig {
  unsigned length_size;
  std::vector<Bytes> parameters;
};
// Reads the shared bounded avcC description used by encoder output and decoder input.
CodecResult<AvcConfig> ParseAvc(std::span<const std::byte> bytes);
// Checks the frame header used by progressive ProRes 422 decoder input and encoder output.
CodecResult<void> ValidateProResFrame(std::span<const std::byte> bytes, std::uint32_t width, std::uint32_t height);
} // namespace huxerui::media::detail
