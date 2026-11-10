#include "codec/decoder/backend.h"
#include "codec/decoder/copy.h"
#include "codec/hevc.h"
#include <algorithm>
#include <emscripten/val.h>

namespace huxerui::media::detail {
namespace {
using emscripten::val;
val Uint8(std::span<const std::byte> bytes) {
  return val(emscripten::typed_memory_view(bytes.size(), reinterpret_cast<const unsigned char*>(bytes.data()))).call<val>("slice");
}
BufferReference Buffer(const val& array) {
  Bytes bytes(array["length"].as<std::size_t>());
  val(emscripten::typed_memory_view(bytes.size(), reinterpret_cast<unsigned char*>(bytes.data()))).call<void>("set", array);
  return Own(std::move(bytes));
}
class WebDecoder final : public DecoderBackend {
public:
  std::uint32_t PresentationTimescale() const noexcept override {
    // AudioData timestamps can be rounded from the decoder's sample clock back to microseconds.
    if (const auto* audio = std::get_if<AudioDecoderConfig>(&config_)) return audio->output.sample_rate;
    return 1000000;
  }

  CodecResult<void> Open(const DecoderConfig& config) override {
    config_ = config;
    auto options = val::object();
    if (const auto* audio = std::get_if<AudioDecoderConfig>(&config)) {
      switch (audio->codec) {
        case CodecId::Aac: options.set("codec", std::string("mp4a.40.2")); break;
        case CodecId::Opus: options.set("codec", std::string("opus")); break;
        case CodecId::Flac: options.set("codec", std::string("flac")); break;
        case CodecId::Mp3:
          options.set("codec", std::string("mp3"));
          // Raw MP3 WebCodecs chunks carry the synthesis delay, which the file trim does not include.
          mp3_.emplace(audio->output, AudioSampleLimit(*audio).Value(), 529);
          break;
        default: return CodecResult<void>::Failure(Error(CodecErrorCode::Unsupported, "Unsupported WebCodecs audio codec"));
      }
      options.set("audio", true); options.set("rate", audio->output.sample_rate); options.set("channels", audio->output.ChannelCount());
      options.set("maxSamples", AudioSampleLimit(*audio).Value());
      options.set("planar", audio->output.packing == AudioPacking::Planar);
      std::string sample = audio->output.sample_format == AudioSampleFormat::F32 ? "f32" : "s16";
      if (audio->output.packing == AudioPacking::Planar) sample += "-planar";
      options.set("sampleFormat", sample); options.set("description", Uint8(AudioDecoderHeader(*audio)));
    } else {
      const auto& video = std::get<VideoDecoderConfig>(config);
      options.set("audio", false); options.set("width", video.width); options.set("height", video.height);
      if (video.codec == CodecId::Hevc) options.set("codec", HevcCodecString(video.initialization_data));
      options.set("tenBit", video.output == VideoPixelFormat::P010);
      options.set("description", Uint8(video.initialization_data));
    }
    id_ = bridge_.call<unsigned>("open", options);
    return CodecResult<void>::Success();
  }
  CodecResult<bool> Ready() override {
    const auto error = ErrorText();
    if (!error.empty()) return CodecResult<bool>::Failure(Error(CodecErrorCode::Unsupported, error, "WebCodecs"));
    return CodecResult<bool>::Success(bridge_.call<bool>("ready", id_));
  }
  CodecResult<SubmitStatus> Submit(const EncodedPacket& packet) override {
    const auto pts = packet.pts.Rescale(1000000).value().ticks;
    const auto duration = packet.duration ? val(double(packet.duration->Rescale(1000000).value().ticks)) : val::null();
    const auto bytes = packet.data.AsBytes();
    bridge_.call<void>("submit", id_, Uint8(bytes), double(pts), duration, packet.key_frame);
    const auto error = ErrorText();
    if (!error.empty()) return CodecResult<SubmitStatus>::Failure(Error(CodecErrorCode::InvalidData, error, "WebCodecs"));
    if (mp3_) {
      if (mp3_drain_frame_.empty()) {
        // An empty, reservoir-free frame releases the decoder's final synthesis samples.
        mp3_drain_frame_.resize(bytes.size());
        std::ranges::copy(bytes.first(4), mp3_drain_frame_.begin());
        mp3_drain_frame_[1] |= std::byte{1};
      }
      mp3_->Expect(packet.pts);
    }
    return CodecResult<SubmitStatus>::Success(SubmitStatus::Accepted);
  }
  CodecResult<std::optional<BackendOutput>> Poll() override {
    using R = CodecResult<std::optional<BackendOutput>>;
    const auto error = ErrorText();
    if (!error.empty()) return R::Failure(Error(CodecErrorCode::PlatformFailure, error, "WebCodecs"));
    if (mp3_) {
      if (auto frame = mp3_->Read()) return R::Success(BackendOutput{DecodedFrame{std::move(*frame)}});
    }
    auto value = bridge_.call<val>("poll", id_);
    if (value.isNull()) {
      if (!eos_ && bridge_.call<bool>("ended", id_)) {
        if (mp3_ && !mp3_->Drained()) return R::Failure(Error(CodecErrorCode::InvalidData, "MP3 drain has missing samples"));
        eos_ = true;
        return R::Success(BackendOutput{EndOfStream{}});
      }
      return R::Success(std::nullopt);
    }
    MediaTimestamp pts{static_cast<std::int64_t>(value["pts"].as<double>()), 1000000};
    auto planes = value["planes"];
    if (const auto* audio = std::get_if<AudioDecoderConfig>(&config_)) {
      AudioFrame frame{audio->output, pts, value["samples"].as<std::uint32_t>(), {}};
      for (unsigned i = 0; i < planes["length"].as<unsigned>(); ++i) frame.planes.push_back(Buffer(planes[i]));
      if (mp3_) {
        const auto appended = mp3_->Append(frame, draining_);
        if (!appended.Succeeded()) return R::Failure(appended.Error());
        if (auto packet = mp3_->Read()) return R::Success(BackendOutput{DecodedFrame{std::move(*packet)}});
        return R::Success(std::nullopt);
      }
      return R::Success(BackendOutput{DecodedFrame{std::move(frame)}});
    }
    const auto& config = std::get<VideoDecoderConfig>(config_);
    VideoFormat format;
    format.width = value["width"].as<unsigned>(); format.height = value["height"].as<unsigned>();
    auto rect = value["visible"];
    format.visible = {rect["x"].as<unsigned>(), rect["y"].as<unsigned>(), rect["width"].as<unsigned>(), rect["height"].as<unsigned>()};
    const auto layout = value["format"].as<std::string>();
    format.pixel_format = layout == "P010" ? VideoPixelFormat::P010 : layout == "I420" ? VideoPixelFormat::I420 : VideoPixelFormat::NV12;
    format.aspect_width = value["aspectWidth"].as<unsigned>(); format.aspect_height = value["aspectHeight"].as<unsigned>();
    auto color = value["color"];
    if (color["primaries"].equals(val("bt709"))) format.primaries = ColorPrimaries::BT709;
    else if (color["primaries"].equals(val("bt2020"))) format.primaries = ColorPrimaries::BT2020;
    if (color["transfer"].equals(val("bt709"))) format.transfer = ColorTransfer::BT709;
    else if (color["transfer"].equals(val("iec61966-2-1"))) format.transfer = ColorTransfer::SRGB;
    else if (color["transfer"].equals(val("pq"))) format.transfer = ColorTransfer::PQ;
    else if (color["transfer"].equals(val("hlg"))) format.transfer = ColorTransfer::HLG;
    if (color["matrix"].equals(val("bt709"))) format.matrix = ColorMatrix::BT709;
    else if (color["matrix"].equals(val("smpte170m"))) format.matrix = ColorMatrix::BT601;
    else if (color["matrix"].equals(val("bt2020-ncl"))) format.matrix = ColorMatrix::BT2020;
    if (!color["fullRange"].isNull() && !color["fullRange"].isUndefined()) format.range = color["fullRange"].as<bool>() ? ColorRange::Full : ColorRange::Limited;
    std::vector<VideoPlane> source_planes;
    const auto component = layout == "P010" ? 2u : 1u;
    for (unsigned i = 0; i < planes["length"].as<unsigned>(); ++i)
      source_planes.push_back({Buffer(planes[i]), value["strides"][i].as<unsigned>(), i && layout != "I420" ? component * 2 : component});
    auto duration = value["duration"].isNull() ? std::nullopt : std::optional(MediaTimestamp{static_cast<std::int64_t>(value["duration"].as<double>()), 1000000});
    auto copied = CopyVideo(format, pts, duration, std::move(source_planes), config.output);
    if (!copied.Succeeded()) return R::Failure(copied.Error());
    return R::Success(BackendOutput{DecodedFrame{std::move(copied).Value()}});
  }
  CodecResult<SubmitStatus> Finish() override {
    if (!mp3_drain_frame_.empty()) {
      bridge_.call<void>("submit", id_, Uint8(mp3_drain_frame_), 0.0, val::null(), true);
      const auto error = ErrorText();
      if (!error.empty()) return CodecResult<SubmitStatus>::Failure(Error(CodecErrorCode::InvalidData, error, "WebCodecs"));
    }
    draining_ = true;
    bridge_.call<void>("finish", id_);
    return CodecResult<SubmitStatus>::Success(SubmitStatus::Accepted);
  }
  CodecResult<void> Close() override {
    if (!id_) return CodecResult<void>::Success();
    const auto error = bridge_.call<std::string>("close", id_); id_ = 0;
    if (!error.empty()) return CodecResult<void>::Failure(Error(CodecErrorCode::PlatformFailure, error, "WebCodecs.close"));
    return CodecResult<void>::Success();
  }
private:
  std::string ErrorText() { return bridge_.call<std::string>("error", id_); }
  val bridge_ = val::global("HuxerMediaCodec");
  DecoderConfig config_;
  std::optional<PcmPackets> mp3_;
  Bytes mp3_drain_frame_;
  unsigned id_ = 0;
  bool eos_ = false;
  bool draining_ = false;
};
} // namespace
std::unique_ptr<DecoderBackend> MakeDecoderBackend() { return std::make_unique<WebDecoder>(); }
} // namespace huxerui::media::detail
