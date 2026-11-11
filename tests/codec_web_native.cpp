#include <huxerui/media_codec.h>
#include <huxerui/web/external_texture.h>
#include <emscripten.h>
#include <fstream>
#include <iostream>

using namespace huxerui;
using namespace huxerui::media;
namespace {
Bytes Read(const std::string& path) {
  std::ifstream file("/fixtures/" + path, std::ios::binary | std::ios::ate);
  if (!file) throw std::runtime_error("Missing fixture: " + path);
  Bytes bytes(static_cast<std::size_t>(file.tellg()));
  file.seekg(0);
  file.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
  if (!file) throw std::runtime_error("Incomplete fixture: " + path);
  return bytes;
}
std::vector<EncodedPacket> Packets(const std::string& name) {
  std::ifstream stream("/fixtures/" + name + ".txt");
  std::string path;
  std::int64_t pts, dts, duration;
  std::uint32_t scale;
  bool key;
  std::vector<EncodedPacket> packets;
  while (stream >> path >> pts >> dts >> duration >> scale >> key) {
    auto bytes = std::make_shared<Bytes>(Read(path));
    packets.push_back({BufferReference(*bytes, bytes), {pts, scale}, MediaTimestamp{dts, scale}, MediaTimestamp{duration, scale}, key});
  }
  return packets;
}

void Report(std::string message) {
  EM_ASM({ document.body.textContent = UTF8ToString($0); }, message.c_str());
  std::cout << message << '\n';
  emscripten_cancel_main_loop();
}

struct QueueTest {
  enum class Phase { Starting, KeyQueued, Refilling, ReferencesQueued, Draining, Closing };
  std::optional<VideoDecoder> decoder;
  Phase phase = Phase::Starting;
  bool cleaned = false;
  std::string failure;
  double deadline = emscripten_get_now() + 15000;

  void Start() {
    EM_ASM({
      globalThis.VideoDecoder = class {
        static async isConfigSupported() { return {supported: true}; }
        configure() { globalThis.stalledDecoder = this; this.decodeQueueSize = 0; }
        decode() { ++this.decodeQueueSize; }
        async flush() { this.decodeQueueSize = 0; }
        close() { this.decodeQueueSize = 0; }
      };
    });
    VideoDecoderConfig config{.width = 64, .height = 48, .codec = CodecId::Vp9};
    config.limits.input_items = 2;
    config.limits.input_bytes = 6;
    config.cleanup_completed = [this](CodecResult<void> result) {
      if (!result.Succeeded()) failure = result.Error().message;
      cleaned = true;
    };
    auto created = VideoDecoder::Create(config);
    if (!created.Succeeded()) throw std::runtime_error(created.Error().message);
    decoder.emplace(std::move(created).Value());
  }

  SubmitStatus Submit(Bytes bytes, bool key = false) {
    const auto data = std::make_shared<Bytes>(std::move(bytes));
    const auto result = decoder->TrySubmit({BufferReference(*data, data), {0, 1}, {}, {}, key});
    if (!result.Succeeded()) throw std::runtime_error(result.Error().message);
    return result.Value();
  }

