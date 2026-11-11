#include "codec/decoder/backend.h"
#include "media_video_android.h"
#include "codec/decoder/copy.h"
#include "codec/hevc.h"

#include <android/api-level.h>
#include <android/native_window_jni.h>
#include <bit>
#include <cstring>
#include <huxerui/android/external_texture.h>
#include <huxerui/android/platform_registry.h>
#include <huxerui/app.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <mutex>

#include "format/platform_video_storage.h"

namespace huxerui::media::detail {
namespace {
CodecError AndroidError(media_status_t status, const char* message) {
  return Error(CodecErrorCode::PlatformFailure, message, std::to_string(status));
}

struct PreviewSurface {
  std::shared_ptr<android::SurfaceStreamTexture> texture;
  std::unique_ptr<ANativeWindow, decltype(&ANativeWindow_release)> window{nullptr, ANativeWindow_release};
};

class SurfaceOutput {
public:
  explicit SurfaceOutput(AMediaCodec* codec) : codec_(codec) {}

  bool CanAcquire() {
    std::lock_guard lock(mutex_);
    return held_ < 4;
  }

  void Acquire() {
    std::lock_guard lock(mutex_);
    ++held_;
  }

  media_status_t Release(std::optional<std::size_t>& index, bool render) {
    std::lock_guard lock(mutex_);
    if (!index) return AMEDIA_OK;
    const auto value = *std::exchange(index, std::nullopt);
    --held_;
    return codec_ ? AMediaCodec_releaseOutputBuffer(codec_, value, render) : AMEDIA_ERROR_INVALID_OPERATION;
  }

  void Retire() {
    std::lock_guard lock(mutex_);
    codec_ = nullptr;
  }

private:
  std::mutex mutex_;
  AMediaCodec* codec_;
  unsigned held_ = 0;
};

struct SurfacePicture final : PlatformVideoStorage {
  SurfacePicture(const VideoFormat& format, std::shared_ptr<SurfaceOutput> output, std::size_t index)
      : PlatformVideoStorage(format.width, format.height, format.pixel_format), output(std::move(output)), index(index) {
    this->output->Acquire();
  }
  ~SurfacePicture() override { output->Release(index, false); }

  std::shared_ptr<SurfaceOutput> output;
  mutable std::optional<std::size_t> index;
};

class AndroidDecoder final : public DecoderBackend {
public:
  explicit AndroidDecoder(std::shared_ptr<const PreviewSurface> preview = {}) : preview_(std::move(preview)) {}

