#ifdef MEDIA_HAS_AOM

#include "av1.h"
#include "decoder/backend.h"
#include "encoder/backend.h"

#include <aom/aom_decoder.h>
#include <aom/aomcx.h>
#include <aom/aomdx.h>

#include <cstdlib>
#include <cstring>

namespace huxerui::media::detail {
namespace {

CodecError AomError(aom_codec_ctx_t& codec, const char* message, CodecErrorCode code = CodecErrorCode::PlatformFailure) {
  const auto* detail = aom_codec_error_detail(&codec);
  return Error(code, message, detail ? detail : aom_codec_error(&codec));
}

class AomEncoder final : public VideoEncoderBackend {
public:
  ~AomEncoder() override { [[maybe_unused]] const auto closed = Close(); }

  CodecResult<void> Open(const VideoEncoderConfig& config) override {
    max_packet_bytes_ = config.max_packet_bytes;
    ten_bit_ = config.input.pixel_format == VideoPixelFormat::P010;
    aom_codec_enc_cfg_t settings{};
    if (aom_codec_enc_config_default(aom_codec_av1_cx(), &settings, AOM_USAGE_GOOD_QUALITY))
      return CodecResult<void>::Failure(Error(CodecErrorCode::Unsupported, "Cannot obtain AV1 encoder settings"));
    settings.g_w = config.input.width;
    settings.g_h = config.input.height;
    settings.g_timebase = {1, 1000000};
    settings.g_threads = 2;
    settings.g_lag_in_frames = 0;
    settings.g_bit_depth = ten_bit_ ? AOM_BITS_10 : AOM_BITS_8;
    settings.g_input_bit_depth = settings.g_bit_depth;
    settings.rc_end_usage = AOM_VBR;
    settings.rc_target_bitrate = (config.bitrate + 999) / 1000;
    settings.rc_dropframe_thresh = 0;
    settings.kf_max_dist = config.key_frame_interval;
    if (aom_codec_enc_init(&codec_, aom_codec_av1_cx(), &settings, ten_bit_ ? AOM_CODEC_USE_HIGHBITDEPTH : 0))
      return CodecResult<void>::Failure(AomError(codec_, "Cannot open the AV1 encoder", CodecErrorCode::Unsupported));
    opened_ = true;
    const auto primaries = config.input.primaries == ColorPrimaries::BT709 ? 1 :
        config.input.primaries == ColorPrimaries::BT601_525 ? 6 :
        config.input.primaries == ColorPrimaries::BT601_625 ? 5 : 2;
    const auto transfer = config.input.transfer == ColorTransfer::BT709 ? 1 :
        config.input.transfer == ColorTransfer::SRGB ? 13 : config.input.transfer == ColorTransfer::Linear ? 8 : 2;
    const auto matrix = config.input.matrix == ColorMatrix::BT709 ? 1 :
        config.input.matrix == ColorMatrix::BT601 ? 6 : 2;
    if (aom_codec_control(&codec_, AOME_SET_CPUUSED, 6) ||
        aom_codec_control(&codec_, AV1E_SET_COLOR_RANGE, config.input.range == ColorRange::Full ? 1 : 0) ||
        aom_codec_control(&codec_, AV1E_SET_COLOR_PRIMARIES, primaries) ||
        aom_codec_control(&codec_, AV1E_SET_TRANSFER_CHARACTERISTICS, transfer) ||
        aom_codec_control(&codec_, AV1E_SET_MATRIX_COEFFICIENTS, matrix))
      return CodecResult<void>::Failure(AomError(codec_, "AV1 encoder rejected its controls", CodecErrorCode::Unsupported));
    if (!aom_img_alloc(&image_, ten_bit_ ? AOM_IMG_FMT_I42016 : AOM_IMG_FMT_I420, settings.g_w, settings.g_h, 32))
      return CodecResult<void>::Failure(Error(CodecErrorCode::ResourceExhausted, "Cannot allocate AV1 input pixels"));
    image_allocated_ = true;
    auto* header = aom_codec_get_global_headers(&codec_);
    if (!header) return CodecResult<void>::Failure(AomError(codec_, "Cannot read AV1 encoder headers"));
    std::unique_ptr<aom_fixed_buf_t, decltype(&std::free)> owner(header, std::free);
    std::unique_ptr<void, decltype(&std::free)> data(header->buf, std::free);
    const auto record = MakeAv1Configuration({static_cast<const std::byte*>(data.get()), header->sz});
    auto format = ReadVideoEncodeFormat(record, config);
    if (!format.Succeeded()) return CodecResult<void>::Failure(format.Error());
    output_.emplace(std::move(format).Value());
    return CodecResult<void>::Success();
  }