  void Tick() {
    try {
      if (emscripten_get_now() > deadline) throw std::runtime_error("Web decoder input did not make progress");
      const auto pending = EM_ASM_INT({ return globalThis.stalledDecoder?.decodeQueueSize ?? 0; });
      switch (phase) {
        case Phase::Starting:
          if (Submit({std::byte{0x80}, std::byte{0x49}, std::byte{0x83}, std::byte{0x42}}, true) == SubmitStatus::Accepted)
            phase = Phase::KeyQueued;
          break;
        case Phase::KeyQueued:
          if (pending != 1) break;
          if (Submit({std::byte{0x84}, {}, {}}) != SubmitStatus::WouldBlock)
            throw std::runtime_error("A queued reference packet did not retain its input byte credit");
          EM_ASM({ globalThis.stalledDecoder.decodeQueueSize = 0; });
          phase = Phase::Refilling;
          break;
        case Phase::Refilling:
          if (Submit({std::byte{0x84}, {}, {}}) == SubmitStatus::WouldBlock) break;
          if (Submit({std::byte{0x84}}) != SubmitStatus::Accepted)
            throw std::runtime_error("Web decoder did not release the dequeued packet's credit");
          phase = Phase::ReferencesQueued;
          break;
        case Phase::ReferencesQueued:
          if (pending != 2) break;
          if (Submit({std::byte{0x84}}) != SubmitStatus::WouldBlock)
            throw std::runtime_error("Queued reference packets did not retain their input item credits");
          if (const auto result = decoder->Finish(); !result.Succeeded()) throw std::runtime_error(result.Error().message);
          phase = Phase::Draining;
          break;
        case Phase::Draining:
          if (auto result = decoder->TryRead(); !result.Succeeded()) throw std::runtime_error(result.Error().message);
          else if (result.Value()) {
            if (!std::holds_alternative<EndOfStream>(*result.Value())) throw std::runtime_error("Reference packets produced output");
            decoder->Close();
            phase = Phase::Closing;
          }
          break;
        case Phase::Closing:
          if (cleaned) Report(failure.empty() ? "PASS: Web reference packet backpressure, dequeue, and drain" : "FAIL: " + failure);
          break;
      }
    } catch (const std::exception& error) {
      decoder->Close();
      Report(std::string("FAIL: ") + error.what());
    }
  }
};

struct Test : std::enable_shared_from_this<Test> {
  std::optional<AudioDecoder> audio;
  std::optional<VideoDecoder> video;
  std::vector<EncodedPacket> aac, avc;
  std::optional<AudioFrame> held_audio;
  std::optional<VideoFrame> held_video;
  std::shared_ptr<ExternalTexture> held_texture;
  std::optional<MediaTimestamp> texture_pts;
  std::size_t a = 0, v = 0, audio_frames = 0, video_frames = 0;
  bool audio_finish = false, video_finish = false, audio_eos = false, video_eos = false;
  int cleaned = 0, pass = 0;
  bool closing = false, platform = false;
  std::string failure;
  double deadline = emscripten_get_now() + 15000;
  void Start() {
    auto self = shared_from_this();
    auto completion = [self](CodecResult<void> result) {
      if (!result.Succeeded()) self->failure = result.Error().message;
      ++self->cleaned;
    };
    AudioDecoderConfig ac;
    ac.initialization_data = Read("aac.config"); ac.output.sample_rate = 48000; ac.cleanup_completed = completion;
    auto created_audio = AudioDecoder::Create(ac);
    if (!created_audio.Succeeded()) throw std::runtime_error(created_audio.Error().message);
    audio.emplace(std::move(created_audio).Value());
    VideoDecoderConfig vc;
    vc.initialization_data = Read("avc.config"); vc.width = 64; vc.height = 48; vc.cleanup_completed = completion;
    platform = EM_ASM_INT({ return new URLSearchParams(location.search).get('storage') === 'platform'; });
    if (platform) {
      vc.output_storage = VideoStorage::Platform;
      vc.output = VideoPixelFormat::Opaque;
      EM_ASM({
        VideoFrame.prototype.copyTo = function() { throw new Error('Platform output called VideoFrame.copyTo'); };
      });
    }
    auto created_video = VideoDecoder::Create(vc);
    if (!created_video.Succeeded()) throw std::runtime_error(created_video.Error().message);
    video.emplace(std::move(created_video).Value());
    aac = Packets("aac"); avc = Packets("avc");
  }
  template<class Decoder> void Feed(Decoder& decoder, const std::vector<EncodedPacket>& packets, std::size_t& index, bool& finished) {
    while (index < packets.size()) {
      auto submitted = decoder.TrySubmit(packets[index]);
      if (!submitted.Succeeded()) throw std::runtime_error(submitted.Error().message);
      if (submitted.Value() == SubmitStatus::WouldBlock) return;
      ++index;
    }
    if (!finished) {
      auto result = decoder.Finish();
      if (!result.Succeeded()) throw std::runtime_error(result.Error().message);
      finished = true;
    }
  }
  void Tick() {
    try {
      if (!closing) {
        if (emscripten_get_now() > deadline) throw std::runtime_error("WASM decoder timeout");
        Feed(*audio, aac, a, audio_finish); Feed(*video, avc, v, video_finish);
        for (;;) {
          auto result = audio->TryRead();
          if (!result.Succeeded()) throw std::runtime_error(result.Error().message);
          if (!result.Value()) break;
          if (auto* frame = std::get_if<AudioFrame>(&*result.Value())) {
            if (!frame->Validate() || frame->sample_count != 1024) throw std::runtime_error("Invalid WASM PCM output");
            held_audio = *frame; ++audio_frames;
          } else if (std::holds_alternative<EndOfStream>(*result.Value())) audio_eos = true;
        }
        for (;;) {
          auto result = video->TryRead();
          if (!result.Succeeded()) throw std::runtime_error(result.Error().message);
          if (!result.Value()) break;
          if (auto* frame = std::get_if<VideoFrame>(&*result.Value())) {
            if (!frame->Validate() || frame->Storage() != (platform ? VideoStorage::Platform : VideoStorage::Cpu))
              throw std::runtime_error("Invalid WASM video output");
            if (platform) {
              if (frame->format.pixel_format != VideoPixelFormat::Opaque || !frame->planes.empty())
                throw std::runtime_error("WASM platform output exposed CPU planes");
              if (!held_texture) {
                auto texture = MakeExternalTexture(*frame);
                if (!texture.Succeeded()) throw std::runtime_error(texture.Error().message);
                held_texture = std::move(texture).Value();
                texture_pts = frame->pts.Rescale(1000000);
              }
            }
            held_video = *frame; ++video_frames;
          } else if (std::holds_alternative<EndOfStream>(*result.Value())) video_eos = true;
        }
        if (audio_eos && video_eos) {
          if (audio_frames != aac.size() || video_frames != avc.size()) throw std::runtime_error("Unexpected WASM output count");
          if (!pass++) {
            const auto ar = audio->Reset(), vr = video->Reset();
            if (!ar.Succeeded() || !vr.Succeeded()) throw std::runtime_error("WASM reset failed");
            a = v = audio_frames = video_frames = 0;
            audio_finish = video_finish = audio_eos = video_eos = false;
          } else Close();
        }
      }
    } catch (const std::exception& error) { failure = error.what(); Close(); }
    if (closing && cleaned == 2) {
      if (failure.empty() && (!held_audio || !held_video || !held_audio->Validate() || !held_video->Validate())) failure = "Retained WASM output became invalid";
      if (failure.empty() && platform) {
        held_video.reset();
        auto* texture = dynamic_cast<web::VideoFrameTexture*>(held_texture.get());
        if (!texture || texture->Revision() != 1 || !texture_pts) failure = "Missing SDK VideoFrameTexture publication";
        else {
          const auto frame = texture->AcquireFrame();
          if (frame.isUndefined() || frame.isNull()) failure = "SDK texture lost its retained frame";
          else {
            const auto size = texture->IntrinsicSize();
            const auto visible = frame["visibleRect"];
            if (frame["timestamp"].as<double>() != texture_pts->ticks || visible["width"].as<unsigned>() != 64 ||
                visible["height"].as<unsigned>() != 48 || frame["displayWidth"].as<float>() != size.width ||
                frame["displayHeight"].as<float>() != size.height)
              failure = "SDK texture changed the browser frame geometry or timestamp";
            frame.call<void>("close");
          }
        }
        held_texture.reset();
      }
      Report(failure.empty() ? std::string("PASS: C++ WASM AAC/AVC sessions, ") +
          (platform ? "platform texture without pixel readback" : "CPU output") + ", EOS, reset and cleanup" : "FAIL: " + failure);
    }
  }
  void Close() { if (!closing) { closing = true; audio->Close(); video->Close(); } }
};
std::shared_ptr<Test> test;
QueueTest queue_test;
} // namespace
int main() {
  try {
    if (EM_ASM_INT({ return new URLSearchParams(location.search).has('backpressure'); })) {
      queue_test.Start();
      emscripten_set_main_loop([] { queue_test.Tick(); }, 0, false);
      return 0;
    }
    test = std::make_shared<Test>(); test->Start();
    emscripten_set_main_loop([] { test->Tick(); }, 0, false);
  } catch (const std::exception& error) {
    EM_ASM({ document.body.textContent = 'FAIL: ' + UTF8ToString($0); }, error.what());
    return 1;
  }
}
