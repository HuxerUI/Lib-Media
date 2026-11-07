#include "codec/encoder/backend.h"

#include <gst/app/gstappsrc.h>
#include <gst/app/gstappsink.h>
#include <gst/audio/audio.h>

namespace huxerui::media::detail {
namespace {
class GstreamerAudioEncoder final : public AudioEncoderBackend {
public:
  CodecResult<void> Open(const AudioEncoderConfig& config) override {
    config_ = config;
    config_.wakeup = {}; config_.cleanup_completed = {};
    GError* error = nullptr;
    if (!gst_init_check(nullptr, nullptr, &error)) {
      auto failure = Error(CodecErrorCode::Unsupported, error ? error->message : "Cannot initialize GStreamer");
      if (error) g_error_free(error);
      return CodecResult<void>::Failure(std::move(failure));
    }
    pipeline_ = gst_pipeline_new(nullptr);
    source_ = gst_element_factory_make("appsrc", nullptr);
    sink_ = gst_element_factory_make("appsink", nullptr);
    auto* convert = gst_element_factory_make("audioconvert", nullptr);
    auto* encoder = gst_element_factory_make("avenc_aac", nullptr);
    auto* parser = gst_element_factory_make("aacparse", nullptr);
    if (!pipeline_ || !source_ || !sink_ || !convert || !encoder || !parser) {
      for (auto* element : {source_, sink_, convert, encoder, parser}) if (element) gst_object_unref(element);
      source_ = sink_ = nullptr;
      return CodecResult<void>::Failure(Error(CodecErrorCode::Unsupported, "AAC encoding requires the GStreamer app, audioconvert, avenc_aac and aacparse plugins"));
    }
    g_object_set(source_, "format", GST_FORMAT_TIME, "block", FALSE, "max-buffers", guint64(8),
        "max-bytes", guint64(8 * 4096), "is-live", FALSE, nullptr);
    g_object_set(sink_, "sync", FALSE, "max-buffers", guint(1), "drop", FALSE,
        "enable-last-sample", FALSE, "wait-on-eos", FALSE, nullptr);
    g_object_set(encoder, "bitrate", gint(config.bitrate), nullptr);
    gint bitrate = 0;
    g_object_get(encoder, "bitrate", &bitrate, nullptr);
    auto* input = gst_caps_new_simple("audio/x-raw", "format", G_TYPE_STRING, GST_AUDIO_NE(S16),
        "layout", G_TYPE_STRING, "interleaved", "rate", G_TYPE_INT, gint(config.input.sample_rate),
        "channels", G_TYPE_INT, gint(config.input.ChannelCount()), nullptr);
    auto* output = gst_caps_new_simple("audio/mpeg", "mpegversion", G_TYPE_INT, 4, "stream-format", G_TYPE_STRING, "raw",
        "profile", G_TYPE_STRING, "lc", "rate", G_TYPE_INT, gint(config.input.sample_rate),
        "channels", G_TYPE_INT, gint(config.input.ChannelCount()), nullptr);
    gst_app_src_set_caps(GST_APP_SRC(source_), input);
    gst_app_sink_set_caps(GST_APP_SINK(sink_), output);
    gst_caps_unref(input); gst_caps_unref(output);
    gst_bin_add_many(GST_BIN(pipeline_), source_, convert, encoder, parser, sink_, nullptr);
    if (bitrate != gint(config.bitrate) || !gst_element_link_many(source_, convert, encoder, parser, sink_, nullptr))
      return CodecResult<void>::Failure(Error(CodecErrorCode::Unsupported, "GStreamer rejected the AAC encoder configuration"));
    bus_ = gst_element_get_bus(pipeline_);
    if (gst_element_set_state(pipeline_, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
      return CodecResult<void>::Failure(Error(CodecErrorCode::PlatformFailure, "Cannot start GStreamer AAC encoding"));
    return CodecResult<void>::Success();
  }
  CodecResult<SubmitStatus> Submit(std::span<const std::byte> pcm, std::uint32_t samples, std::int64_t first_sample) override {
    using R = CodecResult<SubmitStatus>;
    auto* source = GST_APP_SRC(source_);
    if (gst_app_src_get_current_level_buffers(source) >= 8 || gst_app_src_get_current_level_bytes(source) > 8 * 4096 - pcm.size())
      return R::Success(SubmitStatus::WouldBlock);
    const auto pts = MediaTimestamp{first_sample, config_.input.sample_rate}.Rescale(GST_SECOND);
    const auto end = MediaTimestamp{first_sample + samples, config_.input.sample_rate}.Rescale(GST_SECOND);
    if (!pts || !end) return R::Failure(Error(CodecErrorCode::InvalidData, "PCM timing exceeds GStreamer range"));
    auto* buffer = gst_buffer_new_allocate(nullptr, pcm.size(), nullptr);
    if (!buffer) return R::Failure(Error(CodecErrorCode::ResourceExhausted, "Cannot allocate GStreamer PCM input"));
    if (gst_buffer_fill(buffer, 0, pcm.data(), pcm.size()) != pcm.size()) {
      gst_buffer_unref(buffer);
      return R::Failure(Error(CodecErrorCode::ResourceExhausted, "Cannot copy GStreamer PCM input"));
    }
    GST_BUFFER_PTS(buffer) = pts->ticks;
    GST_BUFFER_DURATION(buffer) = end->ticks - pts->ticks;
    GST_BUFFER_OFFSET(buffer) = first_sample;
    GST_BUFFER_OFFSET_END(buffer) = first_sample + samples;
    const auto status = gst_app_src_push_buffer(source, buffer);
    if (status != GST_FLOW_OK) return R::Failure(Error(CodecErrorCode::PlatformFailure, "GStreamer rejected PCM input", gst_flow_get_name(status)));
    return R::Success(SubmitStatus::Accepted);
  }
  CodecResult<std::optional<AudioEncoderBackendOutput>> Poll() override {
    using R = CodecResult<std::optional<AudioEncoderBackendOutput>>;
    if (pending_) { auto packet = std::move(pending_); pending_.reset(); return R::Success(std::move(*packet)); }
    auto* message = gst_bus_pop_filtered(bus_, GST_MESSAGE_ERROR);
    if (message) {
      GError* error = nullptr; gchar* debug = nullptr;
      gst_message_parse_error(message, &error, &debug);
      auto failure = Error(CodecErrorCode::PlatformFailure, error ? error->message : "AAC encoding failed", debug ? debug : "");
      if (error) g_error_free(error);
      g_free(debug); gst_message_unref(message);
      return R::Failure(std::move(failure));
    }
    auto* sample = gst_app_sink_try_pull_sample(GST_APP_SINK(sink_), 0);
    if (!sample) {
      if (finished_ && !eos_ && gst_app_sink_is_eos(GST_APP_SINK(sink_))) {
        eos_ = true;
        return R::Success(AudioEncoderEnd{priming_, padding_});
      }
      return R::Success(std::nullopt);
    }
    std::unique_ptr<GstSample, decltype(&gst_sample_unref)> owner(sample, gst_sample_unref);
    auto* buffer = gst_sample_get_buffer(sample);
    if (!buffer || !GST_BUFFER_PTS_IS_VALID(buffer) || GST_BUFFER_PTS(buffer) > INT64_MAX)
      return R::Failure(Error(CodecErrorCode::InvalidData, "GStreamer AAC output has no valid PTS"));
    const auto size = gst_buffer_get_size(buffer);
    if (!size || size > kAudioEncoderPacketBytes)
      return R::Failure(Error(CodecErrorCode::ResourceExhausted, "GStreamer AAC packet exceeds 64 KiB"));
    const auto* meta = gst_buffer_get_audio_clipping_meta(buffer);
    std::optional<std::uint64_t> start, end;
    if (meta) {
      auto samples = [this, meta](guint64 value) -> std::optional<std::uint64_t> {
        if (meta->format == GST_FORMAT_DEFAULT) return value;
        if (meta->format != GST_FORMAT_TIME || value > INT64_MAX) return std::nullopt;
        const auto count = MediaTimestamp{std::int64_t(value), GST_SECOND}.Rescale(config_.input.sample_rate);
        if (!count || count->ticks < 0) return std::nullopt;
        return std::uint64_t(count->ticks);
      };
      start = samples(meta->start); end = samples(meta->end);
      if (!start || !end || *start > 1024 || *end > 1024)
        return R::Failure(Error(CodecErrorCode::Unsupported, "Unsupported GStreamer AAC clipping metadata"));
    }
    if (!packet_count_ && start) priming_ = start;
    padding_ = end;
    auto pts = MediaTimestamp{std::int64_t(GST_BUFFER_PTS(buffer)), GST_SECOND}.Rescale(config_.input.sample_rate);
    if (!pts) return R::Failure(Error(CodecErrorCode::InvalidData, "AAC timestamp exceeds sample range"));
    if (start) pts->ticks -= std::int64_t(*start);
    EncodedPacket packet;
    Bytes bytes(size);
    if (gst_buffer_extract(buffer, 0, bytes.data(), size) != size)
      return R::Failure(Error(CodecErrorCode::PlatformFailure, "Cannot copy GStreamer AAC packet"));
    packet.data = Own(std::move(bytes)); packet.pts = *pts; packet.key_frame = true;
    // Audio DTS is optional. Keep it absent when the GStreamer buffer does not supply it.
    if (GST_BUFFER_DTS_IS_VALID(buffer) && GST_BUFFER_DTS(buffer) <= INT64_MAX)
      packet.dts = MediaTimestamp{std::int64_t(GST_BUFFER_DTS(buffer)), GST_SECOND};
    ++packet_count_;
    auto* caps = gst_sample_get_caps(sample);
    const auto* structure = caps ? gst_caps_get_structure(caps, 0) : nullptr;
    const auto* value = structure ? gst_structure_get_value(structure, "codec_data") : nullptr;
    if (!value || !GST_VALUE_HOLDS_BUFFER(value))
      return R::Failure(Error(CodecErrorCode::InvalidData, "GStreamer AAC initialization data is missing"));
    auto* description = gst_value_get_buffer(value);
    const auto init_size = gst_buffer_get_size(description);
    if (init_size != 2 && init_size != 5)
      return R::Failure(Error(CodecErrorCode::Unsupported, "Unsupported GStreamer AAC initialization data"));
    std::byte init[5]{};
    if (gst_buffer_extract(description, 0, init, init_size) != init_size)
      return R::Failure(Error(CodecErrorCode::PlatformFailure, "Cannot read GStreamer AAC initialization data"));
    auto described = ReadAudioEncodeFormat(std::span(init).first(init_size), config_);
    if (!described.Succeeded()) return R::Failure(described.Error());
    auto format = std::move(described).Value();
    if (format_ && *format_ != format)
      return R::Failure(Error(CodecErrorCode::InvalidData, "GStreamer AAC format changed during encoding"));
    if (!format_) {
      format_ = format;
      pending_ = std::move(packet);
      return R::Success(std::move(format));
    }
    return R::Success(std::move(packet));
  }
  CodecResult<SubmitStatus> Finish() override {
    const auto status = gst_app_src_end_of_stream(GST_APP_SRC(source_));
    if (status != GST_FLOW_OK)
      return CodecResult<SubmitStatus>::Failure(Error(CodecErrorCode::PlatformFailure, "GStreamer rejected encoder EOS", gst_flow_get_name(status)));
    finished_ = true;
    return CodecResult<SubmitStatus>::Success(SubmitStatus::Accepted);
  }
  CodecResult<void> Close() override {
    bool failed = false;
    if (pipeline_) {
      failed = gst_element_set_state(pipeline_, GST_STATE_NULL) == GST_STATE_CHANGE_FAILURE;
      gst_object_unref(pipeline_); pipeline_ = nullptr; source_ = sink_ = nullptr;
    }
    if (bus_) { gst_object_unref(bus_); bus_ = nullptr; }
    pending_.reset();
    return failed ? CodecResult<void>::Failure(Error(CodecErrorCode::PlatformFailure, "Cannot stop GStreamer AAC encoder")) : CodecResult<void>::Success();
  }
private:
  AudioEncoderConfig config_;
  GstElement* pipeline_ = nullptr;
  GstElement* source_ = nullptr;
  GstElement* sink_ = nullptr;
  GstBus* bus_ = nullptr;
  std::optional<EncodedPacket> pending_;
  std::optional<AudioEncodeFormat> format_;
  std::optional<std::uint64_t> priming_, padding_;
  std::uint64_t packet_count_ = 0;
  bool finished_ = false, eos_ = false;
};
} // namespace
std::unique_ptr<AudioEncoderBackend> MakeAudioEncoderBackend() { return std::make_unique<GstreamerAudioEncoder>(); }
} // namespace huxerui::media::detail
