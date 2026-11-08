#include "codec/capabilities.h"

#include <codecapi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wmcodecdsp.h>
#include <wrl/client.h>

namespace huxerui::media::detail {
namespace {
using Microsoft::WRL::ComPtr;
struct MfDiscovery {
  bool com = false, mf = false;
  ~MfDiscovery() { Close(); }
  HRESULT Open() {
    auto status = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(status)) return status;
    com = true;
    status = MFStartup(MF_VERSION, MFSTARTUP_FULL);
    mf = SUCCEEDED(status);
    return status;
  }
  HRESULT Close() {
    const auto status = mf ? MFShutdown() : S_OK;
    mf = false;
    if (com) { CoUninitialize(); com = false; }
    return status;
  }
};
std::optional<std::uint32_t> UInt(const VARIANT& value) {
  if (value.vt == VT_UI4) return value.ulVal;
  if (value.vt == VT_I4 && value.lVal >= 0) return static_cast<std::uint32_t>(value.lVal);
  return std::nullopt;
}
std::optional<CodecUIntRange> PropertyRange(ICodecAPI* controls, const GUID& key) {
  VARIANT lower{}, upper{}, increment{};
  const auto status = controls->GetParameterRange(&key, &lower, &upper, &increment);
  const auto first = UInt(lower), last = UInt(upper), step = UInt(increment);
  VariantClear(&lower); VariantClear(&upper); VariantClear(&increment);
  if (FAILED(status) || !first || !last || !step || !*step || *first > *last) return std::nullopt;
  return CodecUIntRange{*first, *last, *step};
}
bool Allows(const CodecUIntRange& range, std::uint32_t value) {
  return value >= range.minimum && value <= range.maximum && (value - range.minimum) % range.step == 0;
}
std::vector<std::uint32_t> PropertyValues(ICodecAPI* controls, const GUID& key) {
  std::vector<std::uint32_t> result;
  struct Values {
    VARIANT* data = nullptr;
    ULONG count = 0;
    ~Values() {
      if (data) {
        for (ULONG i = 0; i < count; ++i) VariantClear(&data[i]);
        CoTaskMemFree(data);
      }
    }
  } values;
  if (SUCCEEDED(controls->GetParameterValues(&key, &values.data, &values.count)) && values.data)
    for (ULONG i = 0; i < std::min<ULONG>(values.count, 512); ++i)
      if (auto value = UInt(values.data[i])) result.push_back(*value);
  VARIANT value{};
  if (SUCCEEDED(controls->GetDefaultValue(&key, &value)))
    if (auto number = UInt(value)) result.push_back(*number);
  VariantClear(&value);
  if (SUCCEEDED(controls->GetValue(&key, &value)))
    if (auto number = UInt(value)) result.push_back(*number);
  VariantClear(&value);
  return result;
}
struct Types {
  bool pcm = false, nv12 = false;
  std::vector<std::uint32_t> rates, channels, profiles;
};
Types AvailableTypes(IMFTransform* transform, bool input, const GUID& compressed) {
  Types result;
  for (DWORD i = 0; i < 512; ++i) {
    ComPtr<IMFMediaType> type;
    const auto status = input ? transform->GetInputAvailableType(0, i, &type) : transform->GetOutputAvailableType(0, i, &type);
    if (FAILED(status)) break;
    GUID subtype{};
    if (FAILED(type->GetGUID(MF_MT_SUBTYPE, &subtype))) continue;
    result.pcm |= subtype == MFAudioFormat_PCM || subtype == MFAudioFormat_Float;
    result.nv12 |= subtype == MFVideoFormat_NV12;
    if (subtype != compressed) continue;
    UINT32 value = 0;
    if (SUCCEEDED(type->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &value)) && value) result.rates.push_back(value);
    if (SUCCEEDED(type->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &value)) && value) result.channels.push_back(value);
    if (SUCCEEDED(type->GetUINT32(MF_MT_MPEG2_PROFILE, &value))) result.profiles.push_back(value);
  }
  return result;
}
template<class T> void AudioTypes(T& result, const Types& compressed, bool pcm) {
  if (pcm) {
    SetSupport(result.sample_formats, CodecSupport::Supported);
    SetSupport(result.packings, CodecSupport::Supported);
  }
  // Available media types can be examples rather than an exhaustive enumeration. Only positive facts are used.
  for (auto& rate : result.sample_rates)
    if (std::find(compressed.rates.begin(), compressed.rates.end(), rate.value) != compressed.rates.end()) rate.support = CodecSupport::Supported;
  for (auto& layout : result.channel_layouts) {
    const auto channels = layout.value == AudioChannelLayout::Mono ? 1u : 2u;
    if (std::find(compressed.channels.begin(), compressed.channels.end(), channels) != compressed.channels.end())
      layout.support = CodecSupport::Supported;
  }
}
unsigned Profile(H264Profile profile) {
  return profile == H264Profile::Baseline ? 66 : profile == H264Profile::Main ? 77 : 100;
}
}
CapabilityResult QueryPlatformCapabilities(CapabilityKind kind) {
  auto snapshot = DefaultCapabilities(kind);
  MfDiscovery discovery;
  auto status = discovery.Open();
  if (FAILED(status)) return CapabilityResult::Failure(Error(CodecErrorCode::PlatformFailure, "Cannot initialize codec capability discovery", std::to_string(status)));
  const bool audio = kind == CapabilityKind::AudioDecoder || kind == CapabilityKind::AudioEncoder;
  const bool encoding = kind == CapabilityKind::AudioEncoder || kind == CapabilityKind::VideoEncoder;
  const auto& clsid = audio ? (encoding ? CLSID_AACMFTEncoder : CLSID_CMSAACDecMFT) :
      (encoding ? CLSID_CMSH264EncoderMFT : CLSID_CMSH264DecoderMFT);
  ComPtr<IMFTransform> transform;
  status = CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&transform));
  if (status == REGDB_E_CLASSNOTREG || status == CLASS_E_CLASSNOTAVAILABLE) {
    SetUnavailable(snapshot, "The system Media Foundation codec component is not installed");
  } else if (FAILED(status)) {
    return CapabilityResult::Failure(Error(CodecErrorCode::PlatformFailure, "Cannot inspect the system codec component", std::to_string(status)));
  } else {
    std::visit([](auto& value) { value.availability = CodecSupport::Supported; }, snapshot);
    const auto compressed = AvailableTypes(transform.Get(), !encoding, audio ? MFAudioFormat_AAC : MFVideoFormat_H264);
    const auto raw = AvailableTypes(transform.Get(), encoding, audio ? MFAudioFormat_AAC : MFVideoFormat_H264);
    if (auto* audio_decoder = std::get_if<AudioDecoderCapabilities>(&snapshot)) AudioTypes(*audio_decoder, compressed, raw.pcm);
    else if (auto* audio_encoder = std::get_if<AudioEncoderCapabilities>(&snapshot)) AudioTypes(*audio_encoder, compressed, raw.pcm);
    else if (auto* video_decoder = std::get_if<VideoDecoderCapabilities>(&snapshot)) {
      if (raw.nv12) SetSupport(video_decoder->pixel_formats, CodecSupport::Supported);
      for (auto& profile : video_decoder->profiles)
        if (std::find(compressed.profiles.begin(), compressed.profiles.end(), Profile(profile.value)) != compressed.profiles.end())
          profile.support = CodecSupport::Supported;
    } else {
      auto& value = std::get<VideoEncoderCapabilities>(snapshot);
      if (raw.nv12) SetSupport(value.pixel_formats, CodecSupport::Supported);
      for (auto& profile : value.profiles)
        if (std::find(compressed.profiles.begin(), compressed.profiles.end(), Profile(profile.profile)) != compressed.profiles.end())
          profile.support = CodecSupport::Supported;
    }
    ComPtr<ICodecAPI> controls;
    if (encoding && SUCCEEDED(transform.As(&controls))) {
      const auto bitrate = PropertyRange(controls.Get(), CODECAPI_AVEncCommonMeanBitRate);
      if (auto* audio_encoder = std::get_if<AudioEncoderCapabilities>(&snapshot)) audio_encoder->platform_limits.bitrate = bitrate;
      else {
        auto& value = std::get<VideoEncoderCapabilities>(snapshot);
        value.platform_limits.bitrate = bitrate;
        const auto profiles = PropertyValues(controls.Get(), CODECAPI_AVEncMPVProfile);
        const auto counts = PropertyValues(controls.Get(), CODECAPI_AVEncMPVDefaultBPictureCount);
        for (auto& profile : value.profiles) {
          if (std::find(profiles.begin(), profiles.end(), Profile(profile.profile)) != profiles.end())
            profile.support = CodecSupport::Supported;
          if (std::find(counts.begin(), counts.end(), 0) != counts.end()) profile.no_b_frames = CodecSupport::Supported;
          if (std::find(counts.begin(), counts.end(), profile.profile == H264Profile::Baseline ? 0u : 2u) != counts.end())
            profile.automatic_b_frames = CodecSupport::Supported;
          if (profile.profile != H264Profile::Baseline &&
              (std::find(counts.begin(), counts.end(), 1) != counts.end() || std::find(counts.begin(), counts.end(), 2) != counts.end())) {
            profile.bounded_b_frames = CodecSupport::Supported;
            if (std::find(counts.begin(), counts.end(), 2) != counts.end()) profile.maximum_b_frames = 2;
          }
        }
        const auto bframes = PropertyRange(controls.Get(), CODECAPI_AVEncMPVDefaultBPictureCount);
        if (bframes) for (auto& profile : value.profiles) {
          profile.no_b_frames = Allows(*bframes, 0) ? CodecSupport::Supported : CodecSupport::Unsupported;
          profile.automatic_b_frames = Allows(*bframes, profile.profile == H264Profile::Baseline ? 0 : 2)
              ? CodecSupport::Supported : CodecSupport::Unknown;
          if (profile.profile != H264Profile::Baseline) {
            profile.maximum_b_frames = Allows(*bframes, 2) ? 2 : Allows(*bframes, 1) ? 1 : 0;
            profile.bounded_b_frames = Allows(*bframes, 1) || Allows(*bframes, 2)
                ? CodecSupport::Supported : CodecSupport::Unsupported;
          }
        }
      }
    }
  }
  transform.Reset();
  status = discovery.Close();
  if (FAILED(status)) return CapabilityResult::Failure(Error(CodecErrorCode::PlatformFailure, "Cannot release capability discovery Media Foundation reference", std::to_string(status)));
  return CapabilityResult::Success(std::move(snapshot));
}
} // namespace huxerui::media::detail
