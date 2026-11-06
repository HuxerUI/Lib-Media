#include <huxerui/media_codec.h>
#include <huxerui/media_playback.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>

using namespace huxerui;
using namespace huxerui::media;
using namespace std::chrono_literals;

namespace {

void Check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

template<class T, class E> T Require(Result<T, E> result) {
  if (!result.Succeeded()) throw std::runtime_error(result.Error().message + " " + result.Error().platform_code);
  if constexpr (!std::is_void_v<T>) return std::move(result).Value();
}

template<class Test> void Wait(Test test, const char* description) {
  const auto deadline = std::chrono::steady_clock::now() + 6s;
  while (std::chrono::steady_clock::now() < deadline) {
    if (test()) return;
    std::this_thread::sleep_for(3ms);
  }
  throw std::runtime_error(description);
}

template<class Session> struct RetireSession {
  Session& session;
  const std::atomic_int& result;
  ~RetireSession() {
    session.Close();
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!result.load() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(3ms);
  }
};

PlaybackSnapshot Read(const DecodedPlayback& playback) {
  auto snapshot = playback.Snapshot();
  if (snapshot.error) throw std::runtime_error(snapshot.error->message + " " + snapshot.error->platform_code);
  return snapshot;
}

AudioFrame Tone(AudioFormat format, std::int64_t pts, std::uint32_t count) {
  auto samples = std::make_shared<std::vector<std::int16_t>>(count * 2);
  for (std::uint32_t index = 0; index < count; ++index) {
    const auto sample = std::int16_t(950 * std::sin(double(pts + index) * 6.283185307179586 * 440 / format.sample_rate));
    (*samples)[index * 2] = (*samples)[index * 2 + 1] = sample;
  }
  return {format, {pts, format.sample_rate}, count, {BufferReference(std::as_bytes(std::span(*samples)), samples)}};
}

BufferReference ReadBytes(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  Check(bool(file), "Cannot open AAC fixture");
  auto bytes = std::make_shared<Bytes>(std::size_t(file.tellg()));
  file.seekg(0);
  file.read(reinterpret_cast<char*>(bytes->data()), std::streamsize(bytes->size()));
  Check(bool(file), "Cannot read AAC fixture");
  return {*bytes, bytes};
}

void WaitState(const DecodedPlayback& playback, PlaybackState state, const char* description) {
  Wait([&] { return Read(playback).state == state; }, description);
}

void Synthetic(DecodedPlayback& playback, AudioFormat format) {
  const auto frame = Tone(format, 0, 12000);
  Check(Require(playback.TrySubmitAudio(frame)) == SubmitStatus::Accepted, "Full-capacity PCM rejected");
  Check(Require(playback.TrySubmitAudio(Tone(format, 12000, 1))) == SubmitStatus::WouldBlock,
        "Queue did not apply backpressure");
  Check(Read(playback).position->ticks == 0, "Submitting PCM moved the clock");
  Require(playback.Play());
  Wait([&] {
    const auto snapshot = Read(playback);
    return snapshot.position && snapshot.position->ticks > 0;
  }, "Device clock did not advance");
  Require(playback.Pause());
  WaitState(playback, PlaybackState::Paused, "Pause did not take effect");
  const auto paused = Read(playback).position;
  std::this_thread::sleep_for(65ms);
  Check(Read(playback).position == paused, "Paused clock advanced");

  Require(playback.Play());
  Wait([&] {
    const auto snapshot = Read(playback);
    return snapshot.state == PlaybackState::Buffering && snapshot.underruns == 1;
  }, "Device did not report starvation after the last committed sample");
  Check(Read(playback).position->ticks == 12000, "Silence changed the media position");
  std::this_thread::sleep_for(50ms);
  Check(Read(playback).position->ticks == 12000, "Starved clock advanced");

  Check(Require(playback.TrySubmitAudio(Tone(format, 12000, 1024))) == SubmitStatus::Accepted, "Short refill rejected");
  Require(playback.FinishAudio());
  WaitState(playback, PlaybackState::Ended, "Short sealed refill did not drain");
  Check(Read(playback).position->ticks == 13024 && Read(playback).buffered_duration.ticks == 0,
        "Short drain reported an incorrect final position");
  Require(playback.Reset());
  WaitState(playback, PlaybackState::Paused, "Reset did not reopen the device");
  Check(!Read(playback).position && Read(playback).underruns == 0, "Old timeline survived Reset");
  std::cout << "Synthetic PCM: capacity, pause, underrun, refill and short tail passed\n";
}

void DecodeAndPlay(DecodedPlayback& playback, AudioFormat format, const std::filesystem::path& root) {
  AudioDecoderConfig config;
  auto init = ReadBytes(root / "aac.config");
  config.initialization_data.assign(init.AsBytes().begin(), init.AsBytes().end());
  config.output = format;
  config.limits.input_items = 2;
  config.limits.output_items = 4;
  config.limits.output_bytes = 4 * 1024 * 2 * 2;
  auto decoder_cleaned = std::make_shared<std::atomic_int>(0);
  config.cleanup_completed = [decoder_cleaned](CodecResult<void> result) { *decoder_cleaned = result.Succeeded() ? 1 : -1; };
  auto decoder = Require(AudioDecoder::Create(config));
  RetireSession retire{decoder, *decoder_cleaned};
  std::vector<EncodedPacket> packets;
  std::ifstream manifest(root / "aac.txt");
  std::string filename;
  std::int64_t pts, dts, duration;
  std::uint32_t scale;
  bool key;
  while (manifest >> filename >> pts >> dts >> duration >> scale >> key) {
    packets.push_back({ReadBytes(root / filename), {pts, scale}, MediaTimestamp{dts, scale},
                       MediaTimestamp{duration, scale}, key});
  }
  Check(packets.size() == 13, "Invalid AAC fixture manifest");

  Require(playback.Play());
  std::size_t next = 0, submitted = 0;
  bool decoder_finished = false, playback_finished = false;
  std::optional<AudioFrame> pending;
  std::optional<MediaTimestamp> last;
  Wait([&] {
    while (next < packets.size() && Require(decoder.TrySubmit(packets[next])) == SubmitStatus::Accepted) ++next;
    if (next == packets.size() && !decoder_finished) {
      Require(decoder.Finish());
      decoder_finished = true;
    }
    while (!playback_finished) {
      if (pending) {
        if (Require(playback.TrySubmitAudio(*pending)) == SubmitStatus::WouldBlock) break;
        pending.reset();
        ++submitted;
      }
      auto output = Require(decoder.TryRead());
      if (!output) break;
      if (auto* frame = std::get_if<AudioFrame>(&*output)) pending = std::move(*frame);
      else if (std::holds_alternative<EndOfStream>(*output)) {
        Require(playback.FinishAudio());
        playback_finished = true;
        decoder.Close();
      }
    }
    const auto snapshot = Read(playback);
    if (snapshot.position) {
      Check(!last || snapshot.position->Compare(*last).value() >= 0, "AAC playback clock regressed");
      last = snapshot.position;
    }
    return snapshot.state == PlaybackState::Ended;
  }, "AAC pipeline did not drain");
  Check(submitted == 13 && last && last->ticks == 12288, "AAC final position lost samples or the negative anchor");
  Wait([&] { return decoder_cleaned->load() != 0; }, "Decoder cleanup did not complete independently");
  Check(*decoder_cleaned == 1, "Decoder cleanup failed");
  std::cout << "AAC decode to PCM device: 13 frames, final position " << last->ticks << '/' << last->timescale << '\n';
}
} // namespace

int main(int argc, char** argv) {
  try {
    AudioPlaybackConfig config;
    config.format = {48000, AudioSampleFormat::S16, AudioPacking::Interleaved, AudioChannelLayout::Stereo};
    auto cleaned = std::make_shared<std::atomic<int>>(0);
    config.cleanup_completed = [cleaned](PlaybackResult<void> result) { *cleaned = result.Succeeded() ? 1 : -1; };
    auto playback = Require(DecodedPlayback::Create(config));
    RetireSession retire{playback, *cleaned};
    WaitState(playback, PlaybackState::Paused, "Playback device did not open");
    Synthetic(playback, config.format);
    DecodeAndPlay(playback, config.format, argc > 1 ? argv[1] : MEDIA_CODEC_FIXTURES);
    playback.Close();
    playback.Close();
    Check(playback.Snapshot().state == PlaybackState::Closed, "Logical close was delayed");
    Wait([&] { return cleaned->load() != 0; }, "Playback cleanup did not complete");
    Check(*cleaned == 1, "Playback cleanup failed");
    std::cout << "Playback cleanup passed\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
