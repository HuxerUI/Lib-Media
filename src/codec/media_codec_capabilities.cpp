#include "codec/capabilities.h"

#include <array>

namespace huxerui::media {
namespace {
template<class T> void Query(detail::CapabilityKind kind, std::function<void(CodecResult<T>)> completion) {
  detail::RunCapabilitiesQuery(kind, [completion = std::move(completion)](detail::CapabilityResult result) mutable {
    if (!completion) return;
    if (!result.Succeeded()) completion(CodecResult<T>::Failure(std::move(result).Error()));
    else if (auto* value = std::get_if<T>(&result.Value())) completion(CodecResult<T>::Success(std::move(*value)));
    else completion(CodecResult<T>::Failure(detail::Error(CodecErrorCode::PlatformFailure, "Platform returned the wrong capability snapshot")));
  });
}
}
void AudioDecoder::QueryCapabilities(std::function<void(CodecResult<AudioDecoderCapabilities>)> completion) {
  Query(detail::CapabilityKind::AudioDecoder, std::move(completion));
}
void AudioDecoder::QueryCapabilities(CodecId codec, std::function<void(CodecResult<AudioDecoderCapabilities>)> completion) {
  if (codec == CodecId::Aac) {
    QueryCapabilities(std::move(completion));
    return;
  }
  auto audio = std::get<AudioDecoderCapabilities>(detail::DefaultCapabilities(detail::CapabilityKind::AudioDecoder));
  AudioDecoderConfig config{.output = {48000, AudioSampleFormat::S16}, .codec = codec};
  audio.sample_rates.clear();
  switch (codec) {
    case CodecId::Opus: {
      constexpr std::array<std::uint8_t, 19> head{'O','p','u','s','H','e','a','d',1,2,0,0,0x80,0xbb};
      const auto bytes = std::as_bytes(std::span(head));
      config.initialization_data.assign(bytes.begin(), bytes.end());
      audio.sample_rates = {{48000}};
      audio.library_limits.sample_rate = CodecUIntRange{48000, 48000, 1};
      break;
    }
    case CodecId::Flac:
      config.initialization_data.resize(34);
      config.initialization_data[0] = config.initialization_data[2] = std::byte{16};
      // STREAMINFO geometry is 48 kHz, two channels, 16 bits and a 4096-sample block.
      config.initialization_data[10] = std::byte{0x0b};
      config.initialization_data[11] = std::byte{0xb8};
      config.initialization_data[12] = std::byte{0x02};
      config.initialization_data[13] = std::byte{0xf0};
      audio.sample_rates = {{44100}, {48000}, {96000}, {192000}};
      audio.library_limits.sample_rate = CodecUIntRange{1, 655350, 1};
      break;
    case CodecId::Mp3:
      audio.sample_rates = {{8000}, {11025}, {12000}, {16000}, {22050}, {24000}, {32000}, {44100}, {48000}};
      audio.library_limits.sample_rate = CodecUIntRange{8000, 48000, 1};
      break;
    default: break;
  }
  Probe(config, [audio = std::move(audio), rate = config.output.sample_rate,
                 completion = std::move(completion)](CodecResult<void> result) mutable {
    if (completion) completion(detail::ApplyAudioDecoderProbe(std::move(audio), rate, std::move(result)));
  });
}
void VideoDecoder::QueryCapabilities(std::function<void(CodecResult<VideoDecoderCapabilities>)> completion) {
  Query(detail::CapabilityKind::VideoDecoder, std::move(completion));
}
void AudioEncoder::QueryCapabilities(std::function<void(CodecResult<AudioEncoderCapabilities>)> completion) {
  Query(detail::CapabilityKind::AudioEncoder, std::move(completion));
}
void VideoEncoder::QueryCapabilities(std::function<void(CodecResult<VideoEncoderCapabilities>)> completion) {
  Query(detail::CapabilityKind::VideoEncoder, std::move(completion));
}
} // namespace huxerui::media