  CodecResult<void> Open(const DecoderConfig& config) override {
    config_ = config;
    const auto* audio = std::get_if<AudioDecoderConfig>(&config_);
    const auto* video = std::get_if<VideoDecoderConfig>(&config_);
    const bool hevc = video && video->codec == CodecId::Hevc;
    if (hevc && video->output == VideoPixelFormat::P010 && android_get_device_api_level() < 33)
      return CodecResult<void>::Failure(Error(CodecErrorCode::Unsupported, "P010 decoding requires Android API 33 or later"));
    const char* mime = hevc ? "video/hevc" : "video/avc";
    if (audio) {
      switch (audio->codec) {
        case CodecId::Aac: mime = "audio/mp4a-latm"; break;
        case CodecId::Opus: mime = "audio/opus"; break;
        case CodecId::Flac: mime = "audio/flac"; break;
        case CodecId::Mp3: mime = "audio/mpeg"; break;
        default: return CodecResult<void>::Failure(Error(CodecErrorCode::Unsupported, "Unsupported Android audio codec"));
      }
    } else if (video->codec == CodecId::Vp8) {
      mime = "video/x-vnd.on2.vp8";
    } else if (video->codec == CodecId::Vp9) {
      mime = "video/x-vnd.on2.vp9";
    } else if (video->codec == CodecId::Av1) {
      mime = "video/av01";
    }
    codec_ = AMediaCodec_createDecoderByType(mime);
    if (!codec_) return CodecResult<void>::Failure(Error(CodecErrorCode::Unsupported, "No Android decoder for this MIME type"));
    auto* format = AMediaFormat_new();
    if (!format) return CodecResult<void>::Failure(Error(CodecErrorCode::ResourceExhausted, "Cannot allocate decoder MediaFormat"));
    std::unique_ptr<AMediaFormat, decltype(&AMediaFormat_delete)> format_owner(format, AMediaFormat_delete);
    AMediaFormat_setString(format, "mime", mime);
    if (audio) {
      audio_sample_limit_ = AudioSampleLimit(*audio).Value();
      if (audio->codec == CodecId::Mp3) mp3_.emplace(audio->output, audio_sample_limit_);
      AMediaFormat_setInt32(format, "sample-rate", audio->output.sample_rate);
      AMediaFormat_setInt32(format, "channel-count", audio->output.ChannelCount());
      AMediaFormat_setInt32(format, "pcm-encoding", audio->output.sample_format == AudioSampleFormat::F32 ? 4 : 2);
      const auto header = AudioDecoderHeader(*audio);
      if (!header.empty()) AMediaFormat_setBuffer(format, "csd-0", header.data(), header.size());
      if (audio->codec == CodecId::Opus) {
        const std::uint64_t delay = 0;
        AMediaFormat_setBuffer(format, "csd-1", &delay, sizeof(delay));
        AMediaFormat_setBuffer(format, "csd-2", &delay, sizeof(delay));
      }
    } else {
      AMediaFormat_setInt32(format, "width", video->width);
      AMediaFormat_setInt32(format, "height", video->height);
      if (video->output_storage == VideoStorage::Cpu)
        AMediaFormat_setInt32(format, "color-format", video->output == VideoPixelFormat::P010 ? 54 : 19);
      else if (!preview_)
        AMediaFormat_setInt32(format, "allow-frame-drop", 0);
      if (hevc) {
        length_size_ = ParseHevcConfiguration(video->initialization_data, true, false).length;
        const auto header = HevcAnnexBConfiguration(video->initialization_data);
        AMediaFormat_setBuffer(format, "csd-0", header.data(), header.size());
      } else if (video->codec == CodecId::Av1) {
        AMediaFormat_setBuffer(format, "csd-0", video->initialization_data.data(), video->initialization_data.size());
      } else if (video->codec == CodecId::H264) {
        auto avc = ParseAvc(video->initialization_data).Value();
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
    }
    ANativeWindow* window = nullptr;
    if (preview_) {
      window = preview_->window.get();
    } else if (video && video->output_storage == VideoStorage::Platform) {
      auto reader = OpenVideoImageReader(*video);
      if (!reader.Succeeded()) return CodecResult<void>::Failure(reader.Error());
      images_ = std::move(reader).Value();
      window = VideoImageReaderWindow(images_.get());
      if (!window) return CodecResult<void>::Failure(Error(CodecErrorCode::Unsupported, "Cannot open the Android decoder surface"));
    }
    auto status = AMediaCodec_configure(codec_, format, window, nullptr, 0);
    const auto deleted = AMediaFormat_delete(format_owner.release());
    if (status != AMEDIA_OK) return CodecResult<void>::Failure(window ?
        Error(CodecErrorCode::Unsupported, "MediaCodec rejected its output surface", std::to_string(status)) :
        AndroidError(status, "MediaCodec configure failed"));
    if (deleted != AMEDIA_OK) return CodecResult<void>::Failure(AndroidError(deleted, "MediaFormat release failed"));
    status = AMediaCodec_start(codec_);
    if (status != AMEDIA_OK) return CodecResult<void>::Failure(AndroidError(status, "MediaCodec start failed"));
    started_ = true;
    if (preview_) surface_output_ = std::make_shared<SurfaceOutput>(codec_);
    return CodecResult<void>::Success();
  }
  CodecResult<SubmitStatus> Submit(const EncodedPacket& packet) override {
    Bytes converted;
    auto bytes = packet.data.AsBytes();
    if (length_size_) {
      auto result = AnnexB(packet, length_size_);
      if (!result.Succeeded()) return CodecResult<SubmitStatus>::Failure(result.Error());
      converted = std::move(result).Value(); bytes = converted;
    }
    auto queued = Queue(bytes, packet.pts.Rescale(1000000)->ticks, packet.key_frame ? AMEDIACODEC_BUFFER_FLAG_KEY_FRAME : 0);
    if (mp3_ && queued.Succeeded() && queued.Value() == SubmitStatus::Accepted) mp3_->Expect(packet.pts);
    return queued;
  }
  CodecResult<std::optional<BackendOutput>> Poll() override {
    using R = CodecResult<std::optional<BackendOutput>>;
    if (eos_) return R::Success(std::nullopt);
    if (surface_pts_) {
      VideoFormat format{.pixel_format = VideoPixelFormat::Opaque};
      ReadColor(format);
      auto acquired = ReadVideoImage(images_, format, *surface_pts_);
      if (!acquired.Succeeded()) return R::Failure(acquired.Error());
      if (!acquired.Value()) return R::Success(std::nullopt);
      surface_pts_.reset();
      return R::Success(BackendOutput{DecodedFrame{std::move(*acquired.Value())}});
    }
    if (mp3_) {
      if (auto frame = mp3_->Read()) return R::Success(BackendOutput{DecodedFrame{std::move(*frame)}});
    }
    if (pending_eos_) return EndOutput();
    if (surface_output_ && !surface_output_->CanAcquire()) return R::Success(std::nullopt);
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
    if (images_ || surface_output_) {
      pending_eos_ = (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) != 0;
      const bool render = !(info.flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG) && (!pending_eos_ || info.size > 0);
      if (render && surface_output_) return SurfaceFrame(index, {info.presentationTimeUs, 1000000});
      const auto status = AMediaCodec_releaseOutputBuffer(codec_, index, render && bool(images_));
      if (status != AMEDIA_OK) return R::Failure(AndroidError(status, "MediaCodec surface output release failed"));
      if (render) surface_pts_ = MediaTimestamp{info.presentationTimeUs, 1000000};
      else if (pending_eos_) return EndOutput();
      return R::Success(std::nullopt);
    }
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
      pending_eos_ = true;
      if (!result.Value()) return EndOutput();
    }
    return result;
  }
  CodecResult<SubmitStatus> Finish() override { return Queue({}, 0, AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM); }
  bool PresentationOrdered() const noexcept override { return images_ || surface_output_; }
  CodecResult<void> Close() override {
    std::optional<CodecError> error;
    if (surface_output_) surface_output_->Retire();
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
    surface_pts_.reset();
    images_.reset();
    return error ? CodecResult<void>::Failure(*error) : CodecResult<void>::Success();
  }
private:
  CodecResult<std::optional<BackendOutput>> SurfaceFrame(std::size_t index, MediaTimestamp pts) {
    using R = CodecResult<std::optional<BackendOutput>>;
    const auto& config = std::get<VideoDecoderConfig>(config_);
    const auto width = Get("width", config.width), height = Get("height", config.height);
    const auto left = Get("crop-left", 0), top = Get("crop-top", 0);
    const auto right = Get("crop-right", width - 1), bottom = Get("crop-bottom", height - 1);
    if (width <= 0 || height <= 0 || left < 0 || top < 0 || right < left || bottom < top || right >= width || bottom >= height)
      return R::Failure(Error(CodecErrorCode::InvalidData, "Invalid MediaCodec preview geometry"));
    VideoFormat format{.width = std::uint32_t(width), .height = std::uint32_t(height),
        .visible = {std::uint32_t(left), std::uint32_t(top), std::uint32_t(right - left + 1), std::uint32_t(bottom - top + 1)},
        .pixel_format = VideoPixelFormat::Opaque};
    ReadColor(format);
    auto frame = VideoFrameAccess::FromPlatform(format, pts, std::nullopt,
        std::make_shared<SurfacePicture>(format, surface_output_, index));
    return R::Success(BackendOutput{DecodedFrame{std::move(frame)}});
  }

