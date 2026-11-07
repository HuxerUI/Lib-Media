#include "codec/decoder/backend.h"
#include "codec/decoder/copy.h"

#include <bit>
#include <cstring>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>

namespace huxerui::media::detail {
namespace {
CodecError AndroidError(media_status_t status, const char* message) {
  return Error(CodecErrorCode::PlatformFailure, message, std::to_string(status));
}
class AndroidDecoder final : public DecoderBackend {
public:
  CodecResult<void> Open(const DecoderConfig& config) override {
    config_ = config;
    const auto* audio = std::get_if<AudioDecoderConfig>(&config_);
    codec_ = AMediaCodec_createDecoderByType(audio ? "audio/mp4a-latm" : "video/avc");
    if (!codec_) return CodecResult<void>::Failure(Error(CodecErrorCode::Unsupported, "No Android decoder for this MIME type"));
    auto* format = AMediaFormat_new();
    AMediaFormat_setString(format, "mime", audio ? "audio/mp4a-latm" : "video/avc");
    if (audio) {
      AMediaFormat_setInt32(format, "sample-rate", audio->output.sample_rate);
      AMediaFormat_setInt32(format, "channel-count", audio->output.ChannelCount());
      AMediaFormat_setInt32(format, "pcm-encoding", 2);
      AMediaFormat_setBuffer(format, "csd-0", audio->initialization_data.data(), audio->initialization_data.size());
    } else {
      const auto& video = std::get<VideoDecoderConfig>(config_);
      AMediaFormat_setInt32(format, "width", video.width);
      AMediaFormat_setInt32(format, "height", video.height);
      AMediaFormat_setInt32(format, "color-format", 19);
      auto avc = ParseAvc(video.initialization_data).Value();
      length_size_ = avc.length_size;
      Bytes sps, pps;
      for (const auto& parameter : avc.parameters) {
        auto& bytes = (std::to_integer<unsigned>(parameter[0]) & 31) == 7 ? sps : pps;
        bytes.insert(bytes.end(), {std::byte{0}, std::byte{0}, std::byte{0}, std::byte{1}});
        bytes.insert(bytes.end(), parameter.begin(), parameter.end());
      }
      AMediaFormat_setBuffer(format, "csd-0", sps.data(), sps.size());
      AMediaFormat_setBuffer(format, "csd-1", pps.data(), pps.size());
    }
    auto status = AMediaCodec_configure(codec_, format, nullptr, nullptr, 0);
    const auto deleted = AMediaFormat_delete(format);
    if (status != AMEDIA_OK) return CodecResult<void>::Failure(AndroidError(status, "MediaCodec configure failed"));
    if (deleted != AMEDIA_OK) return CodecResult<void>::Failure(AndroidError(deleted, "MediaFormat release failed"));
    status = AMediaCodec_start(codec_);
    if (status != AMEDIA_OK) return CodecResult<void>::Failure(AndroidError(status, "MediaCodec start failed"));
    started_ = true;
    return CodecResult<void>::Success();
  }
  CodecResult<SubmitStatus> Submit(const EncodedPacket& packet) override {
    Bytes converted;
    auto bytes = packet.data.AsBytes();
    if (std::holds_alternative<VideoDecoderConfig>(config_)) {
      auto result = AnnexB(packet, length_size_);
      if (!result.Succeeded()) return CodecResult<SubmitStatus>::Failure(result.Error());
      converted = std::move(result).Value(); bytes = converted;
    }
    return Queue(bytes, packet.pts.Rescale(1000000)->ticks, packet.key_frame ? AMEDIACODEC_BUFFER_FLAG_KEY_FRAME : 0);
  }
  CodecResult<std::optional<BackendOutput>> Poll() override {
    using R = CodecResult<std::optional<BackendOutput>>;
    if (eos_) return R::Success(std::nullopt);
    if (pending_eos_) { eos_ = true; return R::Success(BackendOutput{EndOfStream{}}); }
    AMediaCodecBufferInfo info{};
    const auto index = AMediaCodec_dequeueOutputBuffer(codec_, &info, 0);
    if (index == AMEDIACODEC_INFO_TRY_AGAIN_LATER || index == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) return R::Success(std::nullopt);
    if (index == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
      if (format_) {
        const auto status = AMediaFormat_delete(format_); format_ = nullptr;
        if (status != AMEDIA_OK) return R::Failure(AndroidError(status, "MediaFormat release failed"));
      }
      format_ = AMediaCodec_getOutputFormat(codec_);
      return R::Success(std::nullopt);
    }
    if (index < 0) return R::Failure(AndroidError(static_cast<media_status_t>(index), "MediaCodec output dequeue failed"));
    if (!format_) format_ = AMediaCodec_getOutputFormat(codec_);
    auto result = R::Success(std::nullopt);
    if (info.size < 0) {
      result = R::Failure(Error(CodecErrorCode::InvalidData, "MediaCodec returned a negative output size"));
    } else if (info.size > 0) {
      // The returned pointer starts at the data; offset and out_size are unreliable through API 35.
      const auto* data = AMediaCodec_getOutputBuffer(codec_, index, nullptr);
      if (!data) result = R::Failure(Error(CodecErrorCode::InvalidData, "MediaCodec returned no output data"));
      else result = Copy({reinterpret_cast<const std::byte*>(data), std::size_t(info.size)}, {info.presentationTimeUs, 1000000});
    }
    const auto status = AMediaCodec_releaseOutputBuffer(codec_, index, false);
    if (status != AMEDIA_OK) return R::Failure(AndroidError(status, "MediaCodec output release failed"));
    if (!result.Succeeded()) return result;
    if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) {
      if (result.Value()) pending_eos_ = true;
      else { eos_ = true; return R::Success(BackendOutput{EndOfStream{}}); }
    }
    return result;
  }
  CodecResult<SubmitStatus> Finish() override { return Queue({}, 0, AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM); }
  CodecResult<void> Close() override {
    std::optional<CodecError> error;
    if (codec_) {
      if (started_) {
        const auto status = AMediaCodec_stop(codec_);
        if (status != AMEDIA_OK) error = AndroidError(status, "MediaCodec stop failed");
      }
      const auto status = AMediaCodec_delete(codec_); codec_ = nullptr;
      if (status != AMEDIA_OK) error = AndroidError(status, "MediaCodec delete failed");
    }
    if (format_) {
      const auto status = AMediaFormat_delete(format_); format_ = nullptr;
      if (status != AMEDIA_OK) error = AndroidError(status, "MediaFormat release failed");
    }
    return error ? CodecResult<void>::Failure(*error) : CodecResult<void>::Success();
  }
private:
  int Get(const char* key, int fallback) const {
    std::int32_t value;
    return format_ && AMediaFormat_getInt32(format_, key, &value) ? value : fallback;
  }
  CodecResult<SubmitStatus> Queue(std::span<const std::byte> bytes, std::int64_t pts, std::uint32_t flags) {
    using R = CodecResult<SubmitStatus>;
    const auto index = AMediaCodec_dequeueInputBuffer(codec_, 0);
    if (index == AMEDIACODEC_INFO_TRY_AGAIN_LATER) return R::Success(SubmitStatus::WouldBlock);
    if (index < 0) return R::Failure(AndroidError(static_cast<media_status_t>(index), "MediaCodec input dequeue failed"));
    std::size_t capacity = 0;
    auto* target = AMediaCodec_getInputBuffer(codec_, index, &capacity);
    if (!target || bytes.size() > capacity) return R::Failure(Error(CodecErrorCode::InvalidData, "Access unit exceeds MediaCodec input capacity"));
    if (!bytes.empty()) std::memcpy(target, bytes.data(), bytes.size());
    const auto status = AMediaCodec_queueInputBuffer(codec_, index, 0, bytes.size(), std::bit_cast<std::uint64_t>(pts), flags);
    if (status != AMEDIA_OK) return R::Failure(AndroidError(status, "MediaCodec input queue failed"));
    return R::Success(SubmitStatus::Accepted);
  }
  CodecResult<std::optional<BackendOutput>> Copy(std::span<const std::byte> bytes, MediaTimestamp pts) {
    using R = CodecResult<std::optional<BackendOutput>>;
    auto storage = std::make_shared<Bytes>(bytes.begin(), bytes.end());
    BufferReference data(*storage, storage);
    if (const auto* audio = std::get_if<AudioDecoderConfig>(&config_)) {
      auto format = audio->output;
      const auto channels = Get("channel-count", 0);
      if (channels != 1 && channels != 2) return R::Failure(Error(CodecErrorCode::Unsupported, "Unsupported MediaCodec PCM channel count"));
      format.channel_layout = channels == 1 ? AudioChannelLayout::Mono : AudioChannelLayout::Stereo;
      format.sample_rate = Get("sample-rate", 0);
      format.packing = AudioPacking::Interleaved;
      const auto encoding = Get("pcm-encoding", 2);
      if (encoding != 2 && encoding != 4) return R::Failure(Error(CodecErrorCode::Unsupported, "Unsupported MediaCodec PCM sample format"));
      format.sample_format = encoding == 4 ? AudioSampleFormat::F32 : AudioSampleFormat::S16;
      const auto stride = format.BytesPerSample() * format.ChannelCount();
      if (bytes.size() % stride || bytes.size() / stride > 1024) return R::Failure(Error(CodecErrorCode::InvalidData, "Unexpected AAC output sample count"));
      auto copied = CopyAudio({format, pts, static_cast<std::uint32_t>(bytes.size() / stride), {data}}, audio->output);
      if (!copied.Succeeded()) return R::Failure(copied.Error());
      return R::Success(BackendOutput{DecodedFrame{std::move(copied).Value()}});
    }
    const auto& config = std::get<VideoDecoderConfig>(config_);
    const auto color = Get("color-format", 0);
    if (color != 19 && color != 21) return R::Failure(Error(CodecErrorCode::Unsupported, "MediaCodec output is not CPU I420/NV12", std::to_string(color)));
    const auto width = Get("width", 0), height = Get("height", 0);
    const auto stride = Get("stride", width), slice = Get("slice-height", height);
    if (width <= 0 || height <= 0 || width > config.width || height > config.height || stride < width || slice < height)
      return R::Failure(Error(CodecErrorCode::InvalidData, "Invalid MediaCodec geometry"));
    const auto ysize = std::size_t(stride) * slice;
    const auto cstride = color == 19 ? (std::size_t(stride) + 1) / 2 : ((std::size_t(stride) + 1) / 2) * 2;
    const auto csize = cstride * ((std::size_t(slice) + 1) / 2);
    if (ysize > bytes.size() || csize > (bytes.size() - ysize) / (color == 19 ? 2 : 1))
      return R::Failure(Error(CodecErrorCode::InvalidData, "MediaCodec YUV planes exceed output buffer"));
    VideoFormat format;
    format.width = width; format.height = height;
    const auto left = Get("crop-left", 0), top = Get("crop-top", 0);
    const auto right = Get("crop-right", width - 1), bottom = Get("crop-bottom", height - 1);
    if (left < 0 || top < 0 || right < left || bottom < top || right >= width || bottom >= height)
      return R::Failure(Error(CodecErrorCode::InvalidData, "Invalid MediaCodec crop"));
    format.visible = {std::uint32_t(left), std::uint32_t(top), std::uint32_t(right - left + 1), std::uint32_t(bottom - top + 1)};
    format.pixel_format = color == 19 ? VideoPixelFormat::I420 : VideoPixelFormat::NV12;
    const auto range = Get("color-range", 0), standard = Get("color-standard", 0), transfer = Get("color-transfer", 0);
    format.range = range == 1 ? ColorRange::Full : range == 2 ? ColorRange::Limited : ColorRange::Unknown;
    if (standard == 1) { format.primaries = ColorPrimaries::BT709; format.matrix = ColorMatrix::BT709; }
    else if (standard == 2 || standard == 4) { format.primaries = standard == 2 ? ColorPrimaries::BT601_625 : ColorPrimaries::BT601_525; format.matrix = ColorMatrix::BT601; }
    else if (standard == 6) { format.primaries = ColorPrimaries::BT2020; format.matrix = ColorMatrix::BT2020; }
    format.transfer = transfer == 1 ? ColorTransfer::Linear : transfer == 3 ? ColorTransfer::BT709 : transfer == 6 ? ColorTransfer::PQ : transfer == 7 ? ColorTransfer::HLG : ColorTransfer::Unknown;
    std::vector<VideoPlane> planes{{data.Slice(0, ysize), std::size_t(stride), 1}, {data.Slice(ysize, csize), cstride, color == 19 ? 1u : 2u}};
    if (color == 19) planes.push_back({data.Slice(ysize + csize, csize), cstride, 1});
    auto copied = CopyVideo(format, pts, std::nullopt, std::move(planes), config.output);
    if (!copied.Succeeded()) return R::Failure(copied.Error());
    return R::Success(BackendOutput{DecodedFrame{std::move(copied).Value()}});
  }
  DecoderConfig config_;
  AMediaCodec* codec_ = nullptr;
  AMediaFormat* format_ = nullptr;
  unsigned length_size_ = 4;
  bool started_ = false, eos_ = false, pending_eos_ = false;
};
} // namespace
std::unique_ptr<DecoderBackend> MakeDecoderBackend() { return std::make_unique<AndroidDecoder>(); }
} // namespace huxerui::media::detail
