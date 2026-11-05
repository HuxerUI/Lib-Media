#include <huxerui/mediacodec.h>
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
struct Test : std::enable_shared_from_this<Test> {
  std::optional<AudioDecoder> audio;
  std::optional<VideoDecoder> video;
  std::vector<EncodedPacket> aac, avc;
  std::optional<AudioFrame> held_audio;
  std::optional<VideoFrame> held_video;
  std::size_t a = 0, v = 0, audio_frames = 0, video_frames = 0;
  bool audio_finish = false, video_finish = false, audio_eos = false, video_eos = false;
  int cleaned = 0, pass = 0;
  bool closing = false;
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
            if (!frame->Validate()) throw std::runtime_error("Invalid WASM YUV output");
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
      const auto message = failure.empty() ? "PASS: C++ WASM AAC/AVC sessions, CPU output, EOS, reset and cleanup" : "FAIL: " + failure;
      EM_ASM({ document.body.textContent = UTF8ToString($0); }, message.c_str());
      std::cout << message << '\n';
      emscripten_cancel_main_loop();
    }
  }
  void Close() { if (!closing) { closing = true; audio->Close(); video->Close(); } }
};
std::shared_ptr<Test> test;
} // namespace
int main() {
  try {
    test = std::make_shared<Test>(); test->Start();
    emscripten_set_main_loop([] { test->Tick(); }, 0, false);
  } catch (const std::exception& error) {
    EM_ASM({ document.body.textContent = 'FAIL: ' + UTF8ToString($0); }, error.what());
    return 1;
  }
}
