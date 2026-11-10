#include <huxerui/media_codec.h>
#include <huxerui/media_playback.h>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
#if defined(__APPLE__) && TARGET_OS_IOS
#import <AVFoundation/AVFoundation.h>
#endif

#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <future>
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
  const auto epoch = playback.Epoch();
  const auto frame = Tone(format, 0, 12000);
  Check(Require(playback.TrySubmitAudio(epoch, frame)) == SubmitStatus::Accepted, "Full-capacity PCM rejected");
  Check(Require(playback.TrySubmitAudio(epoch, Tone(format, 12000, 1))) == SubmitStatus::WouldBlock,
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
    return snapshot.state == PlaybackState::Buffering && snapshot.audio_underruns == 1;
  }, "Device did not report starvation after the last committed sample");
  Check(Read(playback).position->ticks == 12000, "Silence changed the media position");
  std::this_thread::sleep_for(50ms);
  Check(Read(playback).position->ticks == 12000, "Starved clock advanced");

  Check(Require(playback.TrySubmitAudio(epoch, Tone(format, 12000, 1024))) == SubmitStatus::Accepted, "Short refill rejected");
  Require(playback.FinishAudio(epoch));
  WaitState(playback, PlaybackState::Ended, "Short sealed refill did not drain");
  Check(Read(playback).position->ticks == 13024 && Read(playback).audio_buffered_duration.ticks == 0,
        "Short drain reported an incorrect final position");
  Require(playback.Reset());
  WaitState(playback, PlaybackState::Paused, "Reset did not reopen the device");
  Check(!Read(playback).position && Read(playback).audio_underruns == 0, "Old timeline survived Reset");
  std::cout << "Synthetic PCM: capacity, pause, underrun, refill and short tail passed\n";
}

