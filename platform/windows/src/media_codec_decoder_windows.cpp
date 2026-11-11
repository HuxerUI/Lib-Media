#include "codec/decoder/backend.h"
#include "codec/decoder/copy.h"
#include "codec/hevc.h"
#include "container/container_io.h"
#include "media_codec_transform_windows.h"
#include "media_video_windows.h"

#include <algorithm>
#include <cstring>
#include <deque>

#include <codecapi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wmcodecdsp.h>
#include <wrl/client.h>

namespace huxerui::media::detail {
namespace {
using Microsoft::WRL::ComPtr;
struct MfFailure { CodecError error; };
void Require(HRESULT status, const char* operation, CodecErrorCode code = CodecErrorCode::PlatformFailure) {
  if (FAILED(status)) throw MfFailure{Error(code, operation, std::to_string(status))};
}
template<class T, class Function> CodecResult<T> Guard(Function function) {
  try {
    if constexpr (std::is_void_v<T>) { function(); return CodecResult<void>::Success(); }
    else return CodecResult<T>::Success(function());
  } catch (const MfFailure& error) { return CodecResult<T>::Failure(error.error); }
}

GUID AudioSubtype(CodecId codec) {
  switch (codec) {
    case CodecId::Aac: return MFAudioFormat_AAC;
    case CodecId::Opus: return MFAudioFormat_Opus;
    case CodecId::Flac: return MFAudioFormat_FLAC;
    case CodecId::Mp3: return MFAudioFormat_MP3;
    default: throw MfFailure{Error(CodecErrorCode::Unsupported, "Unsupported Media Foundation audio codec")};
  }
}

void ConfigureAudioInput(IMFMediaType& input, const AudioDecoderConfig& config) {
  Require(input.SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, config.output.ChannelCount()), "Cannot set audio input channels");
  Require(input.SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, config.output.sample_rate), "Cannot set audio input rate");
  if (config.codec == CodecId::Aac) {
    Require(input.SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, config.output.BytesPerSample() * 8), "Cannot set AAC output precision");
    Require(input.SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0), "Cannot configure raw AAC");
    std::vector<UINT8> cookie(12, 0);
    cookie[2] = 0xfe;
    for (auto byte : config.initialization_data) cookie.push_back(std::to_integer<UINT8>(byte));
    Require(input.SetBlob(MF_MT_USER_DATA, cookie.data(), static_cast<UINT32>(cookie.size())), "Cannot set AAC initialization data");
  } else if (config.codec == CodecId::Flac) {
    const auto stream = ParseFlacStreamInfo(config.initialization_data, ContainerErrorCode::InvalidArgument);
    Require(input.SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, stream.bits), "Cannot set FLAC precision");
  } else if (config.codec == CodecId::Opus) {
    const auto header = AudioDecoderHeader(config);
    Require(input.SetBlob(MF_MT_USER_DATA, reinterpret_cast<const UINT8*>(header.data()), static_cast<UINT32>(header.size())),
        "Cannot set Opus initialization data");
  } else {
    Require(input.SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 1), "Cannot set MP3 block alignment");
  }
}

class WindowsDecoder final : public DecoderBackend {
public:
  std::uint32_t PresentationTimescale() const noexcept override { return 10000000; }
  bool PresentationOrdered() const noexcept override { return bool(device_); }

