#include "codec/decoder/backend.h"
#include "codec/decoder/copy.h"
#include "codec/hevc.h"

#include <algorithm>
#include <array>
#include <limits>

#include <gst/app/gstappsrc.h>
#include <gst/app/gstappsink.h>
#include <gst/audio/audio.h>
#include <gst/video/video.h>

namespace huxerui::media::detail {
namespace {
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
bool SetDescription(GstCaps* caps, std::span<const std::byte> bytes, bool stream_header,
                    std::span<const std::byte> comments = {}) {
  GValue headers = G_VALUE_INIT;
  if (stream_header) g_value_init(&headers, GST_TYPE_ARRAY);
  const auto add = [&](std::span<const std::byte> data) {
    auto* buffer = gst_buffer_new_allocate(nullptr, data.size(), nullptr);
    if (!buffer) return false;
    const auto copied = gst_buffer_fill(buffer, 0, data.data(), data.size());
    if (copied == data.size()) {
      if (stream_header) {
        GValue value = G_VALUE_INIT;
        g_value_init(&value, GST_TYPE_BUFFER);
        gst_value_set_buffer(&value, buffer);
        gst_value_array_append_value(&headers, &value);
        g_value_unset(&value);
      } else gst_caps_set_simple(caps, "codec_data", GST_TYPE_BUFFER, buffer, nullptr);
    }
    gst_buffer_unref(buffer);
    return copied == data.size();
  };
  const auto success = add(bytes) && (comments.empty() || add(comments));
  if (stream_header) {
    if (success) gst_structure_set_value(gst_caps_get_structure(caps, 0), "streamheader", &headers);
    g_value_unset(&headers);
  }
  return success;
}
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
    GstCaps* input;
    const char* parser_name = nullptr;
    bool headers_ok;
    std::optional<HevcSequence> sequence;
    if (audio) {
      audio_sample_limit_ = AudioSampleLimit(*audio).Value();
      const auto rate = int(audio->output.sample_rate), channels = int(audio->output.ChannelCount());
      const auto header = AudioDecoderHeader(*audio);
      switch (audio->codec) {
        case CodecId::Aac:
          parser_name = "aacparse";
          input = gst_caps_new_simple("audio/mpeg", "mpegversion", G_TYPE_INT, 4, "stream-format", G_TYPE_STRING, "raw",
              "rate", G_TYPE_INT, rate, "channels", G_TYPE_INT, channels, nullptr);
          headers_ok = SetDescription(input, header, false);
          break;
        case CodecId::Opus: {
          input = gst_caps_new_simple("audio/x-opus", "channel-mapping-family", G_TYPE_INT, 0,
              "rate", G_TYPE_INT, rate, "channels", G_TYPE_INT, channels, nullptr);
          const std::array<std::uint8_t, 16> tags{'O', 'p', 'u', 's', 'T', 'a', 'g', 's'};
          headers_ok = SetDescription(input, header, true, std::as_bytes(std::span(tags)));
          break;
        }
        case CodecId::Flac:
          input = gst_caps_new_simple("audio/x-flac", "framed", G_TYPE_BOOLEAN, TRUE,
              "rate", G_TYPE_INT, rate, "channels", G_TYPE_INT, channels, nullptr);
          headers_ok = SetDescription(input, header, true);
          break;
        case CodecId::Mp3:
          input = gst_caps_new_simple("audio/mpeg", "mpegversion", G_TYPE_INT, 1, "layer", G_TYPE_INT, 3,
              "parsed", G_TYPE_BOOLEAN, TRUE, "rate", G_TYPE_INT, rate, "channels", G_TYPE_INT, channels, nullptr);
          // Raw Layer III packets leave the decoder's synthesis delay in the output PCM.
          mp3_.emplace(audio->output, audio_sample_limit_, Mp3SynthesisDelay);
          headers_ok = true;
          break;
        default:
          return CodecResult<void>::Failure(Error(CodecErrorCode::Unsupported, "Unsupported GStreamer audio codec"));
      }
    } else {
      const auto& video = std::get<VideoDecoderConfig>(config_);
      const bool hevc = video.codec == CodecId::Hevc;
      if (hevc) {
        try {
          const auto description = ParseHevcConfiguration(video.initialization_data, true, true);
          const auto sps = std::ranges::find_if(description.parameters, [](const auto& nal) { return HevcNalType(nal) == 33; });
          sequence = ReadHevcSequence(*sps);
        } catch (const std::invalid_argument& error) {
          return CodecResult<void>::Failure(Error(CodecErrorCode::InvalidArgument, error.what()));
        }
        if (sequence->visible_width > video.width || sequence->visible_height > video.height)
          return CodecResult<void>::Failure(Error(CodecErrorCode::Unsupported, "HEVC picture exceeds the GStreamer decoder bounds"));
      }
      if (video.codec == CodecId::Vp8 || video.codec == CodecId::Vp9) {
        input = gst_caps_new_empty_simple(video.codec == CodecId::Vp8 ? "video/x-vp8" : "video/x-vp9");
        headers_ok = true;
      } else {
        parser_name = hevc ? "h265parse" : "h264parse";
        input = gst_caps_new_simple(hevc ? "video/x-h265" : "video/x-h264", "stream-format", G_TYPE_STRING,
            hevc ? "hvc1" : "avc", "alignment", G_TYPE_STRING, "au", nullptr);
        headers_ok = SetDescription(input, video.initialization_data, false);
      }
    }
    if (!headers_ok) {
      gst_caps_unref(input);
      return CodecResult<void>::Failure(Error(CodecErrorCode::ResourceExhausted, "Cannot copy GStreamer configuration"));
    }
    auto* factories = gst_element_factory_list_get_elements(GST_ELEMENT_FACTORY_TYPE_DECODER
        | (audio ? GST_ELEMENT_FACTORY_TYPE_MEDIA_AUDIO : GST_ELEMENT_FACTORY_TYPE_MEDIA_VIDEO), GST_RANK_MARGINAL);
    auto* decoder_caps = sequence ? gst_caps_copy(input) : input;
    if (sequence) {
      // Filter on the format the parser will negotiate, not just the compressed media type.
      gst_caps_set_simple(decoder_caps, "width", G_TYPE_INT, int(sequence->visible_width), "height", G_TYPE_INT,
          int(sequence->visible_height), "profile", G_TYPE_STRING, sequence->luma_bits == 10 ? "main-10" : "main", nullptr);
    }
    auto* compatible = gst_element_factory_list_filter(factories, decoder_caps, GST_PAD_SINK, FALSE);
    if (sequence) gst_caps_unref(decoder_caps);
    GstElement* decoder = nullptr;
    for (auto* item = compatible; item && !decoder; item = item->next)
      decoder = gst_element_factory_create(GST_ELEMENT_FACTORY(item->data), nullptr);
    gst_plugin_feature_list_free(compatible);
    gst_plugin_feature_list_free(factories);
    if (!decoder) { gst_caps_unref(input); return CodecResult<void>::Failure(Error(CodecErrorCode::Unsupported, "No installed GStreamer decoder accepts the configured caps")); }
    pipeline_ = gst_pipeline_new(nullptr);
    source_ = gst_element_factory_make("appsrc", nullptr);
    sink_ = gst_element_factory_make("appsink", nullptr);
    auto* parser = parser_name ? gst_element_factory_make(parser_name, nullptr) : nullptr;
    auto* convert = gst_element_factory_make(audio ? "audioconvert" : "videoconvert", nullptr);
    if (!pipeline_ || !source_ || !sink_ || (parser_name && !parser) || !convert) {
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
      // GStreamer decoding stays interleaved; plane packing is copied explicitly after mapping.
      output = gst_caps_new_simple("audio/x-raw", "format", G_TYPE_STRING,
          audio->output.sample_format == AudioSampleFormat::F32 ? GST_AUDIO_NE(F32) : GST_AUDIO_NE(S16),
          "layout", G_TYPE_STRING, "interleaved", "rate", G_TYPE_INT, int(audio->output.sample_rate),
          "channels", G_TYPE_INT, int(audio->output.ChannelCount()), nullptr);
    } else {
      const auto layout = std::get<VideoDecoderConfig>(config_).output;
      output = gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING,
          layout == VideoPixelFormat::P010 ? "P010_10LE" : layout == VideoPixelFormat::NV12 ? "NV12" : "I420", nullptr);
    }
    gst_app_sink_set_caps(GST_APP_SINK(sink_), output);
    gst_caps_unref(output);
    gst_bin_add_many(GST_BIN(pipeline_), source_, decoder, convert, sink_, nullptr);
    if (parser) gst_bin_add(GST_BIN(pipeline_), parser);
    const auto linked = parser ? gst_element_link_many(source_, parser, decoder, convert, sink_, nullptr)
                               : gst_element_link_many(source_, decoder, convert, sink_, nullptr);
    if (!linked)
      return CodecResult<void>::Failure(Error(CodecErrorCode::Unsupported, "Cannot link the configured GStreamer decoder"));
    bus_ = gst_element_get_bus(pipeline_);
    if (gst_element_set_state(pipeline_, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
      return CodecResult<void>::Failure(Error(CodecErrorCode::PlatformFailure, "GStreamer cannot start decoding"));
    return CodecResult<void>::Success();
  }
  CodecResult<SubmitStatus> Submit(const EncodedPacket& packet) override {
    using R = CodecResult<SubmitStatus>;
    const auto bytes = packet.data.AsBytes();
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
    const auto platform_pts = encode(pts->ticks), platform_dts = encode(dts->ticks);
    if (!platform_pts || !platform_dts)
      return R::Failure(Error(CodecErrorCode::InvalidData, "Timestamp precedes the first decoding time or exceeds GstClockTime"));
    auto pushed = Push(bytes, *platform_pts, *platform_dts, duration ? duration->ticks : GST_CLOCK_TIME_NONE, packet.key_frame);
    if (mp3_ && pushed.Succeeded() && pushed.Value() == SubmitStatus::Accepted) {
      if (mp3_drain_frame_.empty()) mp3_drain_frame_ = Mp3DrainFrame(bytes);
      mp3_->Expect(packet.pts);
      mp3_last_pts_ = *platform_pts;
    }
    return pushed;
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
      if (finished_ && !eos_ && gst_app_sink_is_eos(GST_APP_SINK(sink_))) {
        if (mp3_ && !mp3_->Drained())
          return R::Failure(Error(CodecErrorCode::InvalidData, "GStreamer MP3 drain has missing PCM samples"));
        eos_ = true;
        return R::Success(BackendOutput{EndOfStream{}});
      }
      return R::Success(std::nullopt);
    }
    std::unique_ptr<GstSample, decltype(&gst_sample_unref)> owned_sample(sample, gst_sample_unref);
    auto result = Copy(owned_sample.get());
    if (!result.Succeeded()) return R::Failure(result.Error());
    if (mp3_) {
      const auto buffered = mp3_->Append(std::get<AudioFrame>(result.Value()), finished_);
      if (!buffered.Succeeded()) return R::Failure(buffered.Error());
      if (auto frame = mp3_->Read()) return R::Success(BackendOutput{DecodedFrame{std::move(*frame)}});
      return R::Success(std::nullopt);
    }
    return R::Success(BackendOutput{std::move(result).Value()});
  }
  CodecResult<SubmitStatus> Finish() override {
    using R = CodecResult<SubmitStatus>;
    if (!mp3_drain_frame_.empty()) {
      const auto& audio = std::get<AudioDecoderConfig>(config_);
      const auto duration = gst_util_uint64_scale_round(audio_sample_limit_, GST_SECOND, audio.output.sample_rate);
      if (duration >= GST_CLOCK_TIME_NONE - mp3_last_pts_)
        return R::Failure(Error(CodecErrorCode::InvalidData, "MP3 drain timestamp overflow"));
      const auto pts = mp3_last_pts_ + duration;
      auto pushed = Push(mp3_drain_frame_, pts, pts, duration, true);
      if (!pushed.Succeeded() || pushed.Value() == SubmitStatus::WouldBlock) return pushed;
      mp3_drain_frame_.clear();
    }
    const auto status = gst_app_src_end_of_stream(GST_APP_SRC(source_));
    if (status != GST_FLOW_OK) return R::Failure(Error(CodecErrorCode::PlatformFailure, "GStreamer EOS rejected", gst_flow_get_name(status)));
    finished_ = true;
    return R::Success(SubmitStatus::Accepted);
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
  CodecResult<SubmitStatus> Push(std::span<const std::byte> bytes, GstClockTime pts, GstClockTime dts,
      GstClockTime duration, bool key_frame) {
    using R = CodecResult<SubmitStatus>;
    const auto limits = std::visit([](const auto& c) { return c.limits; }, config_);
    auto* source = GST_APP_SRC(source_);
    // Only the codec worker pushes; the streaming thread can only lower these levels.
    // Keep pushes nonblocking so this worker can still drain appsink.
    const auto queued_bytes = gst_app_src_get_current_level_bytes(source);
    if (gst_app_src_get_current_level_buffers(source) >= limits.input_items ||
        queued_bytes > limits.input_bytes || bytes.size() > limits.input_bytes - queued_bytes)
      return R::Success(SubmitStatus::WouldBlock);
    auto* buffer = gst_buffer_new_allocate(nullptr, bytes.size(), nullptr);
    if (!buffer) return R::Failure(Error(CodecErrorCode::ResourceExhausted, "Cannot allocate GStreamer input"));
    if (gst_buffer_fill(buffer, 0, bytes.data(), bytes.size()) != bytes.size()) {
      gst_buffer_unref(buffer); return R::Failure(Error(CodecErrorCode::ResourceExhausted, "Cannot copy GStreamer input"));
    }
    GST_BUFFER_PTS(buffer) = pts; GST_BUFFER_DTS(buffer) = dts;
    GST_BUFFER_DURATION(buffer) = duration;
    if (!key_frame) GST_BUFFER_FLAG_SET(buffer, GST_BUFFER_FLAG_DELTA_UNIT);
    const auto status = gst_app_src_push_buffer(source, buffer);
    if (status != GST_FLOW_OK) return R::Failure(Error(CodecErrorCode::PlatformFailure, "GStreamer input rejected", gst_flow_get_name(status)));
    return R::Success(SubmitStatus::Accepted);
  }

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
      return R::Failure(Error(CodecErrorCode::InvalidData, "GStreamer output timestamp overflow"));
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
      if (bytes.AsBytes().size() % stride || bytes.AsBytes().size() / stride > audio_sample_limit_)
        return R::Failure(Error(CodecErrorCode::InvalidData, "Unexpected GStreamer PCM sample count"));
      auto copied = CopyAudio({format, pts, std::uint32_t(bytes.AsBytes().size() / stride), {bytes}}, audio->output);
      if (!copied.Succeeded()) return R::Failure(copied.Error());
      return R::Success(DecodedFrame{std::move(copied).Value()});
    }
    const auto& config = std::get<VideoDecoderConfig>(config_);
    GstVideoInfo info;
    if (!gst_video_info_from_caps(&info, caps) || info.width <= 0 || info.height <= 0 || info.width > config.width || info.height > config.height)
      return R::Failure(Error(CodecErrorCode::InvalidData, "GStreamer video exceeds configured geometry"));
    const auto expected = config.output == VideoPixelFormat::P010 ? GST_VIDEO_FORMAT_P010_10LE :
        config.output == VideoPixelFormat::NV12 ? GST_VIDEO_FORMAT_NV12 : GST_VIDEO_FORMAT_I420;
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
    else if (info.colorimetry.primaries == GST_VIDEO_COLOR_PRIMARIES_BT2020) format.primaries = ColorPrimaries::BT2020;
    if (info.colorimetry.transfer == GST_VIDEO_TRANSFER_BT709) format.transfer = ColorTransfer::BT709;
    else if (info.colorimetry.transfer == GST_VIDEO_TRANSFER_SMPTE2084) format.transfer = ColorTransfer::PQ;
    else if (info.colorimetry.transfer == GST_VIDEO_TRANSFER_ARIB_STD_B67) format.transfer = ColorTransfer::HLG;
    if (info.colorimetry.matrix == GST_VIDEO_COLOR_MATRIX_BT709) format.matrix = ColorMatrix::BT709;
    else if (info.colorimetry.matrix == GST_VIDEO_COLOR_MATRIX_BT601) format.matrix = ColorMatrix::BT601;
    else if (info.colorimetry.matrix == GST_VIDEO_COLOR_MATRIX_BT2020) format.matrix = ColorMatrix::BT2020;
    if (auto* crop = gst_buffer_get_video_crop_meta(buffer)) format.visible = {crop->x, crop->y, crop->width, crop->height};
    std::vector<VideoPlane> planes;
    const bool planar = config.output == VideoPixelFormat::I420;
    const auto component = config.output == VideoPixelFormat::P010 ? 2u : 1u;
    for (unsigned i = 0; i < (planar ? 3u : 2u); ++i) {
      const auto stride = GST_VIDEO_FRAME_PLANE_STRIDE(&frame, i);
      const auto height = i ? (format.height + 1) / 2 : format.height;
      const auto pixel = i && !planar ? component * 2 : component;
      const auto width = (i ? (format.width + 1) / 2 : format.width) * pixel;
      if (stride < int(width)) return R::Failure(Error(CodecErrorCode::Unsupported, "Invalid GStreamer video stride"));
      const auto size = std::size_t(stride) * (height - 1) + width;
      auto* base = static_cast<const std::byte*>(GST_VIDEO_FRAME_PLANE_DATA(&frame, i));
      planes.push_back({Own(Bytes(base, base + size)), std::size_t(stride), pixel});
    }
    const auto duration = GST_BUFFER_DURATION_IS_VALID(buffer) ? std::optional(MediaTimestamp{std::int64_t(GST_BUFFER_DURATION(buffer)), GST_SECOND}) : std::nullopt;
    auto copied = CopyVideo(format, pts, duration, std::move(planes), config.output);
    if (!copied.Succeeded()) return R::Failure(copied.Error());
    return R::Success(DecodedFrame{std::move(copied).Value()});
  }
  DecoderConfig config_;
  unsigned audio_sample_limit_ = 0;
  GstElement* pipeline_ = nullptr;
  GstElement* source_ = nullptr;
  GstElement* sink_ = nullptr;
  GstBus* bus_ = nullptr;
  std::optional<std::uint64_t> time_offset_;
  std::optional<PcmPackets> mp3_;
  Bytes mp3_drain_frame_;
  GstClockTime mp3_last_pts_ = 0;
  bool finished_ = false, eos_ = false;
};
} // namespace
std::unique_ptr<DecoderBackend> MakeDecoderBackend() { return std::make_unique<GstreamerDecoder>(); }
} // namespace huxerui::media::detail
