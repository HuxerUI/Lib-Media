#pragma once

#include "common.h"
#include <algorithm>

namespace huxerui::media::detail {
enum class CapabilityKind { AudioDecoder, VideoDecoder, AudioEncoder, VideoEncoder };
using CapabilitySnapshot = std::variant<AudioDecoderCapabilities, VideoDecoderCapabilities,
    AudioEncoderCapabilities, VideoEncoderCapabilities>;
using CapabilityResult = CodecResult<CapabilitySnapshot>;
using CapabilityCompletion = std::function<void(CapabilityResult)>;

// Candidates and library bounds come from the existing codec contracts; platform evidence starts unknown.
CapabilitySnapshot DefaultCapabilities(CapabilityKind kind);
void SetUnavailable(CapabilitySnapshot& snapshot, std::string diagnostic);
CodecResult<AudioDecoderCapabilities> ApplyAudioDecoderProbe(
    AudioDecoderCapabilities snapshot, std::uint32_t sample_rate, CodecResult<void> result);

template<class T> void SetSupport(std::vector<CodecCapability<T>>& values, CodecSupport support) {
  for (auto& value : values) value.support = support;
}
#ifdef __EMSCRIPTEN__
// JS owns late promises. These calls stay on the event loop and never invoke application callbacks.
unsigned StartCapabilitiesQuery(CapabilityKind kind);
std::optional<CapabilityResult> PollCapabilitiesQuery(unsigned id, CapabilityKind kind);
CodecResult<void> CloseCapabilitiesQuery(unsigned id);
void RunCapabilitiesQuery(CapabilityKind kind, CapabilityCompletion completion);
#else
// Platform calls and all temporary platform objects are confined to the query worker.
CapabilityResult QueryPlatformCapabilities(CapabilityKind kind);
using CapabilitiesQuery = std::function<CapabilityResult(CapabilityKind)>;
void RunCapabilitiesQuery(CapabilityKind kind, CapabilityCompletion completion,
    CapabilitiesQuery query = QueryPlatformCapabilities);
#endif
} // namespace huxerui::media::detail