  CodecResult<SubmitStatus> Submit(const VideoFrame& frame) override {
    using R = CodecResult<SubmitStatus>;
    if (output_) return R::Success(SubmitStatus::WouldBlock);
    CopyPixels(frame);
    const auto duration = frame.duration ? frame.duration->Rescale(1000000) : std::nullopt;
    const auto ticks = duration ? std::max<std::int64_t>(1, duration->ticks) : 1;
    const auto flags = std::exchange(key_, false) ? AOM_EFLAG_FORCE_KF : 0;
    if (aom_codec_encode(&codec_, &image_, frame.pts.ticks, static_cast<unsigned long>(ticks), flags))
      return R::Failure(AomError(codec_, "AV1 frame encoding failed"));
    aom_codec_iter_t cursor = nullptr;
    while (const auto* output = aom_codec_get_cx_data(&codec_, &cursor)) {
      if (output->kind != AOM_CODEC_CX_FRAME_PKT) continue;
      const auto& packet = output->data.frame;
      if (output_ || packet.sz > max_packet_bytes_)
        return R::Failure(Error(CodecErrorCode::ResourceExhausted, "AV1 output exceeds the reserved packet window"));
      const auto* bytes = static_cast<const std::byte*>(packet.buf);
      const MediaTimestamp pts{packet.pts, 1000000};
      output_.emplace(EncodedPacket{Own(Bytes(bytes, bytes + packet.sz)), pts, pts, frame.duration,
          bool(packet.flags & AOM_FRAME_IS_KEY)});
    }
    if (!output_) return R::Failure(Error(CodecErrorCode::InvalidData, "AV1 encoder delayed a zero-lag picture"));
    return R::Success(SubmitStatus::Accepted);
  }

  CodecResult<std::optional<VideoEncoderBackendOutput>> Poll() override {
    using R = CodecResult<std::optional<VideoEncoderBackendOutput>>;
    return R::Success(std::exchange(output_, std::nullopt));
  }

  CodecResult<void> RequestKeyFrame() override { key_ = true; return CodecResult<void>::Success(); }

  CodecResult<SubmitStatus> Finish() override {
    using R = CodecResult<SubmitStatus>;
    if (output_) return R::Success(SubmitStatus::WouldBlock);
    if (aom_codec_encode(&codec_, nullptr, 0, 0, 0)) return R::Failure(AomError(codec_, "AV1 drain failed"));
    aom_codec_iter_t cursor = nullptr;
    while (const auto* output = aom_codec_get_cx_data(&codec_, &cursor))
      if (output->kind == AOM_CODEC_CX_FRAME_PKT)
        return R::Failure(Error(CodecErrorCode::InvalidData, "Unexpected delayed AV1 frame"));
    output_.emplace(VideoEncoderEnd{});
    return R::Success(SubmitStatus::Accepted);
  }

