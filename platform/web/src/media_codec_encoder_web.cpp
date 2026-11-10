#include "codec/encoder/backend.h"
#include "format/platform_video_storage.h"
#include "media_video_web.h"

#include <emscripten/threading.h>
#include <emscripten/val.h>
#include <map>

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

namespace {
class WebVideoEncoder final : public VideoEncoderBackend {
public:
  CodecResult<void> Open(const VideoEncoderConfig& config) override {
    const bool platform = config.input_storage == VideoStorage::Platform;
    if (platform && !emscripten_is_main_browser_thread())
      return CodecResult<void>::Failure(Error(CodecErrorCode::Unsupported, "Web platform video requires the browser main thread"));
    config_ = config; config_.wakeup = {}; config_.cleanup_completed = {};
    auto options = val::object();
    options.set("width", config.input.width); options.set("height", config.input.height);
    options.set("rateNumerator", config.frame_rate_numerator); options.set("rateDenominator", config.frame_rate_denominator);
    options.set("bitrate", config.bitrate); options.set("keyInterval", config.key_frame_interval);
    options.set("maximum", static_cast<unsigned>(config.max_packet_bytes));
    options.set("window", static_cast<unsigned>(VideoEncoderWindow(config)));
    options.set("profile", config.profile == H264Profile::Baseline ? 66 : config.profile == H264Profile::Main ? 77 : 100);
    options.set("maxB", config.max_b_frames ? val(*config.max_b_frames) : val::null());
    options.set("hevc", config.codec == CodecId::Hevc);
    options.set("layout", platform ? "Opaque" : config.input.pixel_format == VideoPixelFormat::P010 ? "P010" :
        config.input.pixel_format == VideoPixelFormat::NV12 ? "NV12" : "I420");
    auto color = val::object();
    if (config.input.primaries != ColorPrimaries::Unknown)
      color.set("primaries", config.input.primaries == ColorPrimaries::BT709 ? "bt709" :
          config.input.primaries == ColorPrimaries::BT601_525 ? "smpte170m" : "bt470bg");
    if (config.input.transfer != ColorTransfer::Unknown)
      color.set("transfer", config.input.transfer == ColorTransfer::BT709 ? "bt709" :
          config.input.transfer == ColorTransfer::SRGB ? "iec61966-2-1" : "linear");
    if (config.input.matrix != ColorMatrix::Unknown)
      color.set("matrix", config.input.matrix == ColorMatrix::BT709 ? "bt709" : "smpte170m");
    if (config.input.range != ColorRange::Unknown) color.set("fullRange", config.input.range == ColorRange::Full);
    options.set("color", color);
    id_ = bridge_.call<unsigned>("open", options);
    return CodecResult<void>::Success();
  }
  CodecResult<bool> Ready() override {
    const auto error = ErrorText();
    return error.empty() ? CodecResult<bool>::Success(bridge_.call<bool>("ready", id_)) :
        CodecResult<bool>::Failure(Error(CodecErrorCode::Unsupported, error, "WebCodecs"));
  }
  CodecResult<SubmitStatus> Submit(const VideoFrame& frame) override {
    using R = CodecResult<SubmitStatus>;
    const auto duration = frame.duration ? frame.duration->Rescale(1000000) : std::optional<MediaTimestamp>{};
    if (frame.pts.ticks > 9007199254740991LL ||
        (frame.duration && (!duration || duration->ticks > 9007199254740991LL)))
      return R::Failure(Error(CodecErrorCode::InvalidData, "Video timing exceeds the WebCodecs safe integer range"));
    auto input = val::undefined();
    if (frame.Storage() == VideoStorage::Platform) {
      const auto* source = BrowserFrame(frame);
      if (!source) return R::Failure(Error(CodecErrorCode::Unsupported, "Expected a WebCodecs video frame"));
      const auto valid = ValidateBrowserFrame(*source, config_.input);
      if (!valid.Succeeded()) return R::Failure(valid.Error());
      input = *source;
      // The browser retains the image but cannot keep the C++ storage reservations alive.
      pending_frames_.emplace(frame.pts.ticks, VideoFrameAccess::PlatformStorage(frame));
    } else {
      const auto sample = config_.input.pixel_format == VideoPixelFormat::P010 ? 2 : 1;
      Bytes pixels(std::size_t(config_.input.width) * config_.input.height * sample * 3 / 2);
      CopyVideoEncoderPixels(frame, config_.input.pixel_format, pixels);
      input = Uint8(pixels);
    }
    const auto accepted = bridge_.call<bool>("submit", id_, input, double(frame.pts.ticks),
        duration ? val(double(duration->ticks)) : val::null());
    auto result = Check();
    if (!result.Succeeded()) return R::Failure(result.Error());
    if (accepted) return R::Success(SubmitStatus::Accepted);
    pending_frames_.erase(frame.pts.ticks);
    return R::Success(SubmitStatus::WouldBlock);
  }
  CodecResult<std::optional<VideoEncoderBackendOutput>> Poll() override {
    using R = CodecResult<std::optional<VideoEncoderBackendOutput>>;
    auto checked = Check();
    if (!checked.Succeeded()) return R::Failure(checked.Error());
    auto value = bridge_.call<val>("poll", id_);
    if (value.isNull()) {
      if (!eos_ && bridge_.call<bool>("ended", id_)) { eos_ = true; return R::Success(VideoEncoderEnd{}); }
      return R::Success(std::nullopt);
    }
    if (value["kind"].as<std::string>() == "format") {
      const auto size = value["description"]["length"].as<unsigned>();
      if (!size || size > 65536) return R::Failure(Error(CodecErrorCode::ResourceExhausted, "Video configuration exceeds 64 KiB"));
      auto bytes = Copy(value["description"]);
      if (bytes.front() != std::byte{1})
        return R::Failure(Error(CodecErrorCode::InvalidData, "WebCodecs did not return a configuration record"));
      auto format = ReadVideoEncodeFormat(bytes, config_);
      if (!format.Succeeded()) return R::Failure(format.Error());
      length_size_ = (std::to_integer<unsigned>(bytes[config_.codec == CodecId::Hevc ? 21 : 4]) & 3) + 1;
      return R::Success(std::move(format).Value());
    }
    const auto size = value["data"]["length"].as<unsigned>();
    if (!size || size > config_.max_packet_bytes)
      return R::Failure(Error(CodecErrorCode::ResourceExhausted, "WebCodecs access unit exceeds the packet bound"));
    auto bytes = Copy(value["data"]);
    auto normalized = NormalizeVideoEncoderPacket(bytes, length_size_, config_.max_packet_bytes, config_.codec);
    if (!normalized.Succeeded()) return R::Failure(normalized.Error());
    const auto pts = static_cast<std::int64_t>(value["pts"].as<double>());
    pending_frames_.erase(pts);
    return R::Success(EncodedPacket{Own(std::move(normalized).Value()),
        {pts, 1000000}, std::nullopt, std::nullopt, false});
  }
  CodecResult<void> RequestKeyFrame() override { bridge_.call<void>("requestKeyFrame", id_); return Check(); }
  CodecResult<void> FlushPending() override { bridge_.call<void>("progress", id_); return Check(); }
  CodecResult<SubmitStatus> Finish() override {
    bridge_.call<void>("finish", id_);
    auto result = Check();
    return result.Succeeded() ? CodecResult<SubmitStatus>::Success(SubmitStatus::Accepted) : CodecResult<SubmitStatus>::Failure(result.Error());
  }
  CodecResult<void> Close() override {
    if (!id_) return CodecResult<void>::Success();
    const auto error = bridge_.call<std::string>("close", id_); id_ = 0;
    pending_frames_.clear();
    return error.empty() ? CodecResult<void>::Success() : CodecResult<void>::Failure(Error(CodecErrorCode::PlatformFailure, error, "WebCodecs.close"));
  }
private:
  std::string ErrorText() { return bridge_.call<std::string>("error", id_); }
  CodecResult<void> Check() {
    const auto error = ErrorText();
    return error.empty() ? CodecResult<void>::Success() : CodecResult<void>::Failure(Error(CodecErrorCode::PlatformFailure, error, "WebCodecs"));
  }
  val bridge_ = val::global("HuxerMediaVideoEncoder");
  VideoEncoderConfig config_;
  std::map<std::int64_t, std::shared_ptr<const PlatformVideoStorage>> pending_frames_;
  unsigned id_ = 0, length_size_ = 4;
  bool eos_ = false;
};
}
std::unique_ptr<VideoEncoderBackend> MakeVideoEncoderBackend() { return std::make_unique<WebVideoEncoder>(); }
} // namespace huxerui::media::detail
