#include <huxerui/media_player.h>
#include <windows.h>

#include <chrono>
#include <cmath>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>

#include "player/state.h"

namespace huxerui::media::detail {
std::shared_ptr<PlayerBackend> CreateWindowsPlayer();
}

using namespace huxerui::media;
using namespace huxerui::media::detail;

int main() {
  const auto file = std::filesystem::temp_directory_path() /
                    ("huxerui-media-smoke-" + std::to_string(GetCurrentProcessId()) + ".wav");
  try {
    {
      std::ofstream output(file, std::ios::binary);
      auto word = [&](std::uint32_t value, int bytes) {
        for (int i = 0; i < bytes; ++i)
          output.put(static_cast<char>((value >> (i * 8)) & 255));
      };
      constexpr int samples = 48000;
      output.write("RIFF", 4);
      word(36 + samples * 2, 4);
      output.write("WAVEfmt ", 8);
      word(16, 4);
      word(1, 2);
      word(1, 2);
      word(24000, 4);
      word(48000, 4);
      word(2, 2);
      word(16, 2);
      output.write("data", 4);
      word(samples * 2, 4);
      for (int i = 0; i < samples; ++i)
        word(static_cast<std::uint16_t>(static_cast<std::int16_t>(1000 * std::sin(i * 0.1))), 2);
    }
    std::deque<std::function<void()>> queue;
    auto state =
        std::make_shared<PlayerState>([&](std::function<void()> callback) { queue.push_back(std::move(callback)); });
    auto player = PlayerAccess::Create(state);
    state->Attach(CreateWindowsPlayer());
    bool ended = false;
    bool progressed = false;
    bool failed = false;
    bool sought = false;
    bool resumed = false;
    state->ended_handlers.emplace(1, [&] { ended = true; });
    state->error_handlers.emplace(1, [&](const MediaError& error) {
      std::cerr << error.message << " (" << error.platform_code << ")\n";
      failed = true;
    });
    state->progress_handlers.emplace(1, [&](const MediaProgress& progress) {
      if (progress.position.count() > 0.1)
        progressed = true;
    });
    if (!player.SetMuted(true) || !player.Load(MediaSource(huxerui::File(file.u8string()))) || !player.Play())
      throw std::runtime_error("Windows initial playback setup was rejected");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!ended && !failed && std::chrono::steady_clock::now() < deadline) {
      MSG message;
      while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
      }
      while (!queue.empty()) {
        auto callback = std::move(queue.front());
        queue.pop_front();
        callback();
      }
      state->Poll();
      const auto current = player.Snapshot();
      if (!sought && current.progress.position.count() > 0.15 && current.seekability == MediaSeekability::Seekable) {
        if (!player.Pause() || !player.SeekTo(MediaTime(0.5)))
          throw std::runtime_error("Windows pause/seek was rejected");
        sought = true;
      } else if (sought && !resumed && !current.is_seeking) {
        if (!player.Play())
          throw std::runtime_error("Windows resume was rejected");
        resumed = true;
      }
      MsgWaitForMultipleObjectsEx(0, nullptr, 20, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }
    const auto snapshot = player.Snapshot();
    state->Detach();
    std::filesystem::remove(file);
    if (failed || !ended || !progressed || !sought || !resumed || !snapshot.progress.duration || snapshot.has_audio != true)
      throw std::runtime_error("Windows audio playback smoke failed or timed out");
    std::cout << "Windows platform audio load/play/pause/seek/resume/progress/end/disposal passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::filesystem::remove(file);
    std::cerr << error.what() << '\n';
    return 1;
  }
}