  CodecResult<void> Close() override {
    output_.reset();
    if (std::exchange(image_allocated_, false)) aom_img_free(&image_);
    if (std::exchange(opened_, false)) aom_codec_destroy(&codec_);
    return CodecResult<void>::Success();
  }

private:
  void CopyPixels(const VideoFrame& frame) {
    const bool planar = frame.format.pixel_format == VideoPixelFormat::I420;
    for (unsigned component = 0; component < 3; ++component) {
      const auto& plane = frame.planes[component && !planar ? 1 : component];
      const auto columns = component ? frame.format.width / 2 : frame.format.width;
      const auto rows = component ? frame.format.height / 2 : frame.format.height;
      const auto bytes = plane.data.AsBytes();
      const auto offset = component == 2 && !planar ? (ten_bit_ ? 2u : 1u) : 0u;
      for (std::size_t y = 0; y < rows; ++y) {
        auto* output = image_.planes[component] + y * image_.stride[component];
        for (std::size_t x = 0; x < columns; ++x) {
          const auto* input = bytes.data() + y * plane.row_stride + x * plane.pixel_stride + offset;
          if (ten_bit_) {
            const std::uint16_t value = (std::to_integer<unsigned>(input[0]) |
                (std::to_integer<unsigned>(input[1]) << 8)) >> 6;
            std::memcpy(output + x * 2, &value, 2);
          } else output[x] = std::to_integer<unsigned char>(*input);
        }
      }
    }
  }

  std::size_t max_packet_bytes_ = 0;
  aom_codec_ctx_t codec_{};
  aom_image_t image_{};
  std::optional<VideoEncoderBackendOutput> output_;
  bool ten_bit_ = false, opened_ = false, image_allocated_ = false, key_ = false;
};

class AomDecoder final : public DecoderBackend {
public:
  ~AomDecoder() override { [[maybe_unused]] const auto closed = Close(); }

  CodecResult<void> Open(const DecoderConfig& config) override {
    const auto& video = std::get<VideoDecoderConfig>(config);
    width_ = video.width;
    height_ = video.height;
    layout_ = video.output;
    const bool ten = layout_ == VideoPixelFormat::P010;
    const aom_codec_dec_cfg_t settings{2, width_, height_, unsigned(!ten)};
    if (aom_codec_dec_init(&codec_, aom_codec_av1_dx(), &settings, 0))
      return CodecResult<void>::Failure(AomError(codec_, "Cannot open the AV1 decoder", CodecErrorCode::Unsupported));
    opened_ = true;
    return CodecResult<void>::Success();
  }

  CodecResult<SubmitStatus> Submit(const EncodedPacket& packet) override {
    using R = CodecResult<SubmitStatus>;
    if (output_) return R::Success(SubmitStatus::WouldBlock);
    auto bytes = packet.data.AsBytes();
    Bytes sized;
    if (const auto offset = InspectAv1Packet(bytes).unsized_obu) {
      sized = SizedAv1Packet(bytes, *offset);
      bytes = sized;
    }
    if (aom_codec_decode(&codec_, reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size(), nullptr))
      return R::Failure(AomError(codec_, "AV1 frame decoding failed", CodecErrorCode::InvalidData));
    aom_codec_iter_t cursor = nullptr;
    if (const auto* image = aom_codec_get_frame(&codec_, &cursor)) {
      const bool ten = layout_ == VideoPixelFormat::P010;
      if (image->fmt != (ten ? AOM_IMG_FMT_I42016 : AOM_IMG_FMT_I420) || image->bit_depth != (ten ? 10u : 8u) ||
          !image->d_w || !image->d_h || image->d_w > width_ || image->d_h > height_)
        return R::Failure(Error(CodecErrorCode::InvalidData, "AV1 output differs from its configured depth or bounds"));
      auto frame = CopyPixels(*image, packet);
      if (aom_codec_get_frame(&codec_, &cursor))
        return R::Failure(Error(CodecErrorCode::Unsupported, "AV1 temporal unit returned multiple pictures"));
      output_ = DecodedFrame{std::move(frame)};
    }
    return R::Success(SubmitStatus::Accepted);
  }