void DecodeAndPlay(DecodedPlayback& playback, AudioFormat format, const std::filesystem::path& root) {
  const auto epoch = playback.Epoch();
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
        if (Require(playback.TrySubmitAudio(epoch, *pending)) == SubmitStatus::WouldBlock) break;
        pending.reset();
        ++submitted;
      }
      auto output = Require(decoder.TryRead());
      if (!output) break;
      if (auto* frame = std::get_if<AudioFrame>(&*output)) pending = std::move(*frame);
      else if (std::holds_alternative<EndOfStream>(*output)) {
        Require(playback.FinishAudio(epoch));
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

VideoFrame Picture(std::int64_t pts, const std::shared_ptr<const Bytes>& bytes) {
  const BufferReference pixels(*bytes, bytes);
  return {{.width = 2, .height = 2, .visible = {0, 0, 2, 2}}, {pts, 1000}, MediaTimestamp{40, 1000},
          {{pixels.Slice(0, 4), 2, 1}, {pixels.Slice(4, 2), 2, 2}}};
}

void VideoOnly() {
  auto playback = Require(DecodedPlayback::Create({.video = true}));
  auto other = Require(DecodedPlayback::Create({.video = true}));
  const auto retired = playback.Epoch();
  auto bytes = std::make_shared<const Bytes>(6);
  const std::weak_ptr<const Bytes> held = bytes;
  Check(Require(playback.TrySubmitVideo(retired, Picture(0, bytes))) == SubmitStatus::Accepted, "Cannot submit video");
  bytes.reset();
  Check(!held.expired(), "Video input did not retain the plane owner");
  const auto epoch = Require(playback.Reset());
  Check(held.expired(), "Video reset retained old frame storage");
  Check(epoch == playback.Epoch(), "Reset did not return the installed epoch");
  bytes = std::make_shared<const Bytes>(6);
  Check(retired != epoch && !playback.TrySubmitVideo(retired, Picture(0, bytes)).Succeeded() &&
        !playback.TrySubmitVideo(other.Epoch(), Picture(0, bytes)).Succeeded() && !playback.FinishVideo(retired).Succeeded(),
        "A retired or foreign video producer crossed Reset");
  auto wrong = playback.PresentVideo(retired, [](const VideoFrame&) { throw std::runtime_error("Old frame presented"); });
  Check(!wrong.Succeeded() && wrong.Error().code == PlaybackErrorCode::InvalidState, "Old presentation crossed Reset");
  Check(Require(playback.TrySubmitVideo(epoch, Picture(-40, bytes))) == SubmitStatus::Accepted &&
        Require(playback.TrySubmitVideo(epoch, Picture(0, bytes))) == SubmitStatus::Accepted &&
        Require(playback.TrySubmitVideo(epoch, Picture(40, bytes))) == SubmitStatus::WouldBlock, "Video capacity was not bounded");
  Require(playback.FinishVideo(epoch));
  std::optional<VideoFrame> displayed;
  const auto present = [&](const VideoFrame& frame) { displayed = frame; };
  Check(!Require(playback.PresentVideo(epoch, present)).lag_ms, "Paused video published a frame");
  Require(playback.Play());
  Wait([&] {
    Require(playback.PresentVideo(epoch, present));
    return Read(playback).state == PlaybackState::Ended;
  }, "Video-only playback did not end");
  Check(displayed && displayed->pts == MediaTimestamp{0, 1000}, "Video-only playback lost its final frame");
  playback.Close();
  Check(displayed->Validate() && Read(playback).state == PlaybackState::Closed, "Close invalidated the published frame");
  std::cout << "Video-only playback: queue, epoch retirement and retained final frame passed\n";
}

void PublicationBarrier(bool close) {
  auto playback = Require(DecodedPlayback::Create({.video = true}));
  const auto epoch = playback.Epoch();
  auto pixels = std::make_shared<const Bytes>(6);
  Check(Require(playback.TrySubmitVideo(epoch, Picture(0, pixels))) == SubmitStatus::Accepted, "Cannot queue video");
  Require(playback.Play());
  std::promise<void> entered, release, retiring;
  auto can_finish = release.get_future();
  auto presenting = std::async(std::launch::async, [&] {
    return Require(playback.PresentVideo(epoch, [&](const VideoFrame& frame) {
      entered.set_value();
      can_finish.wait();
      Check(Read(playback).state == PlaybackState::Playing && frame.Validate(), "Admitted frame retired during upload");
    }));
  });
  entered.get_future().wait();
  auto retirement = std::async(std::launch::async, [&] {
    retiring.set_value();
    if (!close) return Require(playback.Reset());
    playback.Close();
    return PlaybackEpoch{};
  });
  retiring.get_future().wait();
  const bool waiting = retirement.wait_for(20ms) == std::future_status::timeout;
  const auto submitted = playback.TrySubmitVideo(epoch, Picture(40, pixels));
  release.set_value();
  const auto published = presenting.get();
  const auto next = retirement.get();
  Check(waiting && published.lag_ms && Require(submitted) == SubmitStatus::Accepted,
        "Upload blocked ordinary submission or failed to serialize retirement");
  Check(!playback.TrySubmitVideo(epoch, Picture(0, pixels)).Succeeded() && next == playback.Epoch(),
        "Old input crossed the publication barrier");
  std::cout << (close ? "Close" : "Reset") << " serialized with upload while state access continued\n";
}

void AudioAndVideo(AudioPlaybackConfig config) {
  auto cleaned = std::make_shared<std::atomic_int>(0);
  config.cleanup_completed = [cleaned](PlaybackResult<void> result) { *cleaned = result.Succeeded() ? 1 : -1; };
  auto playback = Require(DecodedPlayback::Create({.audio = config, .video = true}));
  RetireSession retire{playback, *cleaned};
  WaitState(playback, PlaybackState::Paused, "A/V device did not open");
  const auto retired = playback.Epoch();
  const auto epoch = Require(playback.Reset());
  WaitState(playback, PlaybackState::Paused, "A/V device did not reopen");
  auto audio = Tone(config.format, 0, 2400);
  Check(!playback.TrySubmitAudio(retired, audio).Succeeded() && !playback.FinishAudio(retired).Succeeded(),
        "An old audio producer crossed A/V Reset");
  Check(Require(playback.TrySubmitAudio(epoch, audio)) == SubmitStatus::Accepted, "Cannot submit A/V audio");
  Require(playback.FinishAudio(epoch));
  Require(playback.Play());
  std::optional<MediaTimestamp> shown;
  const auto present = [&](const VideoFrame& frame) { shown = frame.pts; };
  Check(Read(playback).state == PlaybackState::Buffering && Read(playback).position == MediaTimestamp{0, 48000},
        "Audio started before video was ready");
  auto bytes = std::make_shared<const Bytes>(6);
  Check(Require(playback.TrySubmitVideo(epoch, Picture(0, bytes))) == SubmitStatus::Accepted &&
        Require(playback.TrySubmitVideo(epoch, Picture(80, bytes))) == SubmitStatus::Accepted, "Cannot queue A/V video tail");
  Require(playback.FinishVideo(epoch));
  Wait([&] {
    const auto snapshot = Read(playback);
    return snapshot.position && snapshot.position->ticks > 0;
  }, "Ready audio did not start before the first PresentVideo call");
  Wait([&] {
    Require(playback.PresentVideo(epoch, present));
    return Read(playback).state == PlaybackState::Ended;
  }, "A/V playback did not drain its video tail");
  const auto ended = Read(playback);
  Check(shown == MediaTimestamp{80, 1000} && ended.position && ended.position->Compare({120, 1000}) >= 0,
        "A/V ended before the final video interval");
  playback.Close();
  Check(Read(playback).state == PlaybackState::Closed, "A/V close was not logical immediately");
  Wait([&] { return *cleaned != 0; }, "A/V device cleanup did not complete");
  Check(*cleaned == 1, "A/V device cleanup failed");
  std::cout << "A/V playback: gated audio, stale producer, video tail and device cleanup passed\n";
}

#if defined(__APPLE__) && TARGET_OS_IOS
void ActivateIosAudio() {
  @autoreleasepool {
    auto* session = AVAudioSession.sharedInstance;
    NSError* error = nil;
    if (![session setCategory:AVAudioSessionCategoryPlayback error:&error] || ![session setActive:YES error:&error]) {
      const auto* message = error.localizedDescription.UTF8String;
      throw std::runtime_error(message ? message : "Cannot activate the iOS test audio session");
    }
  }
}

void IosOutputLoss(AudioPlaybackConfig config, NSNotificationName name, NSDictionary* info, const char* expected,
                   bool paused) {
  @autoreleasepool {
    auto cleaned = std::make_shared<std::atomic_int>(0);
    config.cleanup_completed = [cleaned](PlaybackResult<void> result) { *cleaned = result.Succeeded() ? 1 : -1; };
    auto playback = Require(DecodedPlayback::Create({.audio = config}));
    RetireSession retire{playback, *cleaned};
    WaitState(playback, PlaybackState::Paused, "iOS device did not open before the notification");
    const auto epoch = playback.Epoch();
    Check(Require(playback.TrySubmitAudio(epoch, Tone(config.format, 0, 12000))) == SubmitStatus::Accepted,
          "Cannot queue iOS notification test audio");
    Require(playback.Play());
    WaitState(playback, PlaybackState::Playing, "iOS notification test audio did not start");
    if (paused) {
      Require(playback.Pause());
      WaitState(playback, PlaybackState::Paused, "iOS notification test audio did not pause");
    }
    const auto position = playback.Snapshot().position;
    [NSNotificationCenter.defaultCenter postNotificationName:name object:AVAudioSession.sharedInstance userInfo:info];
    Wait([&] { return playback.Snapshot().state == PlaybackState::Failed; }, "iOS audio loss did not fail playback");
    const auto failure = playback.Snapshot();
    Check(failure.error && failure.error->code == PlaybackErrorCode::DeviceFailure &&
          failure.error->message.find(expected) != std::string::npos, "Unexpected iOS audio loss error");
    Wait([&] { return *cleaned != 0; }, "iOS audio loss did not retire the device");
    Check(*cleaned == 1 && playback.Snapshot().position == failure.position, "iOS audio loss advanced a retired clock");
    if (paused) Check(failure.position == position, "iOS audio loss advanced the paused clock");
  }
}
#endif
} // namespace

