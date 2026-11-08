#include "codec/capabilities.h"

#include <emscripten/val.h>

namespace huxerui::media::detail {
namespace {
using emscripten::val;
CodecSupport Support(const val& value) {
  const auto status = value.as<int>();
  if (status < int(CodecSupport::Unknown) || status > int(CodecSupport::Supported))
    throw std::runtime_error("Invalid WebCodecs capability status");
  return static_cast<CodecSupport>(status);
}
template<class T> void Read(std::vector<CodecCapability<T>>& values, const val& source) {
  if (source["length"].as<std::size_t>() != values.size())
    throw std::runtime_error("Invalid WebCodecs capability candidate count");
  for (std::size_t i = 0; i < values.size(); ++i) values[i].support = Support(source[i]);
}
}
unsigned StartCapabilitiesQuery(CapabilityKind kind) {
  const auto id = val::global("HuxerMediaCodecCapabilities").call<unsigned>("open", int(kind));
  if (!id) throw std::runtime_error("Cannot allocate a WebCodecs capability query");
  return id;
}
std::optional<CapabilityResult> PollCapabilitiesQuery(unsigned id, CapabilityKind kind) {
  const auto report = val::global("HuxerMediaCodecCapabilities").call<val>("poll", id);
  if (report.isNull()) return std::nullopt;
  if (!report["error"].isUndefined())
    return CapabilityResult::Failure(Error(CodecErrorCode::PlatformFailure, report["error"].as<std::string>(), "WebCodecs"));
  auto snapshot = DefaultCapabilities(kind);
  const auto availability = Support(report["availability"]);
  if (availability == CodecSupport::Unsupported)
    SetUnavailable(snapshot, report["diagnostic"].as<std::string>());
  else std::visit([&](auto& result) {
    result.availability = availability;
    result.diagnostic = report["diagnostic"].as<std::string>();
    using T = std::decay_t<decltype(result)>;
    if constexpr (std::is_same_v<T, AudioDecoderCapabilities> || std::is_same_v<T, AudioEncoderCapabilities>) {
      SetSupport(result.sample_formats, Support(report["representations"]));
      SetSupport(result.packings, Support(report["representations"]));
      Read(result.sample_rates, report["rates"]); Read(result.channel_layouts, report["channels"]);
    } else {
      SetSupport(result.pixel_formats, Support(report["representations"]));
      if constexpr (std::is_same_v<T, VideoDecoderCapabilities>) Read(result.profiles, report["profiles"]);
      else for (std::size_t i = 0; i < result.profiles.size(); ++i) {
        auto& profile = result.profiles[i];
        profile.support = Support(report["profiles"][i]);
        profile.automatic_b_frames = profile.support;
        profile.no_b_frames = profile.profile == H264Profile::Baseline ? profile.support : CodecSupport::Unsupported;
        profile.bounded_b_frames = CodecSupport::Unsupported; profile.maximum_b_frames = 0;
      }
    }
  }, snapshot);
  return CapabilityResult::Success(std::move(snapshot));
}
CodecResult<void> CloseCapabilitiesQuery(unsigned id) {
  try {
    const auto error = val::global("HuxerMediaCodecCapabilities").call<std::string>("close", id);
    if (!error.empty()) return CodecResult<void>::Failure(Error(CodecErrorCode::PlatformFailure, error, "WebCodecs"));
    return CodecResult<void>::Success();
  } catch (const std::exception& error) {
    return CodecResult<void>::Failure(Error(CodecErrorCode::PlatformFailure, error.what(), "WebCodecs"));
  }
}
} // namespace huxerui::media::detail
