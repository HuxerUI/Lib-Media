#include "capabilities.h"
#include "execution/executor.h"

namespace huxerui::media::detail {
namespace {
template<class T> std::vector<CodecCapability<T>> Candidates(std::initializer_list<T> values) {
  std::vector<CodecCapability<T>> result;
  for (auto value : values) result.push_back({value, CodecSupport::Unknown});
  return result;
}
template<class T> void AudioCandidates(T& result, bool encoding) {
  result.sample_formats = encoding ? Candidates({AudioSampleFormat::S16}) :
      Candidates({AudioSampleFormat::S16, AudioSampleFormat::F32});
  result.packings = encoding ? Candidates({AudioPacking::Interleaved}) :
      Candidates({AudioPacking::Interleaved, AudioPacking::Planar});
  result.channel_layouts = Candidates({AudioChannelLayout::Mono, AudioChannelLayout::Stereo});
  result.sample_rates = encoding ? Candidates<std::uint32_t>({44100, 48000}) :
      Candidates<std::uint32_t>({96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350});
  result.library_limits.sample_rate = encoding ? CodecUIntRange{44100, 48000, 3900} : CodecUIntRange{7350, 96000, 1};
  result.library_limits.channels = CodecUIntRange{1, 2, 1};
  if (encoding) result.library_limits.bitrate = CodecUIntRange{1, INT32_MAX, 1};
}
struct QueryState : std::enable_shared_from_this<QueryState> {
  CapabilityKind kind;
  CapabilityCompletion completion;
#ifdef __EMSCRIPTEN__
  unsigned id = 0;
#else
  CapabilitiesQuery query;
  std::shared_ptr<MediaWorker> worker;
#endif
  void Schedule(unsigned delay = 0) {
#ifdef __EMSCRIPTEN__
    auto self = shared_from_this();
    DispatchMedia([self] { self->Run(); }, delay);
#else
    worker->Schedule(delay);
#endif
  }
  void Complete(CapabilityResult result) {
    auto callback = [completion = std::move(completion), result = std::move(result)]() mutable {
      if (completion) completion(std::move(result));
    };
#ifdef __EMSCRIPTEN__
    callback();
#else
    query = {};
    worker->Finish(std::move(callback));
#endif
  }
  void Run() {
    std::optional<CapabilityResult> result;
    try {
#ifdef __EMSCRIPTEN__
      if (!id) id = StartCapabilitiesQuery(kind);
      result = PollCapabilitiesQuery(id, kind);
      if (!result) { Schedule(2); return; }
#else
      result = query(kind);
#endif
    } catch (const std::bad_alloc&) {
      result = CapabilityResult::Failure(Error(CodecErrorCode::ResourceExhausted, "Cannot allocate codec capability snapshot"));
    } catch (const std::exception& error) {
      result = CapabilityResult::Failure(Error(CodecErrorCode::PlatformFailure, error.what()));
    }
#ifdef __EMSCRIPTEN__
    if (id) {
      const auto closed = CloseCapabilitiesQuery(std::exchange(id, 0));
      if (!closed.Succeeded()) result = CapabilityResult::Failure(closed.Error());
    }
#endif
    Complete(std::move(*result));
  }
};
}
CapabilitySnapshot DefaultCapabilities(CapabilityKind kind) {
  if (kind == CapabilityKind::AudioDecoder) {
    AudioDecoderCapabilities result;
    AudioCandidates(result, false);
    return result;
  }
  if (kind == CapabilityKind::AudioEncoder) {
    AudioEncoderCapabilities result;
    AudioCandidates(result, true);
    return result;
  }
  if (kind == CapabilityKind::VideoDecoder) {
    VideoDecoderCapabilities result;
    result.pixel_formats = Candidates({VideoPixelFormat::I420, VideoPixelFormat::NV12});
    result.profiles = Candidates({H264Profile::Baseline, H264Profile::Main, H264Profile::High});
    result.library_limits.width = CodecUIntRange{1, 8192, 1};
    result.library_limits.height = CodecUIntRange{1, 8192, 1};
    return result;
  }
  VideoEncoderCapabilities result;
  result.pixel_formats = Candidates({VideoPixelFormat::I420, VideoPixelFormat::NV12});
  for (auto profile : {H264Profile::Baseline, H264Profile::Main, H264Profile::High}) {
    H264EncoderProfileCapabilities value;
    value.profile = profile;
    if (profile == H264Profile::Baseline) { value.bounded_b_frames = CodecSupport::Unsupported; value.maximum_b_frames = 0; }
    result.profiles.push_back(value);
  }
  result.library_limits.width = CodecUIntRange{2, 8192, 2};
  result.library_limits.height = CodecUIntRange{2, 8192, 2};
  result.library_limits.width_alignment = result.library_limits.height_alignment = 2;
  result.library_limits.maximum_frame_rate = 240;
  result.library_limits.bitrate = CodecUIntRange{1, INT32_MAX, 1};
  return result;
}
void SetUnavailable(CapabilitySnapshot& snapshot, std::string diagnostic) {
  std::visit([&](auto& value) {
    value.availability = value.hardware_acceleration = CodecSupport::Unsupported;
    value.diagnostic = std::move(diagnostic);
    using T = std::decay_t<decltype(value)>;
    if constexpr (std::is_same_v<T, AudioDecoderCapabilities> || std::is_same_v<T, AudioEncoderCapabilities>) {
      SetSupport(value.sample_formats, CodecSupport::Unsupported);
      SetSupport(value.packings, CodecSupport::Unsupported);
      SetSupport(value.channel_layouts, CodecSupport::Unsupported);
      SetSupport(value.sample_rates, CodecSupport::Unsupported);
    } else {
      SetSupport(value.pixel_formats, CodecSupport::Unsupported);
      if constexpr (std::is_same_v<T, VideoDecoderCapabilities>) SetSupport(value.profiles, CodecSupport::Unsupported);
      else for (auto& profile : value.profiles) {
        profile.support = profile.no_b_frames = profile.automatic_b_frames = profile.bounded_b_frames = CodecSupport::Unsupported;
        profile.maximum_b_frames = 0;
      }
    }
  }, snapshot);
}
CodecResult<AudioDecoderCapabilities> ApplyAudioDecoderProbe(
    AudioDecoderCapabilities snapshot, std::uint32_t sample_rate, CodecResult<void> result) {
  using R = CodecResult<AudioDecoderCapabilities>;
  if (result.Succeeded()) {
    snapshot.availability = CodecSupport::Supported;
    const auto rate = std::ranges::find(snapshot.sample_rates, sample_rate, &CodecCapability<std::uint32_t>::value);
    if (rate != snapshot.sample_rates.end()) rate->support = CodecSupport::Supported;
  } else {
    auto error = std::move(result).Error();
    if (error.code == CodecErrorCode::InvalidArgument) return R::Failure(std::move(error));
    // A rejected configuration says nothing about other rates, layouts, or bitstreams.
    snapshot.diagnostic = std::move(error.message);
  }
  return R::Success(std::move(snapshot));
}
#ifdef __EMSCRIPTEN__
void RunCapabilitiesQuery(CapabilityKind kind, CapabilityCompletion completion) {
#else
void RunCapabilitiesQuery(CapabilityKind kind, CapabilityCompletion completion, CapabilitiesQuery query) {
#endif
  auto state = std::make_shared<QueryState>();
  state->kind = kind; state->completion = std::move(completion);
#ifndef __EMSCRIPTEN__
  state->query = std::move(query);
  state->worker = MediaWorker::Start([state](bool) { state->Run(); });
#endif
  state->Schedule();
}
} // namespace huxerui::media::detail