  CodecResult<std::optional<BackendOutput>> Poll() override {
    return CodecResult<std::optional<BackendOutput>>::Success(std::exchange(output_, std::nullopt));
  }
  CodecResult<SubmitStatus> Finish() override {
    using R = CodecResult<SubmitStatus>;
    if (output_) return R::Success(SubmitStatus::WouldBlock);
    if (aom_codec_decode(&codec_, nullptr, 0, nullptr)) return R::Failure(AomError(codec_, "AV1 decoder drain failed"));
    aom_codec_iter_t cursor = nullptr;
    if (aom_codec_get_frame(&codec_, &cursor))
      return R::Failure(Error(CodecErrorCode::InvalidData, "Unexpected delayed AV1 decoded picture"));
    output_ = EndOfStream{};
    return R::Success(SubmitStatus::Accepted);
  }
  CodecResult<void> Close() override {
    output_.reset();
    if (std::exchange(opened_, false)) aom_codec_destroy(&codec_);
    return CodecResult<void>::Success();
  }

private:
  VideoFrame CopyPixels(const aom_image_t& image, const EncodedPacket& packet) const {
    VideoFormat format{.width = image.d_w, .height = image.d_h, .visible = {0, 0, image.d_w, image.d_h},
        .pixel_format = layout_};
    format.range = image.range == AOM_CR_FULL_RANGE ? ColorRange::Full : ColorRange::Limited;
    if (image.cp == 1) format.primaries = ColorPrimaries::BT709;
    else if (image.cp == 5) format.primaries = ColorPrimaries::BT601_625;
    else if (image.cp == 6) format.primaries = ColorPrimaries::BT601_525;
    if (image.tc == 1) format.transfer = ColorTransfer::BT709;
    else if (image.tc == 8) format.transfer = ColorTransfer::Linear;
    else if (image.tc == 13) format.transfer = ColorTransfer::SRGB;
    if (image.mc == 1) format.matrix = ColorMatrix::BT709;
    else if (image.mc == 5 || image.mc == 6) format.matrix = ColorMatrix::BT601;
    VideoFrame frame{format, packet.pts, packet.duration, {}};
    const bool planar = layout_ == VideoPixelFormat::I420;
    const auto depth = layout_ == VideoPixelFormat::P010 ? 2u : 1u;
    for (unsigned plane = 0; plane < (planar ? 3u : 2u); ++plane) {
      const auto columns = plane ? (image.d_w + 1) / 2 : image.d_w;
      const auto rows = plane ? (image.d_h + 1) / 2 : image.d_h;
      const auto components = plane && !planar ? 2u : 1u;
      const auto pixel = components * depth;
      const auto row = std::size_t(columns) * pixel;
      Bytes bytes(row * rows);
      for (std::size_t y = 0; y < rows; ++y)
        for (std::size_t x = 0; x < columns; ++x)
          for (unsigned index = 0; index < components; ++index) {
            const auto component = plane + index;
            const auto* source = image.planes[component] + y * image.stride[component] + depth * x;
            auto* target = bytes.data() + y * row + x * pixel + depth * index;
            if (depth == 2) {
              std::uint16_t sample;
              std::memcpy(&sample, source, 2);
              target[0] = std::byte(sample << 6);
              target[1] = std::byte(sample >> 2);
            } else *target = std::byte(*source);
          }
      frame.planes.push_back({Own(std::move(bytes)), row, pixel});
    }
    return frame;
  }

  aom_codec_ctx_t codec_{};
  std::optional<BackendOutput> output_;
  unsigned width_ = 0, height_ = 0;
  VideoPixelFormat layout_ = VideoPixelFormat::NV12;
  bool opened_ = false;
};

} // namespace

std::unique_ptr<VideoEncoderBackend> MakeAv1EncoderBackend() { return std::make_unique<AomEncoder>(); }
std::unique_ptr<DecoderBackend> MakeAv1DecoderBackend() { return std::make_unique<AomDecoder>(); }

} // namespace huxerui::media::detail

#endif
