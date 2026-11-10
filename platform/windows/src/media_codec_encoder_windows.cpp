#include "codec/encoder/backend.h"
#include "media_codec_transform_windows.h"

#include <algorithm>
#include <codecapi.h>
#include <map>
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

namespace {
class WindowsVideoEncoder final : public VideoEncoderBackend {
public:
  CodecResult<void> Open(const VideoEncoderConfig& config) override {
    return Guard<void>([&] {
      config_ = config; config_.wakeup = {}; config_.cleanup_completed = {};
      const bool hevc = config.codec == CodecId::Hevc;
      const bool ten_bit = config.input.pixel_format == VideoPixelFormat::P010;
      const auto bframes = VideoEncoderBFrames(config);
      if (!hevc && bframes > 2)
        throw EncoderFailure{Error(CodecErrorCode::Unsupported, "Media Foundation H.264 supports at most two consecutive B pictures")};
      Require(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "Cannot initialize video encoder COM"); com_ = true;
      Require(MFStartup(MF_VERSION, MFSTARTUP_FULL), "Cannot initialize video encoder Media Foundation"); mf_ = true;
      if (hevc) {
        const MFT_REGISTER_TYPE_INFO type{MFMediaType_Video, MFVideoFormat_HEVC};
        auto status = OpenSynchronousTransform(MFT_CATEGORY_VIDEO_ENCODER, nullptr, &type, encoder_);
        if (FAILED(status)) {
          const MFT_REGISTER_TYPE_INFO alternate{MFMediaType_Video, MFVideoFormat_H265};
          status = OpenSynchronousTransform(MFT_CATEGORY_VIDEO_ENCODER, nullptr, &alternate, encoder_);
        }
        Require(status, "No synchronous Media Foundation HEVC encoder", CodecErrorCode::Unsupported);
      } else {
        Require(CoCreateInstance(CLSID_CMSH264EncoderMFT, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&encoder_)),
            "Media Foundation H.264 encoder is unavailable", CodecErrorCode::Unsupported);
      }
      Require(encoder_.As(&controls_), "Video encoder has no control interface", CodecErrorCode::Unsupported);
      if (!hevc || controls_->IsSupported(&CODECAPI_AVEncMPVDefaultBPictureCount) == S_OK) {
        Set(CODECAPI_AVEncMPVDefaultBPictureCount, bframes);
      } else if (config.max_b_frames.value_or(0)) {
        throw EncoderFailure{Error(CodecErrorCode::Unsupported, "HEVC encoder cannot enforce the requested B-picture bound")};
      }
      Set(CODECAPI_AVEncMPVGOPSize, config.key_frame_interval);
      if (!hevc) Set(CODECAPI_AVEncNumWorkerThreads, 1);
      VARIANT latency{}; latency.vt = VT_BOOL;
      latency.boolVal = (hevc ? !config.max_b_frames || bframes : bframes != 0) ? VARIANT_FALSE : VARIANT_TRUE;
      Require(controls_->SetValue(&CODECAPI_AVLowLatencyMode, &latency), "Cannot configure video latency", CodecErrorCode::Unsupported);
      ComPtr<IMFMediaType> output, input;
      Require(MFCreateMediaType(&output), "Cannot create video output type");
      Require(MFCreateMediaType(&input), "Cannot create video input type");
      for (auto* type : {output.Get(), input.Get()}) {
        Require(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video), "Cannot select video media type");
        Require(MFSetAttributeSize(type, MF_MT_FRAME_SIZE, config.input.width, config.input.height), "Cannot set video dimensions");
        Require(MFSetAttributeRatio(type, MF_MT_FRAME_RATE, config.frame_rate_numerator, config.frame_rate_denominator), "Cannot set video frame rate");
        Require(MFSetAttributeRatio(type, MF_MT_PIXEL_ASPECT_RATIO, 1, 1), "Cannot set video pixel aspect ratio");
        Require(type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive), "Cannot select progressive video");
        SetColor(type, config.input);
      }
      Require(output->SetGUID(MF_MT_SUBTYPE, hevc ? MFVideoFormat_HEVC : MFVideoFormat_H264), "Cannot select encoded output");
      Require(output->SetUINT32(MF_MT_AVG_BITRATE, config.bitrate), "Cannot set video bitrate");
      const UINT32 profile = hevc ? UINT32(ten_bit ? eAVEncH265VProfile_Main_420_10 : eAVEncH265VProfile_Main_420_8) :
          config.profile == H264Profile::Baseline ? eAVEncH264VProfile_Base :
          config.profile == H264Profile::Main ? eAVEncH264VProfile_Main : eAVEncH264VProfile_High;
      Require(output->SetUINT32(MF_MT_MPEG2_PROFILE, profile), "Cannot select video profile");
      auto status = encoder_->SetOutputType(0, output.Get(), 0);
      if (hevc && FAILED(status)) {
        Require(output->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H265), "Cannot select alternate HEVC output");
        status = encoder_->SetOutputType(0, output.Get(), 0);
      }
      Require(status, "Video encoder rejected output configuration", CodecErrorCode::Unsupported);
      const auto row = config.input.width * (ten_bit ? 2 : 1);
      Require(input->SetGUID(MF_MT_SUBTYPE, ten_bit ? MFVideoFormat_P010 : MFVideoFormat_NV12), "Cannot select CPU YUV input");
      Require(input->SetUINT32(MF_MT_DEFAULT_STRIDE, row), "Cannot set YUV stride");
      Require(encoder_->SetInputType(0, input.Get(), 0), "Video encoder rejected CPU YUV input", CodecErrorCode::Unsupported);
      ComPtr<IMFMediaType> actual;
      Require(encoder_->GetOutputCurrentType(0, &actual), "Cannot query video output configuration");
      UINT32 bitrate = 0, actual_profile = 0;
      Require(actual->GetUINT32(MF_MT_AVG_BITRATE, &bitrate), "Video bitrate is missing");
      Require(actual->GetUINT32(MF_MT_MPEG2_PROFILE, &actual_profile), "Video profile is missing");
      if (bitrate != config.bitrate || actual_profile != UINT32(profile))
        throw EncoderFailure{Error(CodecErrorCode::Unsupported, "Video encoder changed requested controls")};
      Require(encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0), "Cannot begin video encoding");
      Require(encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0), "Cannot start video encoding");
      frame_bytes_ = std::size_t(row) * config.input.height * 3 / 2;
    });
  }
  CodecResult<SubmitStatus> Submit(const VideoFrame& frame) override {
    return Guard<SubmitStatus>([&] {
      if (submitted_frames_ > std::uint64_t(INT64_MAX) / config_.frame_rate_denominator)
        throw EncoderFailure{Error(CodecErrorCode::InvalidData, "Video picture identity exceeds the codec timeline")};
      // Use a fixed codec clock for picture identity; Media Foundation interpolates B-picture timestamps.
      // Original variable-rate PTS is restored from the bounded identity map, independently of that clock.
      const MediaTimestamp identity{std::int64_t(submitted_frames_ * config_.frame_rate_denominator), config_.frame_rate_numerator};
      const MediaTimestamp interval{config_.frame_rate_denominator, config_.frame_rate_numerator};
      const auto pts = identity.Rescale(10000000), duration = interval.Rescale(10000000);
      const auto expected = identity.Rescale(1000000);
      if (!pts || !duration || duration->ticks <= 0)
        throw EncoderFailure{Error(CodecErrorCode::InvalidData, "Video timing exceeds Media Foundation range")};
      ComPtr<IMFSample> sample; ComPtr<IMFMediaBuffer> buffer;
      Require(MFCreateSample(&sample), "Cannot create video input sample");
      Require(MFCreateMemoryBuffer(static_cast<DWORD>(frame_bytes_), &buffer), "Cannot allocate YUV input");
      BYTE* target = nullptr;
      Require(buffer->Lock(&target, nullptr, nullptr), "Cannot map YUV input");
      const auto layout = config_.input.pixel_format == VideoPixelFormat::P010 ? VideoPixelFormat::P010 : VideoPixelFormat::NV12;
      CopyVideoEncoderPixels(frame, layout, {reinterpret_cast<std::byte*>(target), frame_bytes_});
      Require(buffer->Unlock(), "Cannot unmap YUV input");
      Require(buffer->SetCurrentLength(static_cast<DWORD>(frame_bytes_)), "Cannot set YUV input length");
      Require(sample->AddBuffer(buffer.Get()), "Cannot attach YUV input");
      Require(sample->SetSampleTime(pts->ticks), "Cannot set video input PTS");
      Require(sample->SetSampleDuration(duration->ticks), "Cannot set video input duration");
      if (key_requested_) Set(CODECAPI_AVEncVideoForceKeyFrame, 1);
      const auto status = encoder_->ProcessInput(0, sample.Get(), 0);
      if (status == MF_E_NOTACCEPTING) return SubmitStatus::WouldBlock;
      Require(status, "Video encoder rejected input");
      input_submitted_ = true;
      variable_timing_ |= !expected || frame.pts.ticks != expected->ticks || (frame.duration && *frame.duration != interval);
      input_times_.emplace(pts->ticks, frame.pts.ticks);
      ++submitted_frames_;
      key_requested_ = false;
      return SubmitStatus::Accepted;
    });
  }
  CodecResult<std::optional<VideoEncoderBackendOutput>> Poll() override {
    return Guard<std::optional<VideoEncoderBackendOutput>>([&]() -> std::optional<VideoEncoderBackendOutput> {
      if (pending_) { auto value = std::move(*pending_); pending_.reset(); return value; }
      if (eos_) return std::nullopt;
      MFT_OUTPUT_STREAM_INFO info{};
      Require(encoder_->GetOutputStreamInfo(0, &info), "Cannot query video output storage");
      if (info.cbSize > std::max(config_.max_packet_bytes, frame_bytes_ + 65536))
        throw EncoderFailure{Error(CodecErrorCode::ResourceExhausted, "Video output scratch exceeds configured bounds")};
      ComPtr<IMFSample> sample;
      if (!(info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES)) {
        ComPtr<IMFMediaBuffer> buffer;
        Require(MFCreateSample(&sample), "Cannot allocate video output sample");
        Require(MFCreateAlignedMemoryBuffer(info.cbSize, info.cbAlignment ? info.cbAlignment - 1 : 0, &buffer), "Cannot allocate video output scratch");
        Require(sample->AddBuffer(buffer.Get()), "Cannot attach video output scratch");
      }
      MFT_OUTPUT_DATA_BUFFER output{0, sample.Get(), 0, nullptr}; DWORD flags = 0;
      const auto status = encoder_->ProcessOutput(0, 1, &output, &flags);
      if (output.pEvents) output.pEvents->Release();
      if (!sample && output.pSample) sample.Attach(output.pSample);
      if (status == MF_E_TRANSFORM_NEED_MORE_INPUT) {
        if (finished_) { eos_ = true; return VideoEncoderEnd{}; }
        return std::nullopt;
      }
      Require(status, "Video encoding failed");
      if (!sample) throw EncoderFailure{Error(CodecErrorCode::InvalidData, "Video encoder returned no sample")};
      LONGLONG pts = 0;
      Require(sample->GetSampleTime(&pts), "Video output PTS is missing");
      if (!time_offset_) {
        if (input_times_.empty()) throw EncoderFailure{Error(CodecErrorCode::InvalidData, "Video output has no submitted picture")};
        // Reordered Media Foundation output starts on a codec timeline delayed from the first input picture.
        time_offset_ = pts;
      }
      pts = InputTime(pts);
      auto matched = input_times_.end();
      for (auto next = input_times_.begin(); next != input_times_.end(); ++next) {
        const auto distance = next->first >= pts ? std::uint64_t(next->first) - std::uint64_t(pts) :
            std::uint64_t(pts) - std::uint64_t(next->first);
        // B-picture interpolation can move a rounded input timestamp by up to one microsecond.
        if (distance > 10) continue;
        if (matched != input_times_.end())
          throw EncoderFailure{Error(CodecErrorCode::InvalidData, "Video output timing is ambiguous at Media Foundation precision")};
        matched = next;
      }
      if (matched == input_times_.end()) {
        std::string detail = std::to_string(pts) + (finished_ ? " drained; pending" : " running; pending");
        for (const auto& [time, original] : input_times_) detail += " " + std::to_string(time);
        UINT32 picture_type = 99;
        sample->GetUINT32(MFSampleExtension_VideoEncodePictureType, &picture_type);
        detail += "; picture=" + std::to_string(picture_type);
        throw EncoderFailure{Error(CodecErrorCode::InvalidData, "Video output timing cannot be correlated with submitted pixels", std::move(detail))};
      }
      const auto original_pts = matched->second;
      input_times_.erase(matched);
      ComPtr<IMFMediaBuffer> buffer;
      Require(sample->ConvertToContiguousBuffer(&buffer), "Cannot access video packet");
      DWORD size = 0;
      Require(buffer->GetCurrentLength(&size), "Cannot query video packet length");
      if (!size || size > config_.max_packet_bytes)
        throw EncoderFailure{Error(CodecErrorCode::ResourceExhausted, "Video access unit exceeds configured byte bound")};
      BYTE* data = nullptr;
      Require(buffer->Lock(&data, nullptr, nullptr), "Cannot map video packet");
      auto normalized = NormalizeVideoEncoderPacket({reinterpret_cast<std::byte*>(data), size}, 0, config_.max_packet_bytes, config_.codec);
      Require(buffer->Unlock(), "Cannot unmap video packet");
      if (!normalized.Succeeded()) throw EncoderFailure{normalized.Error()};
      EncodedPacket packet{Own(std::move(normalized).Value()), {original_pts, 1000000}, std::nullopt, std::nullopt, false};
      UINT64 dts;
      if (!variable_timing_ && SUCCEEDED(sample->GetUINT64(MFSampleExtension_DecodeTimestamp, &dts)))
        packet.dts = MediaTimestamp{InputTime(static_cast<std::int64_t>(dts)), 10000000};
      auto format = ReadFormat();
      if (!format_) {
        format_ = format; pending_ = std::move(packet);
        return format;
      }
      if (*format_ != format) throw EncoderFailure{Error(CodecErrorCode::Unsupported, "Video output configuration changed")};
      return packet;
    });
  }
  CodecResult<void> RequestKeyFrame() override { key_requested_ = true; return CodecResult<void>::Success(); }
  CodecResult<SubmitStatus> Finish() override {
    return Guard<SubmitStatus>([&] {
      Require(encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0), "Video encoder rejected EOS");
      Require(encoder_->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0), "Video drain failed");
      finished_ = true;
      return SubmitStatus::Accepted;
    });
  }
  CodecResult<void> Close() override {
    std::optional<CodecError> failure;
    if (encoder_ && input_submitted_) {
      for (auto message : {MFT_MESSAGE_COMMAND_FLUSH, MFT_MESSAGE_NOTIFY_END_STREAMING}) {
        const auto status = encoder_->ProcessMessage(message, 0);
        if (FAILED(status)) failure = Error(CodecErrorCode::PlatformFailure, "Video encoder teardown failed", std::to_string(status));
      }
    }
    pending_.reset(); input_times_.clear(); controls_.Reset(); encoder_.Reset();
    if (mf_) {
      const auto status = MFShutdown(); mf_ = false;
      if (FAILED(status)) failure = Error(CodecErrorCode::PlatformFailure, "Video encoder MFShutdown failed", std::to_string(status));
    }
    if (com_) { CoUninitialize(); com_ = false; }
    return failure ? CodecResult<void>::Failure(*failure) : CodecResult<void>::Success();
  }