  void ReadColor(VideoFormat& format) const {
    const auto range = Get("color-range", 0), standard = Get("color-standard", 0), transfer = Get("color-transfer", 0);
    format.range = range == 1 ? ColorRange::Full : range == 2 ? ColorRange::Limited : ColorRange::Unknown;
    if (standard == 1) { format.primaries = ColorPrimaries::BT709; format.matrix = ColorMatrix::BT709; }
    else if (standard == 2 || standard == 4) { format.primaries = standard == 2 ? ColorPrimaries::BT601_625 : ColorPrimaries::BT601_525; format.matrix = ColorMatrix::BT601; }
    else if (standard == 6) { format.primaries = ColorPrimaries::BT2020; format.matrix = ColorMatrix::BT2020; }
    format.transfer = transfer == 1 ? ColorTransfer::Linear : transfer == 3 ? ColorTransfer::BT709 : transfer == 6 ? ColorTransfer::PQ : transfer == 7 ? ColorTransfer::HLG : ColorTransfer::Unknown;
  }

  CodecResult<std::optional<BackendOutput>> EndOutput() {
    using R = CodecResult<std::optional<BackendOutput>>;
    if (mp3_ && !mp3_->Drained())
      return R::Failure(Error(CodecErrorCode::InvalidData, "MediaCodec MP3 drain has missing PCM samples"));
    eos_ = true;
    return R::Success(BackendOutput{EndOfStream{}});
  }

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
      if (bytes.size() % stride || bytes.size() / stride > audio_sample_limit_ * (mp3_ ? 2 : 1))
        return R::Failure(Error(CodecErrorCode::InvalidData, "Unexpected MediaCodec PCM sample count"));
      auto copied = CopyAudio({format, pts, static_cast<std::uint32_t>(bytes.size() / stride), {data}}, audio->output);
      if (!copied.Succeeded()) return R::Failure(copied.Error());
      if (mp3_) {
        const auto buffered = mp3_->Append(copied.Value());
        if (!buffered.Succeeded()) return R::Failure(buffered.Error());
        if (auto frame = mp3_->Read()) return R::Success(BackendOutput{DecodedFrame{std::move(*frame)}});
        return R::Success(std::nullopt);
      }
      return R::Success(BackendOutput{DecodedFrame{std::move(copied).Value()}});
    }
    const auto& config = std::get<VideoDecoderConfig>(config_);
    const auto color = Get("color-format", 0);
    const bool ten_bit = color == 54;
    const auto component = ten_bit ? 2u : 1u;
    if (ten_bit != (config.output == VideoPixelFormat::P010) || (!ten_bit && color != 19 && color != 21))
      return R::Failure(Error(CodecErrorCode::Unsupported, "Unexpected MediaCodec CPU YUV layout", std::to_string(color)));
    const auto width = Get("width", 0), height = Get("height", 0);
    const auto stride = Get("stride", ten_bit ? 0 : width), slice = Get("slice-height", height);
    if (width <= 0 || height <= 0 || width > config.width || height > config.height || slice < height ||
        stride < width * int(component) || (ten_bit && (stride % 4 || stride < (width + 1) / 2 * 4)))
      return R::Failure(Error(CodecErrorCode::InvalidData, "Invalid MediaCodec geometry"));
    if (std::size_t(slice) > bytes.size() / std::size_t(stride))
      return R::Failure(Error(CodecErrorCode::InvalidData, "MediaCodec luma exceeds output buffer"));
    const auto ysize = std::size_t(stride) * slice;
    const auto cstride = ten_bit ? std::size_t(stride) : (std::size_t(stride) + 1) / 2 * (color == 19 ? 1 : 2);
    const auto crows = (std::size_t(slice) + 1) / 2;
    if (crows > (bytes.size() - ysize) / cstride / (color == 19 ? 2 : 1))
      return R::Failure(Error(CodecErrorCode::InvalidData, "MediaCodec YUV planes exceed output buffer"));
    const auto csize = cstride * crows;
    VideoFormat format;
    format.width = width; format.height = height;
    const auto left = Get("crop-left", 0), top = Get("crop-top", 0);
    const auto right = Get("crop-right", width - 1), bottom = Get("crop-bottom", height - 1);
    if (left < 0 || top < 0 || right < left || bottom < top || right >= width || bottom >= height)
      return R::Failure(Error(CodecErrorCode::InvalidData, "Invalid MediaCodec crop"));
    format.visible = {std::uint32_t(left), std::uint32_t(top), std::uint32_t(right - left + 1), std::uint32_t(bottom - top + 1)};
    format.pixel_format = ten_bit ? VideoPixelFormat::P010 : color == 19 ? VideoPixelFormat::I420 : VideoPixelFormat::NV12;
    ReadColor(format);
    std::vector<VideoPlane> planes{{data.Slice(0, ysize), std::size_t(stride), component},
        {data.Slice(ysize, csize), cstride, color == 19 ? 1u : 2 * component}};
    if (color == 19) planes.push_back({data.Slice(ysize + csize, csize), cstride, 1});
    auto copied = CopyVideo(format, pts, std::nullopt, std::move(planes), config.output);
    if (!copied.Succeeded()) return R::Failure(copied.Error());
    return R::Success(BackendOutput{DecodedFrame{std::move(copied).Value()}});
  }
  DecoderConfig config_;
  std::shared_ptr<const PreviewSurface> preview_;
  std::shared_ptr<SurfaceOutput> surface_output_;
  AMediaCodec* codec_ = nullptr;
  AMediaFormat* format_ = nullptr;
  std::shared_ptr<AImageReader> images_;
  std::optional<MediaTimestamp> surface_pts_;
  unsigned audio_sample_limit_ = 0;
  std::optional<PcmPackets> mp3_;
  unsigned length_size_ = 0;
  bool started_ = false, eos_ = false, pending_eos_ = false;
};
} // namespace
std::unique_ptr<DecoderBackend> MakeDecoderBackend() { return std::make_unique<AndroidDecoder>(); }

