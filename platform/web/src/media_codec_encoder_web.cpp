#include "codec/encoder/backend.h"

#include <emscripten/val.h>

namespace huxerui::media::detail {
namespace {
using emscripten::val;
val Uint8(std::span<const std::byte> bytes) {
  return val(emscripten::typed_memory_view(bytes.size(), reinterpret_cast<const unsigned char*>(bytes.data()))).call<val>("slice");
}
Bytes Copy(const val& source) {
  Bytes bytes(source["length"].as<std::size_t>());
  val(emscripten::typed_memory_view(bytes.size(), reinterpret_cast<unsigned char*>(bytes.data()))).call<void>("set", source);
  return bytes;
}
class WebAudioEncoder final : public AudioEncoderBackend {
public:
  CodecResult<void> Open(const AudioEncoderConfig& config) override {
    config_ = config;
    config_.wakeup = {}; config_.cleanup_completed = {};
    auto options = val::object();
    options.set("rate", config.input.sample_rate); options.set("channels", config.input.ChannelCount()); options.set("bitrate", config.bitrate);
    id_ = bridge_.call<unsigned>("open", options);
    return CodecResult<void>::Success();
  }
  CodecResult<bool> Ready() override {
    const auto error = ErrorText();
    if (!error.empty()) return CodecResult<bool>::Failure(Error(CodecErrorCode::Unsupported, error, "WebCodecs"));
    return CodecResult<bool>::Success(bridge_.call<bool>("ready", id_));
  }
  CodecResult<SubmitStatus> Submit(std::span<const std::byte> pcm, std::uint32_t samples, std::int64_t first_sample) override {
    const auto pts = MediaTimestamp{first_sample, config_.input.sample_rate}.Rescale(1000000);
    if (!pts || pts->ticks > 9007199254740991LL)
      return CodecResult<SubmitStatus>::Failure(Error(CodecErrorCode::InvalidData, "PCM timestamp exceeds WebCodecs safe integer range"));
    const auto accepted = bridge_.call<bool>("submit", id_, Uint8(pcm), double(pts->ticks), samples);
    const auto error = ErrorText();
    if (!error.empty()) return CodecResult<SubmitStatus>::Failure(Error(CodecErrorCode::PlatformFailure, error, "WebCodecs"));
    return CodecResult<SubmitStatus>::Success(accepted ? SubmitStatus::Accepted : SubmitStatus::WouldBlock);
  }
  CodecResult<std::optional<AudioEncoderBackendOutput>> Poll() override {
    using R = CodecResult<std::optional<AudioEncoderBackendOutput>>;
    const auto error = ErrorText();
    if (!error.empty()) return R::Failure(Error(CodecErrorCode::PlatformFailure, error, "WebCodecs"));
    auto value = bridge_.call<val>("poll", id_);
    if (value.isNull()) {
      if (!eos_ && bridge_.call<bool>("ended", id_)) { eos_ = true; return R::Success(AudioEncoderEnd{}); }
      return R::Success(std::nullopt);
    }
    if (value["kind"].as<std::string>() == "format") {
      auto parsed = ReadAudioEncodeFormat(Copy(value["description"]), config_);
      if (!parsed.Succeeded()) return R::Failure(parsed.Error());
      return R::Success(std::move(parsed).Value());
    }
    const auto length = value["data"]["length"].as<std::size_t>();
    if (!length || length > kAudioEncoderPacketBytes)
      return R::Failure(Error(CodecErrorCode::ResourceExhausted, "WebCodecs AAC output exceeds 64 KiB"));
    EncodedPacket packet{Own(Copy(value["data"])), {static_cast<std::int64_t>(value["pts"].as<double>()), 1000000}, std::nullopt, std::nullopt, true};
    return R::Success(std::move(packet));
  }
  CodecResult<SubmitStatus> Finish() override {
    bridge_.call<void>("finish", id_);
    const auto error = ErrorText();
    if (!error.empty()) return CodecResult<SubmitStatus>::Failure(Error(CodecErrorCode::PlatformFailure, error, "WebCodecs.flush"));
    return CodecResult<SubmitStatus>::Success(SubmitStatus::Accepted);
  }
  CodecResult<void> Close() override {
    if (!id_) return CodecResult<void>::Success();
    const auto error = bridge_.call<std::string>("close", id_); id_ = 0;
    return error.empty() ? CodecResult<void>::Success() : CodecResult<void>::Failure(Error(CodecErrorCode::PlatformFailure, error, "WebCodecs.close"));
  }
private:
  std::string ErrorText() { return bridge_.call<std::string>("error", id_); }
  val bridge_ = val::global("HuxerMediaAudioEncoder");
  AudioEncoderConfig config_;
  unsigned id_ = 0;
  bool eos_ = false;
};
} // namespace
std::unique_ptr<AudioEncoderBackend> MakeAudioEncoderBackend() { return std::make_unique<WebAudioEncoder>(); }
} // namespace huxerui::media::detail