  CodecResult<void> Open(const DecoderConfig& config) override {
    return Guard<void>([&] {
      config_ = config;
      Require(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "COM initialization failed"); com_ = true;
      Require(MFStartup(MF_VERSION, MFSTARTUP_FULL), "Media Foundation initialization failed"); mf_ = true;
      const auto* audio = std::get_if<AudioDecoderConfig>(&config_);
      const auto* video = std::get_if<VideoDecoderConfig>(&config_);
      const bool hevc = video && video->codec == CodecId::Hevc;
      const bool vpx = video && (video->codec == CodecId::Vp8 || video->codec == CodecId::Vp9);
      const GUID subtype = audio ? AudioSubtype(audio->codec) : hevc ? MFVideoFormat_HEVC :
          vpx ? (video->codec == CodecId::Vp8 ? MFVideoFormat_VP80 : MFVideoFormat_VP90) : MFVideoFormat_H264;
      if (audio && audio->codec != CodecId::Aac) {
        const MFT_REGISTER_TYPE_INFO type{MFMediaType_Audio, subtype};
        Require(OpenSynchronousTransform(MFT_CATEGORY_AUDIO_DECODER, &type, nullptr, decoder_),
            "No synchronous Media Foundation audio decoder for this codec", CodecErrorCode::Unsupported);
      } else if (hevc || vpx) {
        const MFT_REGISTER_TYPE_INFO type{MFMediaType_Video, subtype};
        Require(OpenSynchronousTransform(MFT_CATEGORY_VIDEO_DECODER, &type, nullptr, decoder_),
            "No synchronous Media Foundation decoder for this video codec", CodecErrorCode::Unsupported);
      } else {
        Require(CoCreateInstance(audio ? CLSID_CMSAACDecMFT : CLSID_CMSH264DecoderMFT, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&decoder_)), "System AAC/AVC decoder is unavailable");
      }
      if (video && video->output_storage == VideoStorage::Platform) {
        Require(OpenVideoDevice(device_), "No compatible D3D11 video device", CodecErrorCode::Unsupported);
        Require(BindVideoDevice(*decoder_.Get(), *device_, device_manager_),
            "The Media Foundation decoder rejected D3D11 output", CodecErrorCode::Unsupported);
      }
      ComPtr<IMFMediaType> input;
      Require(MFCreateMediaType(&input), "Cannot allocate decoder input type");
      Require(input->SetGUID(MF_MT_MAJOR_TYPE, audio ? MFMediaType_Audio : MFMediaType_Video), "Cannot set input major type");
      Require(input->SetGUID(MF_MT_SUBTYPE, subtype), "Cannot set input subtype");
      if (audio) {
        ConfigureAudioInput(*input.Get(), *audio);
        if (audio->codec == CodecId::Flac) parameters_ = AudioDecoderHeader(*audio);
      } else {
        if (!vpx) {
          ComPtr<IMFAttributes> attributes;
          Require(decoder_->GetAttributes(&attributes), "Cannot configure video buffering");
          Require(attributes->SetUINT32(MF_LOW_LATENCY, TRUE), "Cannot bound video startup buffering");
        }
        Require(MFSetAttributeSize(input.Get(), MF_MT_FRAME_SIZE, video->width, video->height), "Cannot set video dimensions");
        if (hevc) {
          length_size_ = ParseHevcConfiguration(video->initialization_data, true, false).length;
          parameters_ = HevcAnnexBConfiguration(video->initialization_data);
          Require(input->SetUINT32(MF_MT_MPEG2_PROFILE, video->output == VideoPixelFormat::P010 ?
              eAVEncH265VProfile_Main_420_10 : eAVEncH265VProfile_Main_420_8), "Cannot set HEVC profile");
          Require(input->SetBlob(MF_MT_MPEG_SEQUENCE_HEADER, reinterpret_cast<const UINT8*>(parameters_.data()),
              static_cast<UINT32>(parameters_.size())), "Cannot set HEVC initialization data");
        } else if (!vpx) {
          const auto avc = ParseAvc(video->initialization_data).Value();
          length_size_ = avc.length_size;
          for (const auto& parameter : avc.parameters) {
            parameters_.insert(parameters_.end(), {std::byte{0}, std::byte{0}, std::byte{0}, std::byte{1}});
            parameters_.insert(parameters_.end(), parameter.begin(), parameter.end());
          }
        }
      }
      Require(decoder_->SetInputType(0, input.Get(), 0), "Decoder rejected input format");
      Negotiate();
      if (audio && audio->codec == CodecId::Mp3)
        mp3_.emplace(audio->output, AudioSampleLimit(*audio).Value());
      Require(decoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0), "Cannot begin decoder streaming");
      Require(decoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0), "Cannot start decoder stream");
    });
  }
  CodecResult<SubmitStatus> Submit(const EncodedPacket& packet) override {
    return Guard<SubmitStatus>([&] {
      auto bytes = packet.data.AsBytes();
      const auto* audio = std::get_if<AudioDecoderConfig>(&config_);
      unsigned audio_samples = 0;
      if (audio && !mp3_) {
        const auto count = AudioPacketSamples(*audio, bytes);
        if (!count.Succeeded()) throw MfFailure{count.Error()};
        audio_samples = count.Value();
      }
      Bytes formatted;
      if (length_size_) {
        auto converted = AnnexB(packet, length_size_);
        if (!converted.Succeeded()) throw MfFailure{converted.Error()};
        formatted = std::move(converted).Value();
        bytes = formatted;
      }
      if (first_ && !parameters_.empty()) {
        if (audio) formatted.assign(bytes.begin(), bytes.end());
        formatted.insert(formatted.begin(), parameters_.begin(), parameters_.end());
        bytes = formatted;
      }
      const auto pts = packet.pts.Rescale(PresentationTimescale());
      const auto duration = packet.duration ? packet.duration->Rescale(PresentationTimescale()) : std::optional<MediaTimestamp>{};
      if (!pts || (packet.duration && !duration)) throw MfFailure{Error(CodecErrorCode::InvalidData, "Timestamp exceeds Media Foundation range")};
      const auto status = Push(bytes, pts->ticks, duration ? std::optional{duration->ticks} : std::nullopt, packet.key_frame);
      if (status == SubmitStatus::WouldBlock) return status;
      if (mp3_) {
        if (mp3_drain_frame_.empty()) mp3_drain_frame_ = Mp3DrainFrame(bytes);
        mp3_->Expect(packet.pts);
        mp3_last_pts_ = pts->ticks;
      } else if (audio) {
        audio_timing_.push_back({pts->ticks, audio_samples});
      }
      first_ = false;
      return SubmitStatus::Accepted;
    });
  }
  CodecResult<std::optional<BackendOutput>> Poll() override {
    return Guard<std::optional<BackendOutput>>([&]() -> std::optional<BackendOutput> {
      if (eos_) return std::nullopt;
      if (mp3_) {
        if (auto frame = mp3_->Read()) return BackendOutput{DecodedFrame{std::move(*frame)}};
      }
      MFT_OUTPUT_STREAM_INFO info{};
      Require(decoder_->GetOutputStreamInfo(0, &info), "Cannot query decoder output requirements");
      ComPtr<IMFSample> sample;
      if (device_) {
        if (!(info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)))
          throw MfFailure{Error(CodecErrorCode::Unsupported, "The decoder cannot supply DXGI output samples")};
      } else if (!(info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES)) {
        const auto* audio = std::get_if<AudioDecoderConfig>(&config_);
        // The MFT can reserve scratch for its maximum PCM format independently of the negotiated stream.
        auto maximum = std::max<std::size_t>(ValidateConfig(config_).Value() * 4, 64 * 1024);
        if (audio && audio->codec == CodecId::Flac) maximum = 65535 * 8 * sizeof(float);
        if (audio && audio->codec == CodecId::Opus) maximum = 512 * 1024;
        if (info.cbSize > maximum) throw MfFailure{Error(CodecErrorCode::ResourceExhausted,
            "Media Foundation output allocation exceeds configured frame bound", std::to_string(info.cbSize) + "/" + std::to_string(maximum))};
        ComPtr<IMFMediaBuffer> buffer;
        Require(MFCreateAlignedMemoryBuffer(info.cbSize, info.cbAlignment ? info.cbAlignment - 1 : 0, &buffer), "Cannot allocate decoder output");
        Require(MFCreateSample(&sample), "Cannot allocate decoded sample");
        Require(sample->AddBuffer(buffer.Get()), "Cannot attach decoded storage");
      }
      MFT_OUTPUT_DATA_BUFFER output{0, sample.Get(), 0, nullptr};
      DWORD status = 0;
      const auto result = decoder_->ProcessOutput(0, 1, &output, &status);
      if (output.pEvents) output.pEvents->Release();
      if (!sample && output.pSample) sample.Attach(output.pSample);
      if (result == MF_E_TRANSFORM_STREAM_CHANGE) { Negotiate(); return std::nullopt; }
      if (result == MF_E_TRANSFORM_NEED_MORE_INPUT) {
        if (finished_) {
          if (mp3_ && !mp3_->Drained())
            throw MfFailure{Error(CodecErrorCode::InvalidData, "Media Foundation MP3 drain has missing samples")};
          eos_ = true;
          return BackendOutput{EndOfStream{}};
        }
        return std::nullopt;
      }
      Require(result, "Media Foundation decoding failed");
      if (!sample) throw MfFailure{Error(CodecErrorCode::InvalidData, "Decoder returned no output sample")};
      auto frame = ReadSample(sample.Get());
      if (frame) return BackendOutput{std::move(*frame)};
      return std::nullopt;
    });
  }
  CodecResult<SubmitStatus> Finish() override {
    return Guard<SubmitStatus>([&] {
      if (!mp3_drain_frame_.empty()) {
        const auto& audio = std::get<AudioDecoderConfig>(config_);
        const auto duration = MediaTimestamp{AudioSampleLimit(audio).Value(), audio.output.sample_rate}.Rescale(PresentationTimescale())->ticks;
        if (mp3_last_pts_ > INT64_MAX - duration)
          throw MfFailure{Error(CodecErrorCode::InvalidData, "MP3 drain timestamp overflow")};
        if (Push(mp3_drain_frame_, mp3_last_pts_ + duration, duration, true) == SubmitStatus::WouldBlock)
          return SubmitStatus::WouldBlock;
        mp3_drain_frame_.clear();
      }
      Require(decoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0), "Decoder rejected end-of-stream");
      Require(decoder_->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0), "Decoder drain failed");
      finished_ = true;
      return SubmitStatus::Accepted;
    });
  }
  CodecResult<void> Close() override {
    std::optional<CodecError> error;
    auto record = [&](HRESULT status, const char* operation) {
      if (FAILED(status)) error = Error(CodecErrorCode::PlatformFailure, operation, std::to_string(status));
    };
    if (decoder_) {
      record(decoder_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0), "Decoder flush failed during cleanup");
      record(decoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0), "Decoder end-streaming failed during cleanup");
      output_type_.Reset(); decoder_.Reset();
    }
    device_manager_.Reset();
    device_.reset();
    audio_timing_.clear();
    mp3_.reset();
    mp3_drain_frame_.clear();
    if (mf_) { record(MFShutdown(), "Media Foundation release failed"); mf_ = false; }
    if (com_) { CoUninitialize(); com_ = false; }
    return error ? CodecResult<void>::Failure(*error) : CodecResult<void>::Success();
  }