private:
  LONGLONG InputTime(LONGLONG value) const {
    const auto offset = time_offset_.value_or(0);
    if ((offset > 0 && value < INT64_MIN + offset) || (offset < 0 && value > INT64_MAX + offset))
      throw EncoderFailure{Error(CodecErrorCode::InvalidData, "Video codec timeline exceeds signed timestamp bounds")};
    return value - offset;
  }
  void Set(const GUID& property, ULONG value) {
    VARIANT setting{}; setting.vt = VT_UI4; setting.ulVal = value;
    Require(controls_->SetValue(&property, &setting), "Video encoder rejected a requested control", CodecErrorCode::Unsupported);
    VARIANT actual{};
    const auto status = controls_->GetValue(&property, &actual);
    const bool matches = SUCCEEDED(status) && actual.vt == VT_UI4 && actual.ulVal == value;
    VariantClear(&actual);
    if (!matches && property != CODECAPI_AVEncVideoForceKeyFrame)
      throw EncoderFailure{Error(CodecErrorCode::Unsupported, "Video encoder changed a requested control")};
  }
  static void SetColor(IMFMediaType* type, const VideoFormat& input) {
    if (input.primaries != ColorPrimaries::Unknown)
      Require(type->SetUINT32(MF_MT_VIDEO_PRIMARIES, input.primaries == ColorPrimaries::BT709 ? MFVideoPrimaries_BT709 :
          input.primaries == ColorPrimaries::BT601_525 ? MFVideoPrimaries_SMPTE170M : MFVideoPrimaries_BT470_2_SysBG), "Cannot set video primaries");
    if (input.transfer != ColorTransfer::Unknown)
      Require(type->SetUINT32(MF_MT_TRANSFER_FUNCTION, input.transfer == ColorTransfer::BT709 ? MFVideoTransFunc_709 :
          input.transfer == ColorTransfer::SRGB ? MFVideoTransFunc_sRGB : MFVideoTransFunc_10), "Cannot set video transfer");
    if (input.matrix != ColorMatrix::Unknown)
      Require(type->SetUINT32(MF_MT_YUV_MATRIX, input.matrix == ColorMatrix::BT709 ? MFVideoTransferMatrix_BT709 : MFVideoTransferMatrix_BT601), "Cannot set video matrix");
    if (input.range != ColorRange::Unknown)
      Require(type->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, input.range == ColorRange::Full ? MFNominalRange_0_255 : MFNominalRange_16_235), "Cannot set video range");
  }
  VideoEncodeFormat ReadFormat() {
    ComPtr<IMFMediaType> type;
    Require(encoder_->GetOutputCurrentType(0, &type), "Cannot query video configuration");
    UINT32 size = 0;
    Require(type->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &size), "Video parameter sets are missing");
    if (!size || size > 65536) throw EncoderFailure{Error(CodecErrorCode::ResourceExhausted, "Video parameter sets exceed 64 KiB")};
    Bytes bytes(size);
    Require(type->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, reinterpret_cast<UINT8*>(bytes.data()), size, &size), "Cannot read video parameter sets");
    auto format = ReadVideoEncodeFormat(bytes, config_);
    if (!format.Succeeded()) throw EncoderFailure{format.Error()};
    return std::move(format).Value();
  }
  VideoEncoderConfig config_;
  ComPtr<IMFTransform> encoder_;
  ComPtr<ICodecAPI> controls_;
  std::optional<VideoEncodeFormat> format_;
  std::optional<EncodedPacket> pending_;
  std::map<LONGLONG, std::int64_t> input_times_;
  std::optional<LONGLONG> time_offset_;
  std::size_t frame_bytes_ = 0;
  std::uint64_t submitted_frames_ = 0;
  bool com_ = false, mf_ = false, input_submitted_ = false, variable_timing_ = false, key_requested_ = false, finished_ = false, eos_ = false;
};
}
std::unique_ptr<VideoEncoderBackend> MakeVideoEncoderBackend() { return std::make_unique<WindowsVideoEncoder>(); }
} // namespace huxerui::media::detail