int main(int argc, char** argv) {
  try {
#if defined(__APPLE__) && TARGET_OS_IOS
    ActivateIosAudio();
#endif
    AudioPlaybackConfig config;
    config.format = {48000, AudioSampleFormat::S16, AudioPacking::Interleaved, huxerui::media::AudioChannelLayout::Stereo};
    auto cleaned = std::make_shared<std::atomic<int>>(0);
    config.cleanup_completed = [cleaned](PlaybackResult<void> result) { *cleaned = result.Succeeded() ? 1 : -1; };
    auto playback = Require(DecodedPlayback::Create({.audio = config}));
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
    VideoOnly();
    PublicationBarrier(false);
    PublicationBarrier(true);
    AudioAndVideo(config);
#if defined(__APPLE__) && TARGET_OS_IOS
    for (bool paused : {false, true}) {
      IosOutputLoss(config, AVAudioSessionInterruptionNotification,
                    @{AVAudioSessionInterruptionTypeKey: @(AVAudioSessionInterruptionTypeBegan)}, "interrupted", paused);
      IosOutputLoss(config, AVAudioSessionRouteChangeNotification,
                    @{AVAudioSessionRouteChangeReasonKey: @(AVAudioSessionRouteChangeReasonOldDeviceUnavailable)},
                    "route changed", paused);
    }
    std::cout << "iOS interruption and output-route invalidation passed in Playing and Paused\n";
#endif
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
