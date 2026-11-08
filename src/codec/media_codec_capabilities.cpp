#include "codec/capabilities.h"

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
