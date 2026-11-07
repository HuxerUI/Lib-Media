#include "codec/decoder/backend.h"
#include "codec/decoder/copy.h"

#include <algorithm>
#include <cstring>
#include <deque>

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
void Require(HRESULT status, const char* operation) {
  if (FAILED(status)) throw MfFailure{Error(CodecErrorCode::PlatformFailure, operation, std::to_string(status))};
}
template<class T, class Function> CodecResult<T> Guard(Function function) {
  try {
    if constexpr (std::is_void_v<T>) { function(); return CodecResult<void>::Success(); }
    else return CodecResult<T>::Success(function());
  } catch (const MfFailure& error) { return CodecResult<T>::Failure(error.error); }
}
class WindowsDecoder final : public DecoderBackend {
public:
  std::uint32_t PresentationTimescale() const noexcept override { return 10000000; }

  CodecResult<void> Open(const DecoderConfig& config) override {
    return Guard<void>([&] {
      config_ = config;
      Require(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "COM initialization failed"); com_ = true;
      Require(MFStartup(MF_VERSION, MFSTARTUP_FULL), "Media Foundation initialization failed"); mf_ = true;
      const auto* audio = std::get_if<AudioDecoderConfig>(&config_);
      Require(CoCreateInstance(audio ? CLSID_CMSAACDecMFT : CLSID_CMSH264DecoderMFT, nullptr, CLSCTX_INPROC_SERVER,
          IID_PPV_ARGS(&decoder_)), "System AAC/AVC decoder is unavailable");
      ComPtr<IMFMediaType> input;
      Require(MFCreateMediaType(&input), "Cannot allocate decoder input type");
      Require(input->SetGUID(MF_MT_MAJOR_TYPE, audio ? MFMediaType_Audio : MFMediaType_Video), "Cannot set input major type");
      Require(input->SetGUID(MF_MT_SUBTYPE, audio ? MFAudioFormat_AAC : MFVideoFormat_H264), "Cannot set input subtype");
      if (audio) {
        Require(input->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, audio->output.ChannelCount()), "Cannot set AAC channels");
        Require(input->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, audio->output.sample_rate), "Cannot set AAC rate");
        Require(input->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, audio->output.BytesPerSample() * 8), "Cannot set AAC output precision");
        Require(input->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0), "Cannot configure raw AAC");
        std::vector<UINT8> cookie(12, 0);
        cookie[2] = 0xfe;
        for (auto byte : audio->initialization_data) cookie.push_back(std::to_integer<UINT8>(byte));
        Require(input->SetBlob(MF_MT_USER_DATA, cookie.data(), static_cast<UINT32>(cookie.size())), "Cannot set AAC initialization data");
      } else {
        const auto& video = std::get<VideoDecoderConfig>(config_);
        ComPtr<IMFAttributes> attributes;
        Require(decoder_->GetAttributes(&attributes), "Cannot configure AVC buffering");
        Require(attributes->SetUINT32(MF_LOW_LATENCY, TRUE), "Cannot bound AVC startup buffering");
        Require(MFSetAttributeSize(input.Get(), MF_MT_FRAME_SIZE, video.width, video.height), "Cannot set AVC dimensions");
        const auto avc = ParseAvc(video.initialization_data).Value();
        length_size_ = avc.length_size;
        for (const auto& parameter : avc.parameters) {
          parameters_.insert(parameters_.end(), {std::byte{0}, std::byte{0}, std::byte{0}, std::byte{1}});
          parameters_.insert(parameters_.end(), parameter.begin(), parameter.end());
        }
      }
      Require(decoder_->SetInputType(0, input.Get(), 0), "Decoder rejected input format");
      Negotiate();
      Require(decoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0), "Cannot begin decoder streaming");
      Require(decoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0), "Cannot start decoder stream");
    });
  }
  CodecResult<SubmitStatus> Submit(const EncodedPacket& packet) override {
    return Guard<SubmitStatus>([&] {
      auto bytes = packet.data.AsBytes();
      Bytes annex;
      if (std::holds_alternative<VideoDecoderConfig>(config_)) {
        auto converted = AnnexB(packet, length_size_);
        if (!converted.Succeeded()) throw MfFailure{converted.Error()};
        annex = std::move(converted).Value();
        if (first_) annex.insert(annex.begin(), parameters_.begin(), parameters_.end());
        bytes = annex;
      }
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
      const auto pts = packet.pts.Rescale(PresentationTimescale());
      const auto duration = packet.duration ? packet.duration->Rescale(PresentationTimescale()) : std::optional<MediaTimestamp>{};
      if (!pts || (packet.duration && !duration)) throw MfFailure{Error(CodecErrorCode::InvalidData, "Timestamp exceeds Media Foundation range")};
      Require(sample->SetSampleTime(pts->ticks), "Cannot set packet PTS");
      if (duration) Require(sample->SetSampleDuration(duration->ticks), "Cannot set packet duration");
      Require(sample->SetUINT32(MFSampleExtension_CleanPoint, packet.key_frame), "Cannot set packet random-access flag");
      const auto status = decoder_->ProcessInput(0, sample.Get(), 0);
      if (status == MF_E_NOTACCEPTING) return SubmitStatus::WouldBlock;
      Require(status, "Decoder rejected access unit");
      if (std::holds_alternative<AudioDecoderConfig>(config_)) audio_timing_.push_back(pts->ticks);
      first_ = false;
      return SubmitStatus::Accepted;
    });
  }
  CodecResult<std::optional<BackendOutput>> Poll() override {
    return Guard<std::optional<BackendOutput>>([&]() -> std::optional<BackendOutput> {
      if (eos_) return std::nullopt;
      MFT_OUTPUT_STREAM_INFO info{};
      Require(decoder_->GetOutputStreamInfo(0, &info), "Cannot query decoder output requirements");
      ComPtr<IMFSample> sample;
      if (!(info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES)) {
        // The system AAC MFT requests 48 KiB even for one mono/stereo AAC-LC frame.
        const auto maximum = std::max<std::size_t>(ValidateConfig(config_).Value() * 4, 64 * 1024);
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
        if (finished_) { eos_ = true; return BackendOutput{EndOfStream{}}; }
        return std::nullopt;
      }
      Require(result, "Media Foundation decoding failed");
      if (!sample) throw MfFailure{Error(CodecErrorCode::InvalidData, "Decoder returned no output sample")};
      return BackendOutput{Copy(sample.Get())};
    });
  }
  CodecResult<SubmitStatus> Finish() override {
    return Guard<SubmitStatus>([&] {
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
    audio_timing_.clear();
    if (mf_) { record(MFShutdown(), "Media Foundation release failed"); mf_ = false; }
    if (com_) { CoUninitialize(); com_ = false; }
    return error ? CodecResult<void>::Failure(*error) : CodecResult<void>::Success();
  }
private:
  void Negotiate() {
    const auto* audio = std::get_if<AudioDecoderConfig>(&config_);
    const GUID wanted = audio ? (audio->output.sample_format == AudioSampleFormat::F32 ? MFAudioFormat_Float : MFAudioFormat_PCM) : MFVideoFormat_NV12;
    for (DWORD i = 0;; ++i) {
      ComPtr<IMFMediaType> type;
      const auto status = decoder_->GetOutputAvailableType(0, i, &type);
      if (status == MF_E_NO_MORE_TYPES) break;
      Require(status, "Cannot enumerate decoder output formats");
      GUID subtype{};
      Require(type->GetGUID(MF_MT_SUBTYPE, &subtype), "Output subtype is missing");
      if (subtype != wanted) continue;
      if (audio) {
        UINT32 rate, channels;
        Require(type->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate), "PCM rate is missing");
        Require(type->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels), "PCM channel count is missing");
        if (rate != audio->output.sample_rate || channels != audio->output.ChannelCount()) continue;
      } else {
        UINT32 width, height;
        Require(MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &width, &height), "Video size is missing");
        const auto& config = std::get<VideoDecoderConfig>(config_);
        if (!width || !height || width > config.width || height > config.height) continue;
      }
      Require(decoder_->SetOutputType(0, type.Get(), 0), "Decoder rejected CPU output format");
      output_type_ = std::move(type);
      return;
    }
    throw MfFailure{Error(CodecErrorCode::Unsupported, "No compatible CPU PCM/NV12 output format")};
  }
  DecodedFrame Copy(IMFSample* sample) {
    LONGLONG time;
    Require(sample->GetSampleTime(&time), "Decoded sample has no PTS");
    const MediaTimestamp pts{time, PresentationTimescale()};
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
      const auto stride = format.BytesPerSample() * format.ChannelCount();
      if (size % stride || size / stride != 1024 || audio_timing_.empty())
        throw MfFailure{Error(CodecErrorCode::InvalidData, "Unexpected AAC output count")};
      const auto expected = audio_timing_.front();
      // The AAC MFT can truncate a drained timestamp by one 100 ns tick.
      if (time != expected && !(time < INT64_MAX && time + 1 == expected) && !(time > INT64_MIN && time - 1 == expected))
        throw MfFailure{Error(CodecErrorCode::InvalidData, "AAC output timing does not match the accepted access unit")};
      auto result = CopyAudio({format, {expected, PresentationTimescale()}, size / stride, {data}}, audio->output);
      if (!result.Succeeded()) throw MfFailure{result.Error()};
      audio_timing_.pop_front();
      return DecodedFrame{std::move(result).Value()};
    }
    VideoFormat format;
    Require(MFGetAttributeSize(output_type_.Get(), MF_MT_FRAME_SIZE, &format.width, &format.height), "Decoded geometry is missing");
    format.visible = {0, 0, format.width, format.height};
    const auto raw_stride = MFGetAttributeUINT32(output_type_.Get(), MF_MT_DEFAULT_STRIDE, format.width);
    const auto stride = static_cast<LONG>(raw_stride);
    if (stride < static_cast<LONG>(format.width)) throw MfFailure{Error(CodecErrorCode::Unsupported, "Unsupported NV12 stride")};
    const auto ysize = std::size_t(stride) * format.height;
    const auto csize = std::size_t(stride) * ((format.height + 1) / 2);
    if (ysize > size || csize > size - ysize) throw MfFailure{Error(CodecErrorCode::InvalidData, "NV12 planes exceed output bytes")};
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
    const auto primaries = MFGetAttributeUINT32(output_type_.Get(), MF_MT_VIDEO_PRIMARIES, 0);
    const auto matrix = MFGetAttributeUINT32(output_type_.Get(), MF_MT_YUV_MATRIX, 0);
    const auto range = MFGetAttributeUINT32(output_type_.Get(), MF_MT_VIDEO_NOMINAL_RANGE, 0);
    if (primaries == MFVideoPrimaries_BT709) format.primaries = ColorPrimaries::BT709;
    if (matrix == MFVideoTransferMatrix_BT709) format.matrix = ColorMatrix::BT709;
    else if (matrix == MFVideoTransferMatrix_BT601) format.matrix = ColorMatrix::BT601;
    if (range == MFNominalRange_0_255) format.range = ColorRange::Full;
    else if (range == MFNominalRange_16_235) format.range = ColorRange::Limited;
    LONGLONG duration;
    const auto timed = SUCCEEDED(sample->GetSampleDuration(&duration)) ? std::optional(MediaTimestamp{duration, PresentationTimescale()}) : std::nullopt;
    auto result = CopyVideo(format, pts, timed, {{data.Slice(0, ysize), std::size_t(stride), 1},
        {data.Slice(ysize, csize), std::size_t(stride), 2}}, std::get<VideoDecoderConfig>(config_).output);
    if (!result.Succeeded()) throw MfFailure{result.Error()};
    return DecodedFrame{std::move(result).Value()};
  }
  DecoderConfig config_;
  ComPtr<IMFTransform> decoder_;
  ComPtr<IMFMediaType> output_type_;
  Bytes parameters_;
  std::deque<LONGLONG> audio_timing_;
  unsigned length_size_ = 4;
  bool com_ = false, mf_ = false, first_ = true, finished_ = false, eos_ = false;
};
} // namespace
std::unique_ptr<DecoderBackend> MakeDecoderBackend() { return std::make_unique<WindowsDecoder>(); }
} // namespace huxerui::media::detail