void InstallSurfacePreview(ApplicationContext& context) {
  context.RegisterPlatformModule<SurfacePreview, SurfacePreviewOptions>(surface_preview_type,
      [](Runtime& runtime, const SurfacePreviewOptions& options) {
        const auto env = android::GetPlatformEnv(runtime);
        auto target = std::make_shared<PreviewSurface>();
        target->texture = android::SurfaceStreamTexture::Create(env.jni, options.size,
            int(options.config.width), int(options.config.height));
        const auto surface = target->texture->Surface(env.jni);
        target->window.reset(ANativeWindow_fromSurface(env.jni, surface.Get()));
        if (!target->window) throw std::runtime_error("Cannot obtain the MediaCodec preview surface");
        auto decoder = VideoDecoderAccess::Create(options.config, [target] {
          return std::make_unique<AndroidDecoder>(target);
        });
        if (!decoder.Succeeded()) throw std::runtime_error(decoder.Error().message);
        return SurfacePreview{std::move(decoder).Value(), target->texture};
      });
}

CodecResult<void> PresentSurfaceFrame(const VideoFrame& frame) {
  const auto* picture = dynamic_cast<const SurfacePicture*>(VideoFrameAccess::PlatformStorage(frame).get());
  if (!picture) return CodecResult<void>::Failure(Error(CodecErrorCode::InvalidArgument, "Expected a MediaCodec preview picture"));
  const auto status = picture->output->Release(picture->index, true);
  if (status != AMEDIA_OK) return CodecResult<void>::Failure(AndroidError(status, "Cannot render the MediaCodec preview picture"));
  return CodecResult<void>::Success();
}
} // namespace huxerui::media::detail
