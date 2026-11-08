#include <huxerui/media_codec.h>

#include <chrono>
#include <cmath>
#include <iostream>
#include <mutex>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#else
#include <thread>
#endif

using namespace huxerui::media;
namespace {
void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class T> struct Reply {
  std::mutex mutex;
  std::optional<CodecResult<T>> result;
  int calls = 0;
  void Receive(CodecResult<T> value) {
    std::lock_guard lock(mutex);
    ++calls;
    if (!result) result = std::move(value);
  }
  bool Ready() { std::lock_guard lock(mutex); return result.has_value(); }
  T Take() {
    std::lock_guard lock(mutex);
    Check(calls == 1, "A capability query did not complete exactly once");
    if (!result->Succeeded()) throw std::runtime_error(result->Error().message);
    return std::move(*result).Value();
  }
};
void Range(const std::optional<CodecUIntRange>& range) {
  if (range) Check(range->maximum >= range->minimum && range->step > 0, "Invalid capability range");
}
void Limits(const CodecAudioLimits& value) { Range(value.sample_rate); Range(value.channels); Range(value.bitrate); }
void Limits(const CodecVideoLimits& value) {
  Range(value.width); Range(value.height); Range(value.bitrate);
  if (value.width_alignment) Check(*value.width_alignment > 0, "Invalid width alignment");
  if (value.height_alignment) Check(*value.height_alignment > 0, "Invalid height alignment");
  if (value.maximum_frame_rate) Check(std::isfinite(*value.maximum_frame_rate) && *value.maximum_frame_rate > 0, "Invalid frame-rate bound");
}
template<class T> void Candidates(const std::vector<CodecCapability<T>>& values, CodecSupport availability) {
  Check(!values.empty(), "Capability candidates were omitted");
  if (availability == CodecSupport::Unsupported) for (const auto& value : values)
    Check(value.support == CodecSupport::Unsupported, "Unavailable component advertised usable candidates");
}
template<class T> void Audio(const T& value) {
  Limits(value.library_limits); Limits(value.platform_limits);
  Candidates(value.sample_formats, value.availability); Candidates(value.packings, value.availability);
  Candidates(value.sample_rates, value.availability); Candidates(value.channel_layouts, value.availability);
}
const char* Support(CodecSupport value) {
  return value == CodecSupport::Supported ? "supported" : value == CodecSupport::Unsupported ? "unsupported" : "unknown";
}
}
int main() {
  try {
    Reply<AudioDecoderCapabilities> audio_decoder;
    Reply<VideoDecoderCapabilities> video_decoder;
    Reply<AudioEncoderCapabilities> audio_encoder;
    Reply<VideoEncoderCapabilities> video_encoder;
    AudioDecoder::QueryCapabilities([&](auto result) { audio_decoder.Receive(std::move(result)); });
    VideoDecoder::QueryCapabilities([&](auto result) { video_decoder.Receive(std::move(result)); });
    AudioEncoder::QueryCapabilities([&](auto result) { audio_encoder.Receive(std::move(result)); });
    VideoEncoder::QueryCapabilities([&](auto result) { video_encoder.Receive(std::move(result)); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(25);
    while (!audio_decoder.Ready() || !video_decoder.Ready() || !audio_encoder.Ready() || !video_encoder.Ready()) {
      Check(std::chrono::steady_clock::now() < deadline, "Capability query timed out without a UI runtime");
#ifdef __EMSCRIPTEN__
      emscripten_sleep(1);
#else
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
#endif
    }
    auto ad = audio_decoder.Take(); const auto ae = audio_encoder.Take();
    const auto vd = video_decoder.Take(); const auto ve = video_encoder.Take();
    Audio(ad); Audio(ae);
    Limits(vd.library_limits); Limits(vd.platform_limits);
    Limits(ve.library_limits); Limits(ve.platform_limits);
    Candidates(vd.pixel_formats, vd.availability); Candidates(vd.profiles, vd.availability);
    Candidates(ve.pixel_formats, ve.availability);
    Check(ae.sample_rates.size() == 2 && ae.sample_formats.size() == 1, "Query widened the AAC encoder contract");
    Check(vd.profiles.size() == 3 && ve.profiles.size() == 3, "H.264 profile candidates are incomplete");
    for (const auto& profile : ve.profiles) {
      if (profile.profile == H264Profile::Baseline)
        Check(profile.bounded_b_frames == CodecSupport::Unsupported && profile.maximum_b_frames == 0,
            "Baseline admitted a positive B-picture bound");
      if (profile.maximum_b_frames) Check(*profile.maximum_b_frames <= 16, "B-picture ceiling exceeds the adapter contract");
    }
    auto retained = ad;
    ad.sample_rates.clear();
    Check(!retained.sample_rates.empty(), "Capability snapshots share mutable candidate storage");
    std::cout << "AAC decode: " << Support(ad.availability) << "; AAC encode: " << Support(ae.availability)
        << "; H.264 decode: " << Support(vd.availability) << "; H.264 encode: " << Support(ve.availability) << '\n';
    for (const auto& profile : ve.profiles)
      std::cout << "H.264 profile " << int(profile.profile) << ": " << Support(profile.support)
          << "; no B: " << Support(profile.no_b_frames) << "; auto B: " << Support(profile.automatic_b_frames)
          << "; bounded B: " << Support(profile.bounded_b_frames) << '\n';
    std::cout << "Codec capability queries passed without Runtime or Install\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
