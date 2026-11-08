#include "codec/capabilities.h"

#include <cmath>
#import <AudioToolbox/AudioToolbox.h>
#import <VideoToolbox/VideoToolbox.h>

namespace huxerui::media::detail {
namespace {
struct CFValue {
  CFTypeRef value = nullptr;
  ~CFValue() { if (value) CFRelease(value); }
};
template<class T> std::optional<std::vector<T>> AudioValues(AudioFormatPropertyID property,
    UInt32 specifier_size = 0, const void* specifier = nullptr) {
  UInt32 size = 0;
  if (AudioFormatGetPropertyInfo(property, specifier_size, specifier, &size) ||
      size > 65536 || size % sizeof(T)) return std::nullopt;
  std::vector<T> values(size / sizeof(T));
  if (size && AudioFormatGetProperty(property, specifier_size, specifier, &size, values.data())) return std::nullopt;
  if (size > values.size() * sizeof(T) || size % sizeof(T)) return std::nullopt;
  values.resize(size / sizeof(T));
  return values;
}
std::optional<CodecUIntRange> Bounds(const std::vector<AudioValueRange>& ranges) {
  if (ranges.empty()) return std::nullopt;
  double first = UINT32_MAX, last = 0;
  for (const auto& range : ranges) {
    if (!std::isfinite(range.mMinimum) || !std::isfinite(range.mMaximum) ||
        range.mMinimum < 1 || range.mMaximum < range.mMinimum || range.mMaximum > UINT32_MAX)
      return std::nullopt;
    first = std::min(first, range.mMinimum); last = std::max(last, range.mMaximum);
  }
  if (std::ceil(first) > std::floor(last)) return std::nullopt;
  return CodecUIntRange{std::uint32_t(std::ceil(first)), std::uint32_t(std::floor(last)), 1};
}
template<class T> void AudioRepresentations(T& result) {
  SetSupport(result.sample_formats, CodecSupport::Supported);
  SetSupport(result.packings, CodecSupport::Supported);
  SetSupport(result.channel_layouts, CodecSupport::Supported);
}
void AudioEncoderLimits(AudioEncoderCapabilities& result) {
  const UInt32 format = kAudioFormatMPEG4AAC;
  if (const auto rates = AudioValues<AudioValueRange>(kAudioFormatProperty_AvailableEncodeSampleRates, sizeof(format), &format)) {
    result.platform_limits.sample_rate = Bounds(*rates);
    for (auto& rate : result.sample_rates) {
      const bool found = std::any_of(rates->begin(), rates->end(), [&](const auto& range) {
        return rate.value >= range.mMinimum && rate.value <= range.mMaximum;
      });
      if (result.platform_limits.sample_rate) rate.support = found ? CodecSupport::Supported : CodecSupport::Unsupported;
    }
  }
  if (const auto bitrates = AudioValues<AudioValueRange>(kAudioFormatProperty_AvailableEncodeBitRates, sizeof(format), &format))
    result.platform_limits.bitrate = Bounds(*bitrates);
}
void CompressionOutput(void*, void*, OSStatus, VTEncodeInfoFlags, CMSampleBufferRef) {}
struct CompressionSession {
  VTCompressionSessionRef value = nullptr;
  ~CompressionSession() { if (value) { VTCompressionSessionInvalidate(value); CFRelease(value); } }
};
void EncoderProfiles(VideoEncoderCapabilities& result) {
  for (auto& profile : result.profiles) {
    CompressionSession session;
    if (VTCompressionSessionCreate(kCFAllocatorDefault, 320, 240, kCMVideoCodecType_H264,
        nullptr, nullptr, nullptr, CompressionOutput, nullptr, &session.value) || !session.value) continue;
    const auto level = profile.profile == H264Profile::Baseline ? kVTProfileLevel_H264_Baseline_AutoLevel :
        profile.profile == H264Profile::Main ? kVTProfileLevel_H264_Main_AutoLevel : kVTProfileLevel_H264_High_AutoLevel;
    const auto no_reorder = VTSessionSetProperty(session.value, kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse);
    if (VTSessionSetProperty(session.value, kVTCompressionPropertyKey_ProfileLevel, level)) continue;
    profile.support = CodecSupport::Supported;
    if (!no_reorder) profile.no_b_frames = CodecSupport::Supported;
    if (profile.profile == H264Profile::Baseline) profile.automatic_b_frames = profile.no_b_frames;
    else if (!VTSessionSetProperty(session.value, kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanTrue))
      profile.automatic_b_frames = CodecSupport::Supported;
  }
}
CapabilityResult VideoEncoder(CapabilitySnapshot snapshot) {
  auto& result = std::get<VideoEncoderCapabilities>(snapshot);
  for (auto& profile : result.profiles) {
    profile.bounded_b_frames = CodecSupport::Unsupported; profile.maximum_b_frames = 0;
  }
  CFArrayRef encoders = nullptr;
  const auto status = VTCopyVideoEncoderList(nullptr, &encoders);
  CFValue owner{encoders};
  if (status || !encoders || CFArrayGetCount(encoders) > 4096)
    return CapabilityResult::Failure(Error(CodecErrorCode::PlatformFailure, "Cannot enumerate VideoToolbox encoders", std::to_string(status)));
  bool found = false, hardware = false, known = true, complete = true;
  for (CFIndex i = 0; i < CFArrayGetCount(encoders); ++i) {
    const auto entry = CFArrayGetValueAtIndex(encoders, i);
    if (!entry || CFGetTypeID(entry) != CFDictionaryGetTypeID()) { complete = false; continue; }
    const auto dictionary = static_cast<CFDictionaryRef>(entry);
    const auto type = static_cast<CFNumberRef>(CFDictionaryGetValue(dictionary, kVTVideoEncoderList_CodecType));
    std::int64_t codec = 0;
    if (!type || CFGetTypeID(type) != CFNumberGetTypeID() || !CFNumberGetValue(type, kCFNumberSInt64Type, &codec)) {
      complete = false; continue;
    }
    if (codec != kCMVideoCodecType_H264) continue;
    found = true;
    const auto accelerated = static_cast<CFBooleanRef>(CFDictionaryGetValue(dictionary, kVTVideoEncoderList_IsHardwareAccelerated));
    if (!accelerated || CFGetTypeID(accelerated) != CFBooleanGetTypeID()) known = false;
    else hardware |= CFBooleanGetValue(accelerated);
  }
  if (!found && complete) SetUnavailable(snapshot, "VideoToolbox does not advertise an H.264 encoder");
  else if (found) {
    result.availability = CodecSupport::Supported;
    if (hardware) result.hardware_acceleration = CodecSupport::Supported;
    else if (known && complete) result.hardware_acceleration = CodecSupport::Unsupported;
    SetSupport(result.pixel_formats, CodecSupport::Supported);
    EncoderProfiles(result);
    result.diagnostic = "Positive profile and reordering evidence uses 320x240 reference sessions; unreported platform limits remain unknown";
  } else result.diagnostic = "The VideoToolbox encoder list did not provide complete codec metadata";
  return CapabilityResult::Success(std::move(snapshot));
}
}
CapabilityResult QueryPlatformCapabilities(CapabilityKind kind) {
  @autoreleasepool {
    auto snapshot = DefaultCapabilities(kind);
    if (kind == CapabilityKind::VideoEncoder) return VideoEncoder(std::move(snapshot));
    if (auto* decoder = std::get_if<VideoDecoderCapabilities>(&snapshot)) {
      const bool hardware = VTIsHardwareDecodeSupported(kCMVideoCodecType_H264);
      decoder->hardware_acceleration = hardware ? CodecSupport::Supported : CodecSupport::Unsupported;
      if (hardware) {
        decoder->availability = CodecSupport::Supported;
        SetSupport(decoder->pixel_formats, CodecSupport::Supported);
      }
      decoder->diagnostic = "Hardware discovery does not enumerate software decoders, profiles or numeric limits";
      return CapabilityResult::Success(std::move(snapshot));
    }
    const bool encoding = kind == CapabilityKind::AudioEncoder;
    const auto formats = AudioValues<UInt32>(encoding ? kAudioFormatProperty_EncodeFormatIDs : kAudioFormatProperty_DecodeFormatIDs);
    if (!formats) return CapabilityResult::Failure(Error(CodecErrorCode::PlatformFailure, "Cannot enumerate AudioToolbox codec formats"));
    if (std::find(formats->begin(), formats->end(), kAudioFormatMPEG4AAC) == formats->end())
      SetUnavailable(snapshot, "AudioToolbox does not advertise AAC");
    else if (auto* encoder = std::get_if<AudioEncoderCapabilities>(&snapshot)) {
      encoder->availability = CodecSupport::Supported;
      AudioRepresentations(*encoder); AudioEncoderLimits(*encoder);
    } else {
      auto& audio = std::get<AudioDecoderCapabilities>(snapshot);
      audio.availability = CodecSupport::Supported; AudioRepresentations(audio);
    }
    return CapabilityResult::Success(std::move(snapshot));
  }
}
} // namespace huxerui::media::detail
