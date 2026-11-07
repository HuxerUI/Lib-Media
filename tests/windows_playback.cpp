#include "decoded_playback/device.h"

#include <chrono>
#include <iostream>
#include <stdexcept>
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
  if (!result.Succeeded()) throw std::runtime_error(result.Error().message + " " + result.Error().platform_code);
  return std::move(result).Value();
}

void StarvedRun(AudioFormat format) {
  auto device = MakePlaybackDevice();
  Check(Require(device->Open(format, format.sample_rate / 4, {})), "WASAPI output did not open");
  const auto count = format.sample_rate / 25;
  const std::vector<std::int16_t> samples(count * format.ChannelCount());
  Check(Require(device->Write(samples)) == count, "Cannot prime the WASAPI endpoint");
  Check(Require(device->Observe(false)).presented == 0, "Queued PCM advanced the WASAPI clock");
  Check(Require(device->Start()), "WASAPI output did not start");

  std::this_thread::sleep_for(240ms);
  Check(Require(device->Write(samples)) == 0, "WASAPI accepted media after an untracked silence gap");
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  std::uint64_t presented = 0;
  for (;;) {
    const auto progress = Require(device->Observe(true));
    Check(!progress.accepts_media && progress.presented >= presented && progress.presented <= count,
          "WASAPI silence changed the media range");
    presented = progress.presented;
    if (presented == count) break;
    Check(std::chrono::steady_clock::now() < deadline, "WASAPI did not confirm the starved media tail");
    std::this_thread::sleep_for(3ms);
  }
  Check(Require(device->Pause()), "WASAPI output did not pause");
  std::this_thread::sleep_for(30ms);
  Check(Require(device->Observe(true)).presented == count, "A retired WASAPI range advanced");
  Check(Require(device->Close()) && Require(device->Close()), "WASAPI output did not close idempotently");
  std::cout << "WASAPI " << format.sample_rate << " Hz, " << format.ChannelCount()
            << " channel(s): device-confirmed tail and underrun retirement passed\n";
}
} // namespace

int main() {
  try {
    StarvedRun({48000, AudioSampleFormat::S16, AudioPacking::Interleaved, AudioChannelLayout::Stereo});
    StarvedRun({44100, AudioSampleFormat::S16, AudioPacking::Interleaved, AudioChannelLayout::Mono});
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