private:
  SubmitStatus Push(std::span<const std::byte> bytes, LONGLONG pts, std::optional<LONGLONG> duration, bool sync) {
    if (bytes.size() > MAXDWORD) throw MfFailure{Error(CodecErrorCode::InvalidData, "Packet exceeds Media Foundation buffer range")};
    ComPtr<IMFSample> sample;
    ComPtr<IMFMediaBuffer> buffer;
    Require(MFCreateSample(&sample), "Cannot allocate encoded sample");
    Require(MFCreateMemoryBuffer(static_cast<DWORD>(bytes.size()), &buffer), "Cannot allocate encoded bytes");
    BYTE* target = nullptr;
    Require(buffer->Lock(&target, nullptr, nullptr), "Cannot map encoded bytes");
    std::memcpy(target, bytes.data(), bytes.size());
    Require(buffer->Unlock(), "Cannot unmap encoded bytes");
    Require(buffer->SetCurrentLength(static_cast<DWORD>(bytes.size())), "Cannot set packet size");
    Require(sample->AddBuffer(buffer.Get()), "Cannot attach packet bytes");
    Require(sample->SetSampleTime(pts), "Cannot set packet PTS");
    if (duration) Require(sample->SetSampleDuration(*duration), "Cannot set packet duration");
    Require(sample->SetUINT32(MFSampleExtension_CleanPoint, sync), "Cannot set packet random-access flag");
    const auto status = decoder_->ProcessInput(0, sample.Get(), 0);
    if (status == MF_E_NOTACCEPTING) return SubmitStatus::WouldBlock;
    Require(status, "Decoder rejected access unit");
    return SubmitStatus::Accepted;
  }

  void NegotiateAudioOutput(const AudioDecoderConfig& audio) {
    ComPtr<IMFMediaType> selected;
    AudioSampleFormat selected_format = audio.output.sample_format;
    for (DWORD i = 0;; ++i) {
      ComPtr<IMFMediaType> type;
      const auto status = decoder_->GetOutputAvailableType(0, i, &type);
      if (status == MF_E_NO_MORE_TYPES) break;
      Require(status, "Cannot enumerate decoder PCM formats");
      GUID subtype{};
      Require(type->GetGUID(MF_MT_SUBTYPE, &subtype), "PCM subtype is missing");
      const auto bits = MFGetAttributeUINT32(type.Get(), MF_MT_AUDIO_BITS_PER_SAMPLE, 0);
      if ((subtype != MFAudioFormat_PCM || bits != 16) && (subtype != MFAudioFormat_Float || bits != 32)) continue;
      const auto format = subtype == MFAudioFormat_Float ? AudioSampleFormat::F32 : AudioSampleFormat::S16;
      if (MFGetAttributeUINT32(type.Get(), MF_MT_AUDIO_SAMPLES_PER_SECOND, 0) != audio.output.sample_rate ||
          MFGetAttributeUINT32(type.Get(), MF_MT_AUDIO_NUM_CHANNELS, 0) != audio.output.ChannelCount()) continue;
      if (format == audio.output.sample_format) {
        selected = std::move(type);
        selected_format = format;
        break;
      }
      if (!selected && audio.codec != CodecId::Aac) {
        selected = std::move(type);
        selected_format = format;
      }
    }
    if (!selected) throw MfFailure{Error(CodecErrorCode::Unsupported, "No compatible Media Foundation PCM output format")};
    Require(decoder_->SetOutputType(0, selected.Get(), 0), "Decoder rejected PCM output");
    output_type_ = std::move(selected);
    audio_sample_format_ = selected_format;
  }

  void Negotiate() {
    if (const auto* audio = std::get_if<AudioDecoderConfig>(&config_)) {
      NegotiateAudioOutput(*audio);
      return;
    }
    const auto& config = std::get<VideoDecoderConfig>(config_);
    const GUID wanted = config.output == VideoPixelFormat::P010 ? MFVideoFormat_P010 : MFVideoFormat_NV12;
    for (DWORD i = 0;; ++i) {
      ComPtr<IMFMediaType> type;
      const auto status = decoder_->GetOutputAvailableType(0, i, &type);
      if (status == MF_E_NO_MORE_TYPES) break;
      Require(status, "Cannot enumerate decoder output formats");
      GUID subtype{};
      Require(type->GetGUID(MF_MT_SUBTYPE, &subtype), "Output subtype is missing");
      if (subtype != wanted) continue;
      UINT32 width, height;
      Require(MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &width, &height), "Video size is missing");
      if (!width || !height || width > config.width || height > config.height) continue;
      Require(decoder_->SetOutputType(0, type.Get(), 0), "Decoder rejected output format");
      output_type_ = std::move(type);
      return;
    }
    throw MfFailure{Error(CodecErrorCode::Unsupported, "No compatible Media Foundation output format")};
  }
  std::optional<DecodedFrame> ReadSample(IMFSample* sample) {
    LONGLONG time;
    Require(sample->GetSampleTime(&time), "Decoded sample has no PTS");
    const MediaTimestamp pts{time, PresentationTimescale()};
    if (device_) {
      LONGLONG duration = 0;
      const auto timed = SUCCEEDED(sample->GetSampleDuration(&duration)) ?
          std::optional(MediaTimestamp{duration, PresentationTimescale()}) : std::nullopt;
      auto image = CopyVideoSample(*sample, device_, VideoOutputFormat(), pts, timed);
      if (!image.Succeeded()) throw MfFailure{image.Error()};
      return DecodedFrame{std::move(image).Value()};
    }
    ComPtr<IMFMediaBuffer> buffer;
    Require(sample->ConvertToContiguousBuffer(&buffer), "Cannot access decoded CPU bytes");
    DWORD size;
    Require(buffer->GetCurrentLength(&size), "Cannot query decoded buffer size");
    if (size > std::max<std::size_t>(ValidateConfig(config_).Value() * 4, 64 * 1024))
      throw MfFailure{Error(CodecErrorCode::ResourceExhausted, "Media Foundation CPU output exceeds the bounded copy capacity")};
    Bytes bytes(size);
    BYTE* source = nullptr;
    Require(buffer->Lock(&source, nullptr, nullptr), "Cannot map decoded CPU bytes");
    std::memcpy(bytes.data(), source, size);
    Require(buffer->Unlock(), "Cannot unmap decoded CPU bytes");
    const auto data = Own(std::move(bytes));
    if (const auto* audio = std::get_if<AudioDecoderConfig>(&config_)) {
      auto format = audio->output; format.packing = AudioPacking::Interleaved;
      format.sample_format = audio_sample_format_;
      const auto stride = format.BytesPerSample() * format.ChannelCount();
      if (mp3_) {
        if (!size || size % stride)
          throw MfFailure{Error(CodecErrorCode::InvalidData, "Unexpected Media Foundation MP3 PCM alignment")};
        auto pcm = CopyAudio({format, pts, size / stride, {data}}, audio->output);
        if (!pcm.Succeeded()) throw MfFailure{pcm.Error()};
        const auto buffered = mp3_->Append(pcm.Value(), finished_);
        if (!buffered.Succeeded()) throw MfFailure{buffered.Error()};
        if (auto frame = mp3_->Read()) return DecodedFrame{std::move(*frame)};
        return std::nullopt;
      }
      if (audio_timing_.empty() || size % stride || size / stride != audio_timing_.front().samples)
        throw MfFailure{Error(CodecErrorCode::InvalidData, "Unexpected Media Foundation PCM sample count",
            std::to_string(size / stride) + "/" + (audio_timing_.empty() ? "none" : std::to_string(audio_timing_.front().samples)))};
      const auto expected = audio_timing_.front().pts;
      // The FLAC MFT can stamp a buffered block with the input that made it available.
      // Its output is in frame order and the sample count identifies the submitted block boundary.
      if (audio->codec != CodecId::Flac && time != expected &&
          !(time < INT64_MAX && time + 1 == expected) && !(time > INT64_MIN && time - 1 == expected))
        throw MfFailure{Error(CodecErrorCode::InvalidData, "PCM timing does not match the accepted access unit",
            std::to_string(time) + "/" + std::to_string(expected))};
      auto result = CopyAudio({format, {expected, PresentationTimescale()}, size / stride, {data}}, audio->output);
      if (!result.Succeeded()) throw MfFailure{result.Error()};
      audio_timing_.pop_front();
      return DecodedFrame{std::move(result).Value()};
    }
    const auto format = VideoOutputFormat();
    const auto output = std::get<VideoDecoderConfig>(config_).output;
    const auto component = output == VideoPixelFormat::P010 ? 2u : 1u;
    const auto row = (format.width + 1) / 2 * 2 * component;
    const auto raw_stride = MFGetAttributeUINT32(output_type_.Get(), MF_MT_DEFAULT_STRIDE, row);
    const auto stride = static_cast<LONG>(raw_stride);
    if (stride < static_cast<LONG>(row)) throw MfFailure{Error(CodecErrorCode::Unsupported, "Unsupported CPU YUV stride")};
    const auto ysize = std::size_t(stride) * format.height;
    const auto csize = std::size_t(stride) * ((format.height + 1) / 2);
    if (ysize > size || csize > size - ysize) throw MfFailure{Error(CodecErrorCode::InvalidData, "YUV planes exceed output bytes")};
    LONGLONG duration;
    const auto timed = SUCCEEDED(sample->GetSampleDuration(&duration)) ? std::optional(MediaTimestamp{duration, PresentationTimescale()}) : std::nullopt;
    auto result = CopyVideo(format, pts, timed, {{data.Slice(0, ysize), std::size_t(stride), component},
        {data.Slice(ysize, csize), std::size_t(stride), component * 2}}, output);
    if (!result.Succeeded()) throw MfFailure{result.Error()};
    return DecodedFrame{std::move(result).Value()};
  }
  VideoFormat VideoOutputFormat() const {
    VideoFormat format;
    Require(MFGetAttributeSize(output_type_.Get(), MF_MT_FRAME_SIZE, &format.width, &format.height), "Decoded geometry is missing");
    format.visible = {0, 0, format.width, format.height};
    const auto layout = std::get<VideoDecoderConfig>(config_).output;
    format.pixel_format = layout == VideoPixelFormat::P010 ? VideoPixelFormat::P010 : VideoPixelFormat::NV12;
    UINT32 aspect_width, aspect_height;
    if (SUCCEEDED(MFGetAttributeRatio(output_type_.Get(), MF_MT_PIXEL_ASPECT_RATIO, &aspect_width, &aspect_height))) {
      format.aspect_width = aspect_width; format.aspect_height = aspect_height;
    }
    MFVideoArea area{};
    UINT32 area_size = 0;
    if (SUCCEEDED(output_type_->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, reinterpret_cast<UINT8*>(&area), sizeof(area), &area_size)) && area_size == sizeof(area)) {
      if (area.OffsetX.fract || area.OffsetY.fract || area.OffsetX.value < 0 || area.OffsetY.value < 0 || area.Area.cx <= 0 || area.Area.cy <= 0)
        throw MfFailure{Error(CodecErrorCode::Unsupported, "Unsupported fractional video aperture")};
      format.visible = {UINT32(area.OffsetX.value), UINT32(area.OffsetY.value), UINT32(area.Area.cx), UINT32(area.Area.cy)};
    }
    ReadMediaFoundationVideoColor(output_type_.Get(), format);
    return format;
  }
  DecoderConfig config_;
  ComPtr<IMFTransform> decoder_;
  ComPtr<IMFMediaType> output_type_;
  std::shared_ptr<WindowsVideoDevice> device_;
  ComPtr<IMFDXGIDeviceManager> device_manager_;
  Bytes parameters_;
  struct AudioTiming { LONGLONG pts; unsigned samples; };
  std::deque<AudioTiming> audio_timing_;
  std::optional<PcmPackets> mp3_;
  Bytes mp3_drain_frame_;
  LONGLONG mp3_last_pts_ = 0;
  AudioSampleFormat audio_sample_format_ = AudioSampleFormat::S16;
  unsigned length_size_ = 0;
  bool com_ = false, mf_ = false, first_ = true, finished_ = false, eos_ = false;
};
} // namespace
std::unique_ptr<DecoderBackend> MakeDecoderBackend() { return std::make_unique<WindowsDecoder>(); }
} // namespace huxerui::media::detail
