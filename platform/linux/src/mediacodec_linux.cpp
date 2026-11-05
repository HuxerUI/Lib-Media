#include "codec/backend.h"
#include "codec/copy.h"
#include <limits>

#ifdef HUXERUI_MEDIA_GSTREAMER
#include <gst/app/gstappsrc.h>
#include <gst/app/gstappsink.h>
#include <gst/audio/audio.h>
#include <gst/video/video.h>
#endif

namespace huxerui::media::detail {
namespace {
#ifdef HUXERUI_MEDIA_GSTREAMER
struct BufferMapping {
  GstBuffer* buffer;
  GstMapInfo info{};
  bool mapped = false;
  ~BufferMapping() { if (mapped) gst_buffer_unmap(buffer, &info); }
};
struct VideoMapping {
  GstVideoFrame frame{};
  bool mapped = false;
  ~VideoMapping() { if (mapped) gst_video_frame_unmap(&frame); }
};
class GstreamerDecoder final : public DecoderBackend {
public:
  CodecResult<void> Open(const DecoderConfig& config) override {
    config_ = config;
    GError* error = nullptr;
    if (!gst_init_check(nullptr, nullptr, &error)) {
      const std::string message = error ? error->message : "GStreamer initialization failed";
      if (error) g_error_free(error);
      return CodecResult<void>::Failure(Error(CodecErrorCode::Unsupported, message, "GStreamer"));
    }
    const auto* audio = std::get_if<AudioDecoderConfig>(&config_);
    auto* input = audio ? gst_caps_new_simple("audio/mpeg", "mpegversion", G_TYPE_INT, 4, "stream-format", G_TYPE_STRING, "raw",
        "rate", G_TYPE_INT, int(audio->output.sample_rate), "channels", G_TYPE_INT, int(audio->output.ChannelCount()), nullptr)
        : gst_caps_new_simple("video/x-h264", "stream-format", G_TYPE_STRING, "avc", "alignment", G_TYPE_STRING, "au", nullptr);
    const auto init = std::visit([](const auto& c) { return c.initialization_data; }, config_);
    auto* description = gst_buffer_new_allocate(nullptr, init.size(), nullptr);
    const auto copied = gst_buffer_fill(description, 0, init.data(), init.size());
    if (copied != init.size()) { gst_buffer_unref(description); gst_caps_unref(input); return CodecResult<void>::Failure(Error(CodecErrorCode::ResourceExhausted, "Cannot copy GStreamer configuration")); }
    gst_caps_set_simple(input, "codec_data", GST_TYPE_BUFFER, description, nullptr);
    gst_buffer_unref(description);
    auto* factories = gst_element_factory_list_get_elements(GST_ELEMENT_FACTORY_TYPE_DECODER
        | (audio ? GST_ELEMENT_FACTORY_TYPE_MEDIA_AUDIO : GST_ELEMENT_FACTORY_TYPE_MEDIA_VIDEO), GST_RANK_MARGINAL);
    auto* compatible = gst_element_factory_list_filter(factories, input, GST_PAD_SINK, FALSE);
    GstElement* decoder = nullptr;
    for (auto* item = compatible; item && !decoder; item = item->next)
      decoder = gst_element_factory_create(GST_ELEMENT_FACTORY(item->data), nullptr);
    gst_plugin_feature_list_free(compatible);
    gst_plugin_feature_list_free(factories);
    if (!decoder) { gst_caps_unref(input); return CodecResult<void>::Failure(Error(CodecErrorCode::Unsupported, "No installed GStreamer decoder accepts the configured caps")); }
    pipeline_ = gst_pipeline_new(nullptr);
    source_ = gst_element_factory_make("appsrc", nullptr);
    sink_ = gst_element_factory_make("appsink", nullptr);
    auto* parser = gst_element_factory_make(audio ? "aacparse" : "h264parse", nullptr);
    auto* convert = gst_element_factory_make(audio ? "audioconvert" : "videoconvert", nullptr);
    if (!pipeline_ || !source_ || !sink_ || !parser || !convert) {
      for (auto* element : {source_, sink_, parser, convert, decoder}) if (element) gst_object_unref(element);
      source_ = sink_ = nullptr;
      gst_caps_unref(input);
      return CodecResult<void>::Failure(Error(CodecErrorCode::Unsupported, "Required GStreamer app/parser/converter plugins are missing"));
    }
    const auto limits = std::visit([](const auto& c) { return c.limits; }, config_);
    g_object_set(source_, "format", GST_FORMAT_TIME, "block", FALSE, "max-buffers", guint64(limits.input_items),
        "max-bytes", guint64(limits.input_bytes), "is-live", FALSE, nullptr);
    gst_app_src_set_caps(GST_APP_SRC(source_), input);
    gst_caps_unref(input);
    g_object_set(sink_, "sync", FALSE, "max-buffers", guint(1), "drop", FALSE, "enable-last-sample", FALSE, "wait-on-eos", FALSE, nullptr);
    GstCaps* output;
    if (audio) {
      // Native decoding stays interleaved; plane packing is copied explicitly after mapping.
      output = gst_caps_new_simple("audio/x-raw", "format", G_TYPE_STRING,
          audio->output.sample_format == AudioSampleFormat::F32 ? GST_AUDIO_NE(F32) : GST_AUDIO_NE(S16),
          "layout", G_TYPE_STRING, "interleaved", "rate", G_TYPE_INT, int(audio->output.sample_rate),
          "channels", G_TYPE_INT, int(audio->output.ChannelCount()), nullptr);
    } else output = gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING,
        std::get<VideoDecoderConfig>(config_).output == VideoPixelFormat::NV12 ? "NV12" : "I420", nullptr);
    gst_app_sink_set_caps(GST_APP_SINK(sink_), output);
    gst_caps_unref(output);
    gst_bin_add_many(GST_BIN(pipeline_), source_, parser, decoder, convert, sink_, nullptr);
    if (!gst_element_link_many(source_, parser, decoder, convert, sink_, nullptr))
      return CodecResult<void>::Failure(Error(CodecErrorCode::Unsupported, "Cannot link the configured GStreamer decoder"));
    bus_ = gst_element_get_bus(pipeline_);
    if (gst_element_set_state(pipeline_, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
      return CodecResult<void>::Failure(Error(CodecErrorCode::PlatformFailure, "GStreamer cannot start decoding"));
    return CodecResult<void>::Success();
  }
  CodecResult<SubmitStatus> Submit(const EncodedPacket& packet) override {
    using R = CodecResult<SubmitStatus>;
    const auto bytes = packet.data.AsBytes();
    const auto limits = std::visit([](const auto& c) { return c.limits; }, config_);
    auto* source = GST_APP_SRC(source_);
    // Only the codec worker pushes; the streaming thread can only lower these levels.
    // Keep pushes nonblocking so this worker can still drain appsink.
    const auto queued_bytes = gst_app_src_get_current_level_bytes(source);
    if (gst_app_src_get_current_level_buffers(source) >= limits.input_items ||
        queued_bytes > limits.input_bytes || bytes.size() > limits.input_bytes - queued_bytes)
      return R::Success(SubmitStatus::WouldBlock);

    const auto pts = packet.pts.Rescale(GST_SECOND);
    const auto dts = packet.dts ? packet.dts->Rescale(GST_SECOND) : pts;
    const auto duration = packet.duration ? packet.duration->Rescale(GST_SECOND) : std::optional<MediaTimestamp>{};
    if (!pts || !dts || (packet.duration && !duration)) return R::Failure(Error(CodecErrorCode::InvalidData, "Timestamp exceeds GStreamer nanosecond range"));
    // Translate signed preroll to GstClockTime once; restore the original timeline on output.
    if (!time_offset_) {
      const auto first = std::min(pts->ticks, dts->ticks);
      time_offset_ = first < 0 ? std::uint64_t(-(first + 1)) + 1 : 0;
    }
    const auto encode = [&](std::int64_t value) -> std::optional<std::uint64_t> {
      if (value < 0) {
        const auto magnitude = std::uint64_t(-(value + 1)) + 1;
        if (magnitude > *time_offset_) return std::nullopt;
        return *time_offset_ - magnitude;
      }
      if (std::uint64_t(value) >= GST_CLOCK_TIME_NONE - *time_offset_) return std::nullopt;
      return *time_offset_ + std::uint64_t(value);
    };
    const auto native_pts = encode(pts->ticks), native_dts = encode(dts->ticks);
    if (!native_pts || !native_dts)
      return R::Failure(Error(CodecErrorCode::InvalidData, "Timestamp precedes the first decoding time or exceeds GstClockTime"));
    auto* buffer = gst_buffer_new_allocate(nullptr, bytes.size(), nullptr);
    if (!buffer) return R::Failure(Error(CodecErrorCode::ResourceExhausted, "Cannot allocate GStreamer input"));
    if (gst_buffer_fill(buffer, 0, bytes.data(), bytes.size()) != bytes.size()) {
      gst_buffer_unref(buffer); return R::Failure(Error(CodecErrorCode::ResourceExhausted, "Cannot copy GStreamer input"));
    }
    GST_BUFFER_PTS(buffer) = *native_pts; GST_BUFFER_DTS(buffer) = *native_dts;
    GST_BUFFER_DURATION(buffer) = duration ? duration->ticks : GST_CLOCK_TIME_NONE;
    if (!packet.key_frame) GST_BUFFER_FLAG_SET(buffer, GST_BUFFER_FLAG_DELTA_UNIT);
    const auto status = gst_app_src_push_buffer(source, buffer);
    if (status != GST_FLOW_OK) return R::Failure(Error(CodecErrorCode::PlatformFailure, "GStreamer input rejected", gst_flow_get_name(status)));
    return R::Success(SubmitStatus::Accepted);
  }
  CodecResult<std::optional<BackendOutput>> Poll() override {
    using R = CodecResult<std::optional<BackendOutput>>;
    auto* message = gst_bus_pop_filtered(bus_, GST_MESSAGE_ERROR);
    if (message) {
      GError* error = nullptr; gchar* debug = nullptr;
      gst_message_parse_error(message, &error, &debug);
      const auto failure = Error(CodecErrorCode::PlatformFailure, error ? error->message : "GStreamer decoding failed", debug ? debug : "");
      if (error) g_error_free(error);
      g_free(debug); gst_message_unref(message);
      return R::Failure(failure);
    }
    auto* sample = gst_app_sink_try_pull_sample(GST_APP_SINK(sink_), 0);
    if (!sample) {
      if (finished_ && !eos_ && gst_app_sink_is_eos(GST_APP_SINK(sink_))) { eos_ = true; return R::Success(BackendOutput{EndOfStream{}}); }
      return R::Success(std::nullopt);
    }
    std::unique_ptr<GstSample, decltype(&gst_sample_unref)> owned_sample(sample, gst_sample_unref);
    auto result = Copy(owned_sample.get());
    if (!result.Succeeded()) return R::Failure(result.Error());
    return R::Success(BackendOutput{std::move(result).Value()});
  }
  CodecResult<SubmitStatus> Finish() override {
    const auto status = gst_app_src_end_of_stream(GST_APP_SRC(source_));
    if (status != GST_FLOW_OK) return CodecResult<SubmitStatus>::Failure(Error(CodecErrorCode::PlatformFailure, "GStreamer EOS rejected", gst_flow_get_name(status)));
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
    if (failed) return CodecResult<void>::Failure(Error(CodecErrorCode::PlatformFailure, "GStreamer failed to enter NULL during cleanup"));
    return CodecResult<void>::Success();
  }
private:
  CodecResult<DecodedFrame> Copy(GstSample* sample) {
    using R = CodecResult<DecodedFrame>;
    auto* buffer = gst_sample_get_buffer(sample);
    auto* caps = gst_sample_get_caps(sample);
    if (!GST_BUFFER_PTS_IS_VALID(buffer)) return R::Failure(Error(CodecErrorCode::InvalidData, "GStreamer output has no presentation time"));
    const auto raw = GST_BUFFER_PTS(buffer);
    if (!time_offset_) return R::Failure(Error(CodecErrorCode::InvalidData, "Output has no input timeline"));
    const auto magnitude = raw < *time_offset_ ? *time_offset_ - raw : raw - *time_offset_;
    const auto maximum = std::uint64_t(std::numeric_limits<std::int64_t>::max());
    if (magnitude > maximum + (raw < *time_offset_ ? 1 : 0))
      return R::Failure(Error(CodecErrorCode::InvalidData, "Native output timestamp overflow"));
    const auto ticks = raw >= *time_offset_ ? std::int64_t(magnitude)
        : magnitude == maximum + 1 ? std::numeric_limits<std::int64_t>::min() : -std::int64_t(magnitude);
    const MediaTimestamp pts{ticks, GST_SECOND};
    if (const auto* audio = std::get_if<AudioDecoderConfig>(&config_)) {
      GstAudioInfo info;
      if (!gst_audio_info_from_caps(&info, caps) || GST_AUDIO_INFO_RATE(&info) != audio->output.sample_rate
          || GST_AUDIO_INFO_CHANNELS(&info) != audio->output.ChannelCount())
        return R::Failure(Error(CodecErrorCode::InvalidData, "GStreamer changed the PCM format"));
      BufferMapping map{buffer};
      if (!gst_buffer_map(buffer, &map.info, GST_MAP_READ)) return R::Failure(Error(CodecErrorCode::PlatformFailure, "Cannot map GStreamer PCM"));
      map.mapped = true;
      const auto expected = gst_audio_format_from_string(audio->output.sample_format == AudioSampleFormat::F32 ? GST_AUDIO_NE(F32) : GST_AUDIO_NE(S16));
      if (GST_AUDIO_INFO_FORMAT(&info) != expected || GST_AUDIO_INFO_LAYOUT(&info) != GST_AUDIO_LAYOUT_INTERLEAVED)
        return R::Failure(Error(CodecErrorCode::Unsupported, "Unsupported GStreamer PCM packing/representation"));
      auto bytes = Own(Bytes(reinterpret_cast<std::byte*>(map.info.data), reinterpret_cast<std::byte*>(map.info.data + map.info.size)));
      auto format = audio->output; format.packing = AudioPacking::Interleaved;
      const auto stride = format.BytesPerSample() * format.ChannelCount();
      if (bytes.AsBytes().size() % stride || bytes.AsBytes().size() / stride > 1024)
        return R::Failure(Error(CodecErrorCode::InvalidData, "Unexpected GStreamer AAC sample count"));
      auto copied = CopyAudio({format, pts, std::uint32_t(bytes.AsBytes().size() / stride), {bytes}}, audio->output);
      if (!copied.Succeeded()) return R::Failure(copied.Error());
      return R::Success(DecodedFrame{std::move(copied).Value()});
    }
    const auto& config = std::get<VideoDecoderConfig>(config_);
    GstVideoInfo info;
    if (!gst_video_info_from_caps(&info, caps) || info.width <= 0 || info.height <= 0 || info.width > config.width || info.height > config.height)
      return R::Failure(Error(CodecErrorCode::InvalidData, "GStreamer video exceeds configured geometry"));
    const auto expected = config.output == VideoPixelFormat::NV12 ? GST_VIDEO_FORMAT_NV12 : GST_VIDEO_FORMAT_I420;
    if (GST_VIDEO_INFO_FORMAT(&info) != expected) return R::Failure(Error(CodecErrorCode::Unsupported, "Unsupported GStreamer pixel layout"));
    VideoMapping mapping;
    if (!gst_video_frame_map(&mapping.frame, &info, buffer, GST_MAP_READ)) return R::Failure(Error(CodecErrorCode::PlatformFailure, "Cannot map GStreamer YUV"));
    mapping.mapped = true;
    auto& frame = mapping.frame;
    VideoFormat format;
    format.width = info.width; format.height = info.height; format.visible = {0, 0, format.width, format.height};
    format.pixel_format = config.output;
    format.aspect_width = info.par_n; format.aspect_height = info.par_d;
    format.range = info.colorimetry.range == GST_VIDEO_COLOR_RANGE_0_255 ? ColorRange::Full : info.colorimetry.range == GST_VIDEO_COLOR_RANGE_16_235 ? ColorRange::Limited : ColorRange::Unknown;
    if (info.colorimetry.primaries == GST_VIDEO_COLOR_PRIMARIES_BT709) format.primaries = ColorPrimaries::BT709;
    if (info.colorimetry.transfer == GST_VIDEO_TRANSFER_BT709) format.transfer = ColorTransfer::BT709;
    if (info.colorimetry.matrix == GST_VIDEO_COLOR_MATRIX_BT709) format.matrix = ColorMatrix::BT709;
    else if (info.colorimetry.matrix == GST_VIDEO_COLOR_MATRIX_BT601) format.matrix = ColorMatrix::BT601;
    if (auto* crop = gst_buffer_get_video_crop_meta(buffer)) format.visible = {crop->x, crop->y, crop->width, crop->height};
    std::vector<VideoPlane> planes;
    bool valid = true;
    for (unsigned i = 0; i < (config.output == VideoPixelFormat::I420 ? 3u : 2u); ++i) {
      const auto stride = GST_VIDEO_FRAME_PLANE_STRIDE(&frame, i);
      if (stride <= 0) { valid = false; break; }
      const auto height = i ? (format.height + 1) / 2 : format.height;
      const auto width = i ? ((format.width + 1) / 2) * (config.output == VideoPixelFormat::NV12 ? 2 : 1) : format.width;
      const auto size = std::size_t(stride) * (height - 1) + width;
      auto* base = static_cast<const std::byte*>(GST_VIDEO_FRAME_PLANE_DATA(&frame, i));
      planes.push_back({Own(Bytes(base, base + size)), std::size_t(stride), i && config.output == VideoPixelFormat::NV12 ? 2u : 1u});
    }
    if (!valid) return R::Failure(Error(CodecErrorCode::Unsupported, "Negative GStreamer video stride is unsupported"));
    const auto duration = GST_BUFFER_DURATION_IS_VALID(buffer) ? std::optional(MediaTimestamp{std::int64_t(GST_BUFFER_DURATION(buffer)), GST_SECOND}) : std::nullopt;
    auto copied = CopyVideo(format, pts, duration, std::move(planes), config.output);
    if (!copied.Succeeded()) return R::Failure(copied.Error());
    return R::Success(DecodedFrame{std::move(copied).Value()});
  }
  DecoderConfig config_;
  GstElement* pipeline_ = nullptr;
  GstElement* source_ = nullptr;
  GstElement* sink_ = nullptr;
  GstBus* bus_ = nullptr;
  std::optional<std::uint64_t> time_offset_;
  bool finished_ = false, eos_ = false;
};
#else
class GstreamerDecoder final : public DecoderBackend {
public:
  CodecResult<void> Open(const DecoderConfig&) override {
    return CodecResult<void>::Failure(Error(CodecErrorCode::Unsupported, "Build with HUXERUI_MEDIA_GSTREAMER=ON to enable Linux codecs"));
  }
  CodecResult<SubmitStatus> Submit(const EncodedPacket&) override { return CodecResult<SubmitStatus>::Failure(Error(CodecErrorCode::Unsupported, "GStreamer codecs are disabled")); }
  CodecResult<std::optional<BackendOutput>> Poll() override { return CodecResult<std::optional<BackendOutput>>::Success(std::nullopt); }
  CodecResult<SubmitStatus> Finish() override { return CodecResult<SubmitStatus>::Failure(Error(CodecErrorCode::Unsupported, "GStreamer codecs are disabled")); }
  CodecResult<void> Close() override { return CodecResult<void>::Success(); }
};
#endif
} // namespace
std::unique_ptr<DecoderBackend> MakeDecoderBackend() { return std::make_unique<GstreamerDecoder>(); }
} // namespace huxerui::media::detail
