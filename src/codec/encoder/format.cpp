#include "format.h"

#include <limits>

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
} // namespace huxerui::media::detail
