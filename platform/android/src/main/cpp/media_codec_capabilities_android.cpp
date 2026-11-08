#include "codec/capabilities.h"

#include <cstdlib>
#include <dlfcn.h>
#include <sys/system_properties.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaCodecInfo.h>
#include <media/NdkMediaFormat.h>

namespace huxerui::media::detail {
namespace {
template<class T> T Api(const char* name) { return reinterpret_cast<T>(dlsym(RTLD_DEFAULT, name)); }
template<class T> std::optional<CodecUIntRange> Range(const T* caps, media_status_t (*read)(const T*, AIntRange*)) {
  AIntRange range{};
  if (!read || read(caps, &range) != AMEDIA_OK || range.mLower < 0 || range.mUpper < range.mLower) return std::nullopt;
  return CodecUIntRange{std::uint32_t(range.mLower), std::uint32_t(range.mUpper), 1};
}
template<class T> void Audio(T& result, const AMediaCodecInfo* info) {
  auto get = Api<media_status_t (*)(const AMediaCodecInfo*, const ACodecAudioCapabilities**)>("AMediaCodecInfo_getAudioCapabilities");
  const ACodecAudioCapabilities* caps = nullptr;
  if (!get || get(info, &caps) != AMEDIA_OK || !caps) return;
  auto bitrate = Api<media_status_t (*)(const ACodecAudioCapabilities*, AIntRange*)>("ACodecAudioCapabilities_getBitrateRange");
  result.platform_limits.bitrate = Range(caps, bitrate);
  auto minimum = Api<int32_t (*)(const ACodecAudioCapabilities*)>("ACodecAudioCapabilities_getMinInputChannelCount");
  auto maximum = Api<int32_t (*)(const ACodecAudioCapabilities*)>("ACodecAudioCapabilities_getMaxInputChannelCount");
  if (minimum && maximum) {
    const auto first = minimum(caps), last = maximum(caps);
    if (first > 0 && last >= first) result.platform_limits.channels = CodecUIntRange{std::uint32_t(first), std::uint32_t(last), 1};
  }
  auto rates = Api<media_status_t (*)(const ACodecAudioCapabilities*, const AIntRange**, size_t*)>("ACodecAudioCapabilities_getSupportedSampleRateRanges");
  const AIntRange* ranges = nullptr; size_t count = 0;
  if (rates && rates(caps, &ranges, &count) == AMEDIA_OK && ranges && count && count <= 512) {
    auto first = ranges[0].mLower, last = ranges[0].mUpper;
    bool valid = first > 0 && last >= first;
    for (size_t i = 1; i < count; ++i) {
      valid &= ranges[i].mLower > 0 && ranges[i].mUpper >= ranges[i].mLower;
      first = std::min(first, ranges[i].mLower); last = std::max(last, ranges[i].mUpper);
    }
    if (valid) result.platform_limits.sample_rate = CodecUIntRange{std::uint32_t(first), std::uint32_t(last), 1};
  }
  auto supported = Api<int32_t (*)(const ACodecAudioCapabilities*, int32_t)>("ACodecAudioCapabilities_isSampleRateSupported");
  if (supported) for (auto& rate : result.sample_rates) {
    const auto status = supported(caps, rate.value);
    if (status >= 0) rate.support = status ? CodecSupport::Supported : CodecSupport::Unsupported;
  }
  auto channels = Api<media_status_t (*)(const ACodecAudioCapabilities*, const AIntRange**, size_t*)>("ACodecAudioCapabilities_getInputChannelCountRanges");
  ranges = nullptr; count = 0;
  if (channels && channels(caps, &ranges, &count) == AMEDIA_OK && ranges && count && count <= 512)
    for (auto& layout : result.channel_layouts) {
      const int wanted = layout.value == AudioChannelLayout::Mono ? 1 : 2;
      bool found = false;
      for (size_t i = 0; i < count; ++i) found |= wanted >= ranges[i].mLower && wanted <= ranges[i].mUpper;
      layout.support = found ? CodecSupport::Supported : CodecSupport::Unsupported;
    }
}
void Video(CodecVideoLimits& result, const AMediaCodecInfo* info) {
  auto get = Api<media_status_t (*)(const AMediaCodecInfo*, const ACodecVideoCapabilities**)>("AMediaCodecInfo_getVideoCapabilities");
  const ACodecVideoCapabilities* caps = nullptr;
  if (!get || get(info, &caps) != AMEDIA_OK || !caps) return;
  result.width = Range(caps, Api<media_status_t (*)(const ACodecVideoCapabilities*, AIntRange*)>("ACodecVideoCapabilities_getSupportedWidths"));
  result.height = Range(caps, Api<media_status_t (*)(const ACodecVideoCapabilities*, AIntRange*)>("ACodecVideoCapabilities_getSupportedHeights"));
  result.bitrate = Range(caps, Api<media_status_t (*)(const ACodecVideoCapabilities*, AIntRange*)>("ACodecVideoCapabilities_getBitrateRange"));
  if (auto rates = Range(caps, Api<media_status_t (*)(const ACodecVideoCapabilities*, AIntRange*)>("ACodecVideoCapabilities_getSupportedFrameRates")); rates && rates->maximum)
    result.maximum_frame_rate = rates->maximum;
  auto width = Api<int32_t (*)(const ACodecVideoCapabilities*)>("ACodecVideoCapabilities_getWidthAlignment");
  auto height = Api<int32_t (*)(const ACodecVideoCapabilities*)>("ACodecVideoCapabilities_getHeightAlignment");
  if (width) { const auto value = width(caps); if (value > 0) result.width_alignment = value; }
  if (height) { const auto value = height(caps); if (value > 0) result.height_alignment = value; }
}
bool Profile(const AMediaCodecInfo* info, H264Profile profile) {
  auto supported = Api<int32_t (*)(const AMediaCodecInfo*, const AMediaFormat*)>("AMediaCodecInfo_isFormatSupported");
  if (!supported) return false;
  std::unique_ptr<AMediaFormat, decltype(&AMediaFormat_delete)> format(AMediaFormat_new(), AMediaFormat_delete);
  if (!format) throw std::bad_alloc();
  AMediaFormat_setString(format.get(), "mime", "video/avc");
  AMediaFormat_setInt32(format.get(), "profile", profile == H264Profile::Baseline ? 1 : profile == H264Profile::Main ? 2 : 8);
  // A positive partial-format answer is useful evidence. A negative answer can require more context.
  return supported(info, format.get()) == 1;
}
}
CapabilityResult QueryPlatformCapabilities(CapabilityKind kind) {
  auto snapshot = DefaultCapabilities(kind);
  char version[PROP_VALUE_MAX]{};
  __system_property_get("ro.build.version.sdk", version);
  const auto api = std::strtol(version, nullptr, 10);
  const bool audio = kind == CapabilityKind::AudioDecoder || kind == CapabilityKind::AudioEncoder;
  const bool encoding = kind == CapabilityKind::AudioEncoder || kind == CapabilityKind::VideoEncoder;
  if (kind == CapabilityKind::VideoEncoder && api < 28) {
    SetUnavailable(snapshot, "The CPU video encoder adapter requires Android API 28 or later");
    return CapabilityResult::Success(std::move(snapshot));
  }
  if (auto* video = std::get_if<VideoEncoderCapabilities>(&snapshot); video && api < 29)
    for (auto& profile : video->profiles) if (profile.profile != H264Profile::Baseline) {
      profile.no_b_frames = profile.bounded_b_frames = CodecSupport::Unsupported;
      profile.maximum_b_frames = 0;
    }
  const char* mime = audio ? "audio/mp4a-latm" : "video/avc";
  std::unique_ptr<AMediaCodec, decltype(&AMediaCodec_delete)> codec(encoding ? AMediaCodec_createEncoderByType(mime) : AMediaCodec_createDecoderByType(mime), AMediaCodec_delete);
  if (!codec) {
    std::visit([](auto& value) { value.diagnostic = "Android could not create a matching component; availability and resource failure cannot be distinguished"; }, snapshot);
    return CapabilityResult::Success(std::move(snapshot));
  }
  std::visit([](auto& value) { value.availability = CodecSupport::Supported; }, snapshot);
  const AMediaCodecInfo* info = nullptr;
  auto get_name = Api<media_status_t (*)(AMediaCodec*, char**)>("AMediaCodec_getName");
  auto release_name = Api<void (*)(AMediaCodec*, char*)>("AMediaCodec_releaseName");
  auto get_info = Api<media_status_t (*)(const char*, const AMediaCodecInfo**)>("AMediaCodecStore_getCodecInfo");
  if (api >= 36 && get_name && release_name && get_info) {
    char* name = nullptr;
    const auto status = get_name(codec.get(), &name);
    if (status == AMEDIA_OK && name && get_info(name, &info) != AMEDIA_OK) info = nullptr;
    if (name) release_name(codec.get(), name);
  }
  if (!info) std::visit([](auto& value) { value.diagnostic = "Detailed NDK metadata is unavailable; unreported platform limits remain unknown"; }, snapshot);
  if (info) {
    auto type = Api<AMediaCodecType (*)(const AMediaCodecInfo*)>("AMediaCodecInfo_getMediaCodecInfoType");
    if (type) {
      const auto actual = type(info);
      std::visit([&](auto& value) {
        if (actual == AMediaCodecType_HARDWARE_ACCELERATED) value.hardware_acceleration = CodecSupport::Supported;
        else if (actual == AMediaCodecType_SOFTWARE_ONLY) value.hardware_acceleration = CodecSupport::Unsupported;
      }, snapshot);
    }
  }
  if (auto* decoder = std::get_if<AudioDecoderCapabilities>(&snapshot)) {
    SetSupport(decoder->sample_formats, CodecSupport::Supported); SetSupport(decoder->packings, CodecSupport::Supported);
    if (info) Audio(*decoder, info);
  } else if (auto* encoder = std::get_if<AudioEncoderCapabilities>(&snapshot)) {
    SetSupport(encoder->sample_formats, CodecSupport::Supported); SetSupport(encoder->packings, CodecSupport::Supported);
    if (info) Audio(*encoder, info);
  } else if (auto* decoder = std::get_if<VideoDecoderCapabilities>(&snapshot)) {
    if (info) {
      Video(decoder->platform_limits, info);
      for (auto& profile : decoder->profiles) if (Profile(info, profile.value)) profile.support = CodecSupport::Supported;
    }
  } else {
    auto& video = std::get<VideoEncoderCapabilities>(snapshot);
    if (info) Video(video.platform_limits, info);
    for (auto& profile : video.profiles) {
      if (info && Profile(info, profile.profile)) profile.support = CodecSupport::Supported;
      if (profile.profile == H264Profile::Baseline) profile.no_b_frames = profile.automatic_b_frames = CodecSupport::Supported;
    }
  }
  const auto status = AMediaCodec_delete(codec.release());
  if (status != AMEDIA_OK) return CapabilityResult::Failure(Error(CodecErrorCode::PlatformFailure, "Cannot release the Android capability discovery codec", std::to_string(status)));
  return CapabilityResult::Success(std::move(snapshot));
}
} // namespace huxerui::media::detail
