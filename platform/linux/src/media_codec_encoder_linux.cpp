#include "codec/encoder/backend.h"

#include <gst/app/gstappsrc.h>
#include <gst/app/gstappsink.h>
#include <gst/audio/audio.h>
#include <gst/video/video.h>
#include <gst/video/video-event.h>

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

namespace {
class GstreamerVideoEncoder final : public VideoEncoderBackend {
public:
  CodecResult<void> Open(const VideoEncoderConfig& config) override {
    config_ = config; config_.wakeup = {}; config_.cleanup_completed = {};
    const bool hevc = config.codec == CodecId::Hevc;
    const bool ten_bit = config.input.pixel_format == VideoPixelFormat::P010;
    if (config.bitrate % 1000)
      return CodecResult<void>::Failure(Error(CodecErrorCode::Unsupported, "GStreamer video encoder bitrate must be an integral number of kbit/s"));
    if (config.frame_rate_numerator > INT32_MAX || config.frame_rate_denominator > INT32_MAX)
      return CodecResult<void>::Failure(Error(CodecErrorCode::Unsupported, "GStreamer frame-rate components exceed signed integer range"));
    GError* error = nullptr;
    if (!gst_init_check(nullptr, nullptr, &error)) {
      auto failure = Error(CodecErrorCode::Unsupported, error ? error->message : "Cannot initialize GStreamer");
      if (error) g_error_free(error);
      return CodecResult<void>::Failure(std::move(failure));
    }
    pipeline_ = gst_pipeline_new(nullptr);
    source_ = gst_element_factory_make("appsrc", nullptr);
    sink_ = gst_element_factory_make("appsink", nullptr);
    encoder_ = gst_element_factory_make(hevc ? "x265enc" : "x264enc", nullptr);
    auto* parser = gst_element_factory_make(hevc ? "h265parse" : "h264parse", nullptr);
    auto* convert = hevc ? gst_element_factory_make("videoconvert", nullptr) : nullptr;
    if (!pipeline_ || !source_ || !sink_ || !encoder_ || !parser || (hevc && !convert)) {
      for (auto* element : {source_, sink_, encoder_, parser, convert}) if (element) gst_object_unref(element);
      source_ = sink_ = encoder_ = nullptr;
      return CodecResult<void>::Failure(Error(CodecErrorCode::Unsupported, "Required GStreamer video encoder, parser, or converter is missing"));
    }
    const auto window = VideoEncoderWindow(config);
    row_bytes_ = config.input.width * (ten_bit ? 2 : 1);
    frame_bytes_ = std::size_t(row_bytes_) * config.input.height * 3 / 2;
    g_object_set(source_, "format", GST_FORMAT_TIME, "block", FALSE, "is-live", FALSE,
        "max-buffers", guint64(window), "max-bytes", guint64(window * frame_bytes_), nullptr);
    g_object_set(sink_, "sync", FALSE, "max-buffers", guint(1), "drop", FALSE,
        "enable-last-sample", FALSE, "wait-on-eos", FALSE, nullptr);
    const auto bframes = VideoEncoderBFrames(config);
    guint actual_bframes = bframes, bitrate = 0;
    if (hevc) {
      gst_util_set_object_arg(G_OBJECT(encoder_), "speed-preset", "ultrafast");
      gst_util_set_object_arg(G_OBJECT(encoder_), "tune", "zerolatency");
      const auto options = "open-gop=0:scenecut=0:frame-threads=1:bframes=" + std::to_string(bframes);
      g_object_set(encoder_, "bitrate", guint(config.bitrate / 1000), "key-int-max", gint(config.key_frame_interval),
          "option-string", options.c_str(), nullptr);
    } else {
      g_object_set(encoder_, "speed-preset", 3, "bitrate", guint(config.bitrate / 1000), "bframes", guint(bframes),
          "b-adapt", TRUE, "rc-lookahead", 0, "sync-lookahead", 0, "threads", guint(1),
          "key-int-max", guint(config.key_frame_interval), "byte-stream", FALSE,
          "cabac", config.profile != H264Profile::Baseline, "dct8x8", config.profile == H264Profile::High, nullptr);
      g_object_get(encoder_, "bframes", &actual_bframes, nullptr);
    }
    g_object_get(encoder_, "bitrate", &bitrate, nullptr);
    auto* input = gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, ten_bit ? "P010_10LE" : "NV12",
        "width", G_TYPE_INT, gint(config.input.width), "height", G_TYPE_INT, gint(config.input.height),
        "framerate", GST_TYPE_FRACTION, gint(config.frame_rate_numerator), gint(config.frame_rate_denominator),
        "pixel-aspect-ratio", GST_TYPE_FRACTION, 1, 1, "interlace-mode", G_TYPE_STRING, "progressive", nullptr);
    GstVideoColorimetry color{};
    color.range = config.input.range == ColorRange::Full ? GST_VIDEO_COLOR_RANGE_0_255 :
        config.input.range == ColorRange::Limited ? GST_VIDEO_COLOR_RANGE_16_235 : GST_VIDEO_COLOR_RANGE_UNKNOWN;
    color.primaries = config.input.primaries == ColorPrimaries::BT709 ? GST_VIDEO_COLOR_PRIMARIES_BT709 :
        config.input.primaries == ColorPrimaries::BT601_525 ? GST_VIDEO_COLOR_PRIMARIES_SMPTE170M :
        config.input.primaries == ColorPrimaries::BT601_625 ? GST_VIDEO_COLOR_PRIMARIES_BT470BG : GST_VIDEO_COLOR_PRIMARIES_UNKNOWN;
    color.transfer = config.input.transfer == ColorTransfer::BT709 ? GST_VIDEO_TRANSFER_BT709 :
        config.input.transfer == ColorTransfer::SRGB ? GST_VIDEO_TRANSFER_SRGB :
        config.input.transfer == ColorTransfer::Linear ? GST_VIDEO_TRANSFER_GAMMA10 : GST_VIDEO_TRANSFER_UNKNOWN;
    color.matrix = config.input.matrix == ColorMatrix::BT709 ? GST_VIDEO_COLOR_MATRIX_BT709 :
        config.input.matrix == ColorMatrix::BT601 ? GST_VIDEO_COLOR_MATRIX_BT601 : GST_VIDEO_COLOR_MATRIX_UNKNOWN;
    if (color.range || color.primaries || color.transfer || color.matrix) {
      auto* text = gst_video_colorimetry_to_string(&color);
      if (text) { gst_caps_set_simple(input, "colorimetry", G_TYPE_STRING, text, nullptr); g_free(text); }
    }
    const char* profile = hevc ? (ten_bit ? "main-10" : "main") : config.profile == H264Profile::Baseline ? "baseline" : config.profile == H264Profile::Main ? "main" : "high";
    auto* output = gst_caps_new_simple(hevc ? "video/x-h265" : "video/x-h264", "stream-format", G_TYPE_STRING, hevc ? "hvc1" : "avc",
        "alignment", G_TYPE_STRING, "au", "profile", G_TYPE_STRING, profile, nullptr);
    gst_app_src_set_caps(GST_APP_SRC(source_), input); gst_app_sink_set_caps(GST_APP_SINK(sink_), output);
    gst_caps_unref(input); gst_caps_unref(output);
    gst_bin_add_many(GST_BIN(pipeline_), source_, encoder_, parser, sink_, nullptr);
    bool linked;
    if (convert) {
      gst_bin_add(GST_BIN(pipeline_), convert);
      auto* raw = gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, ten_bit ? "I420_10LE" : "I420", nullptr);
      linked = gst_element_link(source_, convert) && gst_element_link_filtered(convert, encoder_, raw) &&
          gst_element_link_many(encoder_, parser, sink_, nullptr);
      gst_caps_unref(raw);
    } else {
      linked = gst_element_link_many(source_, encoder_, parser, sink_, nullptr);
    }
    if (actual_bframes != bframes || bitrate != config.bitrate / 1000 || !linked)
      return CodecResult<void>::Failure(Error(CodecErrorCode::Unsupported, "GStreamer rejected video encoder controls"));
    bus_ = gst_element_get_bus(pipeline_);
    if (gst_element_set_state(pipeline_, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
      return CodecResult<void>::Failure(Error(CodecErrorCode::PlatformFailure, "Cannot start GStreamer video encoding"));
    return CodecResult<void>::Success();
  }
  CodecResult<SubmitStatus> Submit(const VideoFrame& frame) override {
    using R = CodecResult<SubmitStatus>;
    auto* source = GST_APP_SRC(source_);
    if (gst_app_src_get_current_level_buffers(source) >= VideoEncoderWindow(config_))
      return R::Success(SubmitStatus::WouldBlock);
    const auto pts = frame.pts.Rescale(GST_SECOND);
    const auto duration = frame.duration ? frame.duration->Rescale(GST_SECOND) :
        MediaTimestamp{config_.frame_rate_denominator, config_.frame_rate_numerator}.Rescale(GST_SECOND);
    if (!pts || !duration) return R::Failure(Error(CodecErrorCode::InvalidData, "Video timing exceeds GStreamer range"));
    auto* buffer = gst_buffer_new_allocate(nullptr, frame_bytes_, nullptr);
    if (!buffer) return R::Failure(Error(CodecErrorCode::ResourceExhausted, "Cannot allocate GStreamer video input"));
    GstMapInfo map{};
    if (!gst_buffer_map(buffer, &map, GST_MAP_WRITE)) {
      gst_buffer_unref(buffer);
      return R::Failure(Error(CodecErrorCode::PlatformFailure, "Cannot map GStreamer video input"));
    }
    const bool ten_bit = config_.input.pixel_format == VideoPixelFormat::P010;
    CopyVideoEncoderPixels(frame, ten_bit ? VideoPixelFormat::P010 : VideoPixelFormat::NV12, {reinterpret_cast<std::byte*>(map.data), map.size});
    gst_buffer_unmap(buffer, &map);
    gsize offsets[GST_VIDEO_MAX_PLANES]{0, std::size_t(row_bytes_) * config_.input.height};
    gint strides[GST_VIDEO_MAX_PLANES]{gint(row_bytes_), gint(row_bytes_)};
    if (!gst_buffer_add_video_meta_full(buffer, GST_VIDEO_FRAME_FLAG_NONE, ten_bit ? GST_VIDEO_FORMAT_P010_10LE : GST_VIDEO_FORMAT_NV12,
        config_.input.width, config_.input.height, 2, offsets, strides)) {
      gst_buffer_unref(buffer);
      return R::Failure(Error(CodecErrorCode::ResourceExhausted, "Cannot describe packed GStreamer video strides"));
    }
    GST_BUFFER_PTS(buffer) = pts->ticks; GST_BUFFER_DURATION(buffer) = duration->ticks;
    const auto status = gst_app_src_push_buffer(source, buffer);
    if (status != GST_FLOW_OK) return R::Failure(Error(CodecErrorCode::PlatformFailure, "GStreamer rejected video input", gst_flow_get_name(status)));
    return R::Success(SubmitStatus::Accepted);
  }
  CodecResult<std::optional<VideoEncoderBackendOutput>> Poll() override {
    using R = CodecResult<std::optional<VideoEncoderBackendOutput>>;
    if (pending_) { auto value = std::move(*pending_); pending_.reset(); return R::Success(std::move(value)); }
    if (auto* message = gst_bus_pop_filtered(bus_, GST_MESSAGE_ERROR)) {
      GError* error = nullptr; gchar* debug = nullptr;
      gst_message_parse_error(message, &error, &debug);
      auto failure = Error(CodecErrorCode::PlatformFailure, error ? error->message : "video encoding failed", debug ? debug : "");
      if (error) g_error_free(error);
      g_free(debug); gst_message_unref(message);
      return R::Failure(std::move(failure));
    }
    auto* sample = gst_app_sink_try_pull_sample(GST_APP_SINK(sink_), 0);
    if (!sample) {
      if (finished_ && !eos_ && gst_app_sink_is_eos(GST_APP_SINK(sink_))) { eos_ = true; return R::Success(VideoEncoderEnd{}); }
      return R::Success(std::nullopt);
    }
    std::unique_ptr<GstSample, decltype(&gst_sample_unref)> owner(sample, gst_sample_unref);
    auto* buffer = gst_sample_get_buffer(sample);
    const auto* segment = gst_sample_get_segment(sample);
    if (!buffer || !segment || !GST_BUFFER_PTS_IS_VALID(buffer))
      return R::Failure(Error(CodecErrorCode::InvalidData, "GStreamer video output has no presentation time"));
    auto time = [segment](GstClockTime value) -> std::optional<MediaTimestamp> {
      guint64 stream = 0;
      const auto sign = gst_segment_to_stream_time_full(segment, GST_FORMAT_TIME, value, &stream);
      if (!sign || stream > INT64_MAX) return std::nullopt;
      return MediaTimestamp{sign < 0 ? -std::int64_t(stream) : std::int64_t(stream), GST_SECOND}.Rescale(1000000);
    };
    const auto pts = time(GST_BUFFER_PTS(buffer));
    const auto dts = GST_BUFFER_DTS_IS_VALID(buffer) ? time(GST_BUFFER_DTS(buffer)) : std::optional<MediaTimestamp>{};
    if (!pts || (GST_BUFFER_DTS_IS_VALID(buffer) && !dts))
      return R::Failure(Error(CodecErrorCode::InvalidData, "GStreamer video timestamps exceed the session range"));
    auto* caps = gst_sample_get_caps(sample);
    const auto* structure = caps ? gst_caps_get_structure(caps, 0) : nullptr;
    const auto* value = structure ? gst_structure_get_value(structure, "codec_data") : nullptr;
    if (!value || !GST_VALUE_HOLDS_BUFFER(value))
      return R::Failure(Error(CodecErrorCode::InvalidData, "GStreamer video configuration is missing"));
    auto* description = gst_value_get_buffer(value);
    const auto init_size = gst_buffer_get_size(description);
    if (!init_size || init_size > 65536)
      return R::Failure(Error(CodecErrorCode::ResourceExhausted, "video configuration exceeds 64 KiB"));
    Bytes init(init_size);
    if (gst_buffer_extract(description, 0, init.data(), init.size()) != init.size())
      return R::Failure(Error(CodecErrorCode::PlatformFailure, "Cannot read video configuration"));
    auto described = ReadVideoEncodeFormat(init, config_);
    if (!described.Succeeded()) return R::Failure(described.Error());
    if (init.front() != std::byte{1}) return R::Failure(Error(CodecErrorCode::InvalidData, "GStreamer did not return a configuration record"));
    const auto length_size = (std::to_integer<unsigned>(init[config_.codec == CodecId::Hevc ? 21 : 4]) & 3) + 1;
    if (format_ && *format_ != described.Value())
      return R::Failure(Error(CodecErrorCode::Unsupported, "GStreamer video configuration changed"));
    const auto size = gst_buffer_get_size(buffer);
    if (!size || size > config_.max_packet_bytes)
      return R::Failure(Error(CodecErrorCode::ResourceExhausted, "GStreamer video access unit exceeds the packet bound"));
    GstMapInfo map{};
    if (!gst_buffer_map(buffer, &map, GST_MAP_READ))
      return R::Failure(Error(CodecErrorCode::PlatformFailure, "Cannot map GStreamer video output"));
    auto normalized = NormalizeVideoEncoderPacket({reinterpret_cast<const std::byte*>(map.data), map.size},
        length_size, config_.max_packet_bytes, config_.codec);
    gst_buffer_unmap(buffer, &map);
    if (!normalized.Succeeded()) return R::Failure(normalized.Error());
    EncodedPacket packet{Own(std::move(normalized).Value()), *pts, dts, std::nullopt, false};
    if (!format_) {
      format_ = std::move(described).Value(); pending_ = std::move(packet);
      return R::Success(*format_);
    }
    return R::Success(std::move(packet));
  }
  CodecResult<void> RequestKeyFrame() override {
    if (!gst_element_send_event(encoder_, gst_video_event_new_upstream_force_key_unit(GST_CLOCK_TIME_NONE, TRUE, 0)))
      return CodecResult<void>::Failure(Error(CodecErrorCode::Unsupported, "GStreamer rejected the key-picture request"));
    return CodecResult<void>::Success();
  }
  CodecResult<SubmitStatus> Finish() override {
    const auto status = gst_app_src_end_of_stream(GST_APP_SRC(source_));
    if (status != GST_FLOW_OK) return CodecResult<SubmitStatus>::Failure(Error(CodecErrorCode::PlatformFailure, "GStreamer rejected video EOS", gst_flow_get_name(status)));
    finished_ = true;
    return CodecResult<SubmitStatus>::Success(SubmitStatus::Accepted);
  }
  CodecResult<void> Close() override {
    bool failed = false;
    if (pipeline_) {
      const auto status = gst_element_set_state(pipeline_, GST_STATE_NULL);
      failed = status == GST_STATE_CHANGE_FAILURE ||
          (status == GST_STATE_CHANGE_ASYNC && gst_element_get_state(pipeline_, nullptr, nullptr, GST_CLOCK_TIME_NONE) == GST_STATE_CHANGE_FAILURE);
      gst_object_unref(pipeline_); pipeline_ = nullptr; source_ = sink_ = encoder_ = nullptr;
    }
    if (bus_) { gst_object_unref(bus_); bus_ = nullptr; }
    pending_.reset();
    return failed ? CodecResult<void>::Failure(Error(CodecErrorCode::PlatformFailure, "Cannot stop GStreamer video encoder")) : CodecResult<void>::Success();
  }
private:
  VideoEncoderConfig config_;
  GstElement* pipeline_ = nullptr;
  GstElement* source_ = nullptr;
  GstElement* sink_ = nullptr;
  GstElement* encoder_ = nullptr;
  GstBus* bus_ = nullptr;
  std::optional<VideoEncodeFormat> format_;
  std::optional<EncodedPacket> pending_;
  std::uint32_t row_bytes_ = 0;
  std::size_t frame_bytes_ = 0;
  bool finished_ = false, eos_ = false;
};
}
std::unique_ptr<VideoEncoderBackend> MakeVideoEncoderBackend() { return std::make_unique<GstreamerVideoEncoder>(); }
} // namespace huxerui::media::detail
