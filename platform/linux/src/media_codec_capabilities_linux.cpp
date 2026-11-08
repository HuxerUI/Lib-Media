#include "codec/capabilities.h"

#include <cmath>
#include <numeric>
#include <gst/gst.h>

namespace huxerui::media::detail {
namespace {
using Element = std::unique_ptr<GstElement, decltype(&gst_object_unref)>;
using Caps = std::unique_ptr<GstCaps, decltype(&gst_caps_unref)>;
Caps PadCaps(GstElement* element, GstPadDirection direction) {
  auto* result = gst_caps_new_empty();
  auto* factory = gst_element_get_factory(element);
  for (auto* item = gst_element_factory_get_static_pad_templates(factory); item; item = item->next) {
    auto* pad = static_cast<GstStaticPadTemplate*>(item->data);
    if (pad->direction == direction) gst_caps_append(result, gst_static_pad_template_get_caps(pad));
  }
  return {result, gst_caps_unref};
}
std::optional<CodecUIntRange> IntBounds(const GValue* value) {
  if (!value) return std::nullopt;
  int first = -1, last = -1, step = 1;
  if (G_VALUE_HOLDS_INT(value)) first = last = g_value_get_int(value);
  else if (GST_VALUE_HOLDS_INT_RANGE(value)) {
    first = gst_value_get_int_range_min(value); last = gst_value_get_int_range_max(value);
    step = gst_value_get_int_range_step(value);
  } else if (GST_VALUE_HOLDS_LIST(value)) {
    std::optional<CodecUIntRange> result;
    for (guint i = 0; i < gst_value_list_get_size(value); ++i) {
      const auto next = IntBounds(gst_value_list_get_value(value, i));
      if (!next) return std::nullopt;
      if (!result) result = next;
      else {
        const auto difference = result->minimum > next->minimum ? result->minimum - next->minimum : next->minimum - result->minimum;
        result->step = std::gcd(std::gcd(result->step, next->step), difference);
        result->minimum = std::min(result->minimum, next->minimum); result->maximum = std::max(result->maximum, next->maximum);
      }
    }
    return result;
  }
  if (first < 0 || last < first || step < 1) return std::nullopt;
  return CodecUIntRange{std::uint32_t(first), std::uint32_t(last), std::uint32_t(step)};
}
std::optional<CodecUIntRange> Bounds(GstCaps* caps, const char* name) {
  std::optional<CodecUIntRange> result;
  for (guint i = 0; i < gst_caps_get_size(caps); ++i) {
    const auto next = IntBounds(gst_structure_get_value(gst_caps_get_structure(caps, i), name));
    if (!next) return std::nullopt;
    if (!result) result = next;
    else {
      const auto difference = result->minimum > next->minimum ? result->minimum - next->minimum : next->minimum - result->minimum;
      result->step = std::gcd(std::gcd(result->step, next->step), difference);
      result->minimum = std::min(result->minimum, next->minimum); result->maximum = std::max(result->maximum, next->maximum);
    }
  }
  return result;
}
std::optional<CodecUIntRange> Property(GstElement* element, const char* name, unsigned multiplier = 1) {
  auto* property = g_object_class_find_property(G_OBJECT_GET_CLASS(element), name);
  if (!property) return std::nullopt;
  std::uint64_t first, last;
  if (G_IS_PARAM_SPEC_UINT(property)) {
    first = G_PARAM_SPEC_UINT(property)->minimum; last = G_PARAM_SPEC_UINT(property)->maximum;
  } else if (G_IS_PARAM_SPEC_INT(property) && G_PARAM_SPEC_INT(property)->minimum >= 0) {
    first = G_PARAM_SPEC_INT(property)->minimum; last = G_PARAM_SPEC_INT(property)->maximum;
  } else return std::nullopt;
  if (last > UINT32_MAX / multiplier) return std::nullopt;
  return CodecUIntRange{std::uint32_t(first * multiplier), std::uint32_t(last * multiplier), multiplier};
}
Element Decoder(GstCaps* input) {
  const bool audio = gst_structure_has_name(gst_caps_get_structure(input, 0), "audio/mpeg");
  auto* factories = gst_element_factory_list_get_elements(GST_ELEMENT_FACTORY_TYPE_DECODER |
      (audio ? GST_ELEMENT_FACTORY_TYPE_MEDIA_AUDIO : GST_ELEMENT_FACTORY_TYPE_MEDIA_VIDEO), GST_RANK_MARGINAL);
  auto* compatible = gst_element_factory_list_filter(factories, input, GST_PAD_SINK, FALSE);
  GstElement* result = nullptr;
  for (auto* item = compatible; item && !result; item = item->next)
    result = gst_element_factory_create(GST_ELEMENT_FACTORY(item->data), nullptr);
  gst_plugin_feature_list_free(compatible); gst_plugin_feature_list_free(factories);
  return {result, gst_object_unref};
}
template<class T> void Audio(T& result, GstCaps* raw_or_compressed, bool encoding) {
  SetSupport(result.sample_formats, CodecSupport::Supported);
  SetSupport(result.packings, CodecSupport::Supported);
  result.platform_limits.sample_rate = Bounds(raw_or_compressed, "rate");
  result.platform_limits.channels = Bounds(raw_or_compressed, "channels");
  auto accepts = [&](const char* key, int value) {
    Caps candidate(encoding ? gst_caps_new_simple("audio/x-raw", key, G_TYPE_INT, value, nullptr) :
        gst_caps_new_simple("audio/mpeg", "mpegversion", G_TYPE_INT, 4, "stream-format", G_TYPE_STRING, "raw", key, G_TYPE_INT, value, nullptr), gst_caps_unref);
    return gst_caps_can_intersect(candidate.get(), raw_or_compressed);
  };
  for (auto& rate : result.sample_rates)
    rate.support = accepts("rate", rate.value) ? CodecSupport::Supported : CodecSupport::Unknown;
  for (auto& layout : result.channel_layouts)
    layout.support = accepts("channels", layout.value == AudioChannelLayout::Mono ? 1 : 2) ? CodecSupport::Supported : CodecSupport::Unknown;
}
bool Profile(GstCaps* caps, H264Profile profile) {
  const auto* name = profile == H264Profile::Baseline ? "baseline" : profile == H264Profile::Main ? "main" : "high";
  Caps candidate(gst_caps_new_simple("video/x-h264", "profile", G_TYPE_STRING, name, nullptr), gst_caps_unref);
  if (gst_caps_can_intersect(caps, candidate.get())) return true;
  if (profile != H264Profile::Baseline) return false;
  gst_caps_set_simple(candidate.get(), "profile", G_TYPE_STRING, "constrained-baseline", nullptr);
  return gst_caps_can_intersect(caps, candidate.get());
}
void VideoBounds(CodecVideoLimits& result, GstCaps* caps) {
  result.width = Bounds(caps, "width"); result.height = Bounds(caps, "height");
  std::optional<double> rate;
  for (guint i = 0; i < gst_caps_get_size(caps); ++i) {
    const auto* value = gst_structure_get_value(gst_caps_get_structure(caps, i), "framerate");
    if (!value) return;
    if (GST_VALUE_HOLDS_FRACTION_RANGE(value)) value = gst_value_get_fraction_range_max(value);
    if (!GST_VALUE_HOLDS_FRACTION(value)) return;
    const auto maximum = double(gst_value_get_fraction_numerator(value)) / gst_value_get_fraction_denominator(value);
    if (!std::isfinite(maximum) || maximum <= 0) return;
    rate = std::max(rate.value_or(0), maximum);
  }
  result.maximum_frame_rate = rate;
}
}
CapabilityResult QueryPlatformCapabilities(CapabilityKind kind) {
  auto snapshot = DefaultCapabilities(kind);
  if (auto* video = std::get_if<VideoEncoderCapabilities>(&snapshot))
    video->library_limits.bitrate = CodecUIntRange{1000, 2147483000u, 1000};
  GError* error = nullptr;
  if (!gst_init_check(nullptr, nullptr, &error)) {
    const auto text = std::string(error ? error->message : "Cannot initialize GStreamer capability discovery");
    if (error) g_error_free(error);
    return CapabilityResult::Failure(Error(CodecErrorCode::PlatformFailure, text));
  }
  const bool audio = kind == CapabilityKind::AudioDecoder || kind == CapabilityKind::AudioEncoder;
  const bool encoding = kind == CapabilityKind::AudioEncoder || kind == CapabilityKind::VideoEncoder;
  for (const char* name : {"appsrc", "appsink", audio ? "aacparse" : "h264parse",
      audio ? "audioconvert" : encoding ? "x264enc" : "videoconvert"}) {
    Element required(gst_element_factory_make(name, nullptr), gst_object_unref);
    if (!required) { SetUnavailable(snapshot, std::string("Required GStreamer element is unavailable: ") + name); return CapabilityResult::Success(std::move(snapshot)); }
  }
  Caps input(audio ? gst_caps_new_simple("audio/mpeg", "mpegversion", G_TYPE_INT, 4, "stream-format", G_TYPE_STRING, "raw", nullptr) :
      gst_caps_new_simple("video/x-h264", "stream-format", G_TYPE_STRING, "avc", "alignment", G_TYPE_STRING, "au", nullptr), gst_caps_unref);
  Element element = encoding ? Element(gst_element_factory_make(audio ? "avenc_aac" : "x264enc", nullptr), gst_object_unref) : Decoder(input.get());
  if (!element) { SetUnavailable(snapshot, "No GStreamer component is available to the current codec adapter"); return CapabilityResult::Success(std::move(snapshot)); }
  std::visit([](auto& value) { value.availability = CodecSupport::Supported; }, snapshot);
  const auto* klass = gst_element_factory_get_metadata(gst_element_get_factory(element.get()), GST_ELEMENT_METADATA_KLASS);
  if (klass && std::string_view(klass).find("Hardware") != std::string_view::npos)
    std::visit([](auto& value) { value.hardware_acceleration = CodecSupport::Supported; }, snapshot);
  auto caps = PadCaps(element.get(), GST_PAD_SINK);
  if (auto* audio_decoder = std::get_if<AudioDecoderCapabilities>(&snapshot)) Audio(*audio_decoder, caps.get(), false);
  else if (auto* audio_encoder = std::get_if<AudioEncoderCapabilities>(&snapshot)) {
    Audio(*audio_encoder, caps.get(), true);
    audio_encoder->platform_limits.bitrate = Property(element.get(), "bitrate");
  } else if (auto* video_decoder = std::get_if<VideoDecoderCapabilities>(&snapshot)) {
    SetSupport(video_decoder->pixel_formats, CodecSupport::Supported);
    VideoBounds(video_decoder->platform_limits, caps.get());
    for (auto& profile : video_decoder->profiles)
      profile.support = Profile(caps.get(), profile.value) ? CodecSupport::Supported : CodecSupport::Unknown;
  } else {
    auto& video = std::get<VideoEncoderCapabilities>(snapshot);
    video.hardware_acceleration = CodecSupport::Unsupported;
    SetSupport(video.pixel_formats, CodecSupport::Supported);
    VideoBounds(video.platform_limits, caps.get());
    video.platform_limits.bitrate = Property(element.get(), "bitrate", 1000);
    auto output = PadCaps(element.get(), GST_PAD_SRC);
    const auto bframes = Property(element.get(), "bframes");
    for (auto& profile : video.profiles) {
      profile.support = Profile(output.get(), profile.profile) ? CodecSupport::Supported : CodecSupport::Unknown;
      if (bframes && bframes->minimum == 0) {
        profile.no_b_frames = profile.automatic_b_frames = CodecSupport::Supported;
        if (profile.profile != H264Profile::Baseline) {
          profile.maximum_b_frames = std::min<std::uint32_t>(bframes->maximum, 16);
          profile.bounded_b_frames = *profile.maximum_b_frames ? CodecSupport::Supported : CodecSupport::Unsupported;
        }
      }
    }
  }
  return CapabilityResult::Success(std::move(snapshot));
}
} // namespace huxerui::media::detail
