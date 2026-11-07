#include "codec/encoder/backend.h"

#include <cstring>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wmcodecdsp.h>
#include <wrl/client.h>

namespace huxerui::media::detail {
namespace {
using Microsoft::WRL::ComPtr;
struct EncoderFailure { CodecError error; };
void Require(HRESULT status, const char* message, CodecErrorCode code = CodecErrorCode::PlatformFailure) {
  if (FAILED(status)) throw EncoderFailure{Error(code, message, std::to_string(status))};
}
template<class T, class F> CodecResult<T> Guard(F work) {
  try {
    if constexpr (std::is_void_v<T>) { work(); return CodecResult<void>::Success(); }
    else return CodecResult<T>::Success(work());
  } catch (const EncoderFailure& failure) { return CodecResult<T>::Failure(failure.error); }
}
class WindowsAudioEncoder final : public AudioEncoderBackend {
public:
  CodecResult<void> Open(const AudioEncoderConfig& config) override {
    return Guard<void>([&] {
      config_ = config;
      config_.wakeup = {}; config_.cleanup_completed = {};
      if (config.bitrate != 96000 && config.bitrate != 128000 && config.bitrate != 160000 && config.bitrate != 192000)
        throw EncoderFailure{Error(CodecErrorCode::Unsupported, "System AAC encoder does not support this bitrate")};
      Require(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "Cannot initialize encoder COM"); com_ = true;
      Require(MFStartup(MF_VERSION, MFSTARTUP_FULL), "Cannot initialize encoder Media Foundation"); mf_ = true;
      Require(CoCreateInstance(CLSID_AACMFTEncoder, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&encoder_)),
          "System AAC encoder is unavailable", CodecErrorCode::Unsupported);
      ComPtr<IMFMediaType> input, output;
      Require(MFCreateMediaType(&input), "Cannot allocate PCM input type");
      Require(MFCreateMediaType(&output), "Cannot allocate AAC output type");
      for (auto* type : {input.Get(), output.Get()}) {
        Require(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio), "Cannot set encoder media type");
        Require(type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, config.input.sample_rate), "Cannot set encoder rate");
        Require(type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, config.input.ChannelCount()), "Cannot set encoder channels");
        Require(type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16), "Cannot set encoder precision");
      }
      Require(input->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM), "Cannot select PCM input");
      Require(input->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, config.input.ChannelCount() * 2), "Cannot set PCM stride");
      Require(input->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, config.input.sample_rate * config.input.ChannelCount() * 2), "Cannot set PCM byte rate");
      Require(output->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC), "Cannot select AAC output");
      Require(output->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, config.bitrate / 8), "Cannot set AAC bitrate");
      Require(output->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0), "Cannot select raw AAC output");
      Require(output->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0x29), "Cannot select AAC-LC profile");
      Require(encoder_->SetInputType(0, input.Get(), 0), "Encoder rejected PCM input", CodecErrorCode::Unsupported);
      Require(encoder_->SetOutputType(0, output.Get(), 0), "Encoder rejected AAC output", CodecErrorCode::Unsupported);
      ComPtr<IMFMediaType> actual;
      Require(encoder_->GetOutputCurrentType(0, &actual), "Cannot query AAC output type");
      UINT32 size = 0;
      Require(actual->GetBlobSize(MF_MT_USER_DATA, &size), "AAC encoder did not provide initialization data");
      if (size != 14) throw EncoderFailure{Error(CodecErrorCode::Unsupported, "Unexpected AAC encoder initialization data")};
      UINT8 data[14]{};
      Require(actual->GetBlob(MF_MT_USER_DATA, data, sizeof(data), &size), "Cannot read AAC initialization data");
      format_ = {config.input.sample_rate, config.input.channel_layout, {std::byte(data[12]), std::byte(data[13])}};
      auto valid = ValidateAudioEncodeFormat(format_, config);
      if (!valid.Succeeded()) throw EncoderFailure{valid.Error()};
      Require(encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0), "Cannot begin AAC encoding");
      Require(encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0), "Cannot start AAC encoding");
    });
  }
  CodecResult<SubmitStatus> Submit(std::span<const std::byte> pcm, std::uint32_t samples, std::int64_t first_sample) override {
    return Guard<SubmitStatus>([&] {
      const auto pts = MediaTimestamp{first_sample, config_.input.sample_rate}.Rescale(10000000);
      const auto duration = MediaTimestamp{samples, config_.input.sample_rate}.Rescale(10000000);
      if (!pts || !duration || duration->ticks <= 0)
        throw EncoderFailure{Error(CodecErrorCode::InvalidData, "PCM timing exceeds Media Foundation range")};
      ComPtr<IMFSample> sample;
      ComPtr<IMFMediaBuffer> buffer;
      Require(MFCreateSample(&sample), "Cannot allocate PCM sample");
      Require(MFCreateMemoryBuffer(static_cast<DWORD>(pcm.size()), &buffer), "Cannot allocate PCM input");
      BYTE* target = nullptr;
      Require(buffer->Lock(&target, nullptr, nullptr), "Cannot map PCM input");
      std::memcpy(target, pcm.data(), pcm.size());
      Require(buffer->Unlock(), "Cannot unmap PCM input");
      Require(buffer->SetCurrentLength(static_cast<DWORD>(pcm.size())), "Cannot set PCM input size");
      Require(sample->AddBuffer(buffer.Get()), "Cannot attach PCM input");
      Require(sample->SetSampleTime(pts->ticks), "Cannot set PCM timestamp");
      Require(sample->SetSampleDuration(duration->ticks), "Cannot set PCM duration");
      const auto result = encoder_->ProcessInput(0, sample.Get(), 0);
      if (result == MF_E_NOTACCEPTING) return SubmitStatus::WouldBlock;
      Require(result, "AAC encoder rejected PCM input");
      return SubmitStatus::Accepted;
    });
  }
  CodecResult<std::optional<AudioEncoderBackendOutput>> Poll() override {
    return Guard<std::optional<AudioEncoderBackendOutput>>([&]() -> std::optional<AudioEncoderBackendOutput> {
      if (eos_) return std::nullopt;
      if (!described_) { described_ = true; return format_; }
      MFT_OUTPUT_STREAM_INFO info{};
      Require(encoder_->GetOutputStreamInfo(0, &info), "Cannot query AAC output storage");
      if (info.cbSize > kAudioEncoderPacketBytes)
        throw EncoderFailure{Error(CodecErrorCode::ResourceExhausted, "AAC output scratch exceeds 64 KiB")};
      ComPtr<IMFSample> sample;
      if (!(info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES)) {
        ComPtr<IMFMediaBuffer> buffer;
        Require(MFCreateSample(&sample), "Cannot allocate AAC sample");
        Require(MFCreateAlignedMemoryBuffer(info.cbSize, info.cbAlignment ? info.cbAlignment - 1 : 0, &buffer), "Cannot allocate AAC output");
        Require(sample->AddBuffer(buffer.Get()), "Cannot attach AAC output");
      }
      MFT_OUTPUT_DATA_BUFFER output{0, sample.Get(), 0, nullptr};
      DWORD status = 0;
      const auto result = encoder_->ProcessOutput(0, 1, &output, &status);
      if (output.pEvents) output.pEvents->Release();
      if (!sample && output.pSample) sample.Attach(output.pSample);
      if (result == MF_E_TRANSFORM_NEED_MORE_INPUT) {
        if (finished_) { eos_ = true; return AudioEncoderEnd{}; }
        return std::nullopt;
      }
      Require(result, "AAC encoding failed");
      if (!sample) throw EncoderFailure{Error(CodecErrorCode::InvalidData, "AAC encoder returned no sample")};
      LONGLONG pts;
      Require(sample->GetSampleTime(&pts), "AAC output timestamp is missing");
      ComPtr<IMFMediaBuffer> buffer;
      Require(sample->ConvertToContiguousBuffer(&buffer), "Cannot access AAC output");
      DWORD size = 0;
      Require(buffer->GetCurrentLength(&size), "Cannot query AAC packet size");
      if (!size || size > kAudioEncoderPacketBytes)
        throw EncoderFailure{Error(CodecErrorCode::ResourceExhausted, "AAC packet exceeds 64 KiB")};
      Bytes bytes(size);
      BYTE* source = nullptr;
      Require(buffer->Lock(&source, nullptr, nullptr), "Cannot map AAC packet");
      std::memcpy(bytes.data(), source, size);
      Require(buffer->Unlock(), "Cannot unmap AAC packet");
      EncodedPacket packet{Own(std::move(bytes)), {pts, 10000000}, std::nullopt, std::nullopt, true};
      UINT64 dts;
      if (SUCCEEDED(sample->GetUINT64(MFSampleExtension_DecodeTimestamp, &dts))) packet.dts = MediaTimestamp{static_cast<std::int64_t>(dts), 10000000};
      return packet;
    });
  }
  CodecResult<SubmitStatus> Finish() override {
    return Guard<SubmitStatus>([&] {
      Require(encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0), "AAC encoder rejected EOS");
      Require(encoder_->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0), "AAC encoder drain failed");
      finished_ = true;
      return SubmitStatus::Accepted;
    });
  }
  CodecResult<void> Close() override {
    std::optional<CodecError> error;
    if (encoder_) {
      for (auto message : {MFT_MESSAGE_COMMAND_FLUSH, MFT_MESSAGE_NOTIFY_END_STREAMING}) {
        const auto result = encoder_->ProcessMessage(message, 0);
        if (FAILED(result)) error = Error(CodecErrorCode::PlatformFailure, "AAC encoder teardown failed", std::to_string(result));
      }
      encoder_.Reset();
    }
    if (mf_) {
      const auto result = MFShutdown(); mf_ = false;
      if (FAILED(result)) error = Error(CodecErrorCode::PlatformFailure, "Encoder MFShutdown failed", std::to_string(result));
    }
    if (com_) { CoUninitialize(); com_ = false; }
    return error ? CodecResult<void>::Failure(*error) : CodecResult<void>::Success();
  }
private:
  AudioEncoderConfig config_;
  AudioEncodeFormat format_;
  ComPtr<IMFTransform> encoder_;
  bool com_ = false, mf_ = false, described_ = false, finished_ = false, eos_ = false;
};
} // namespace
std::unique_ptr<AudioEncoderBackend> MakeAudioEncoderBackend() { return std::make_unique<WindowsAudioEncoder>(); }
} // namespace huxerui::media::detail
