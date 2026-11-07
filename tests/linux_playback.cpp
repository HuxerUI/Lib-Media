#include "decoded_playback/device.h"

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

using namespace huxerui::media;
using namespace huxerui::media::detail;
using namespace std::chrono_literals;

namespace {
void Check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
template<class T> T Require(PlaybackResult<T> result) {
  if (!result.Succeeded()) throw std::runtime_error(result.Error().message + ": " + result.Error().platform_code);
  return std::move(result).Value();
}
template<class Test> void Wait(Test test, const char* message) {
  const auto deadline = std::chrono::steady_clock::now() + 7s;
  while (!test()) {
    Check(std::chrono::steady_clock::now() < deadline, message);
    std::this_thread::sleep_for(3ms);
  }
}

void PulseOutput(AudioFormat format) {
  auto device = MakePlaybackDevice();
  Wait([&] { return Require(device->Open(format, format.sample_rate / 4, {})); }, "PulseAudio did not open");
  const std::vector<std::int16_t> samples((format.sample_rate / 10 + 1) * format.ChannelCount());
  const auto count = Require(device->Write(samples));
  Check(count > 0 && count <= samples.size() / format.ChannelCount(), "PulseAudio rejected priming PCM");
  Check(Require(device->Observe(false)).presented == 0, "Queued PCM advanced PulseAudio time");
  Wait([&] { return Require(device->Start()); }, "PulseAudio did not acknowledge start");
  std::uint64_t presented = 0;
  Wait([&] {
    presented = Require(device->Observe(true)).presented;
    return presented > 0;
  }, "PulseAudio did not confirm presentation");
  Wait([&] { return Require(device->Pause()); }, "PulseAudio did not acknowledge pause");
  const auto paused = Require(device->Observe(true)).presented;
  std::this_thread::sleep_for(50ms);
  Check(Require(device->Observe(true)).presented == paused, "PulseAudio advanced while corked");
  Wait([&] { return Require(device->Start()); }, "PulseAudio did not acknowledge resume");
  Wait([&] {
    const auto progress = Require(device->Observe(true));
    Check(progress.presented >= presented && progress.presented <= count, "Invalid PulseAudio media position");
    presented = progress.presented;
    return presented == count && !progress.accepts_media;
  }, "PulseAudio did not drain the final media sample");
  Check(Require(device->Write(samples)) == 0, "PulseAudio accepted media across an underrun");
  Check(Require(device->Close()) && Require(device->Close()), "PulseAudio cleanup was not idempotent");
  std::cout << "PulseAudio " << format.sample_rate << " Hz, " << format.ChannelCount()
            << " channel(s): cork, presentation, drain and retirement passed\n";
}

void MissingServer() {
  auto device = MakePlaybackDevice();
  const AudioFormat format{48000, AudioSampleFormat::S16, AudioPacking::Interleaved, AudioChannelLayout::Stereo};
  Wait([&] {
    auto opened = device->Open(format, format.sample_rate / 4, {});
    if (!opened.Succeeded()) {
      Check(opened.Error().code == PlaybackErrorCode::Unsupported, "Missing PulseAudio server reported the wrong error");
      return true;
    }
    Check(!opened.Value(), "Unexpected PulseAudio server connection");
    return false;
  }, "Missing PulseAudio server did not fail");
  Check(Require(device->Close()) && Require(device->Close()), "Failed PulseAudio setup did not release its resources");
  std::cout << "Missing PulseAudio server: Unsupported and cleanup passed\n";
}
} // namespace

int main(int argc, char** argv) {
  try {
    if (argc > 1 && std::string_view(argv[1]) == "--missing-server") {
      MissingServer();
      return 0;
    }
    PulseOutput({48000, AudioSampleFormat::S16, AudioPacking::Interleaved, AudioChannelLayout::Stereo});
    PulseOutput({44100, AudioSampleFormat::S16, AudioPacking::Interleaved, AudioChannelLayout::Mono});
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
