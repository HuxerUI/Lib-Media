#include "media_player_windows.h"

#include <array>
#include <chrono>
#include <deque>
#include <filesystem>
#include <iostream>

using namespace huxerui;
using namespace huxerui::media;
using namespace huxerui::media::detail;

namespace {

void Check(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}

void Geometry() {
  const auto regression = FitWindowsVideo({1560, 1124}, {2500, 390}, {});
  Check(regression.has_value(), "Regression rectangle missing");
  Check(regression->source.left == 0 && regression->source.top == 0 && regression->source.right == 1 &&
            regression->source.bottom == 1,
        "Contain introduced cropping at fractional pixel boundaries");
  Check(regression->destination.left == 979 && regression->destination.right == 1521,
        "Destination did not round the fractional letterbox symmetrically");
  const std::array fits{ImageFit::Contain, ImageFit::Cover, ImageFit::Fill, ImageFit::None, ImageFit::ScaleDown};
  const std::array horizontal{HorizontalAlignment::Start, HorizontalAlignment::Center, HorizontalAlignment::End};
  const std::array vertical{VerticalAlignment::Start, VerticalAlignment::Center, VerticalAlignment::End};
  const std::array sizes{Size{2500, 390}, Size{390, 2500}, Size{681, 391}, Size{1920, 1080}, Size{1, 1}};
  for (auto fit : fits) {
    for (auto x : horizontal) {
      for (auto y : vertical) {
        for (auto bounds : sizes) {
          const auto rect = FitWindowsVideo({1560, 1124}, bounds, {fit, x, y});
          Check(rect.has_value(), "Valid video rectangle unexpectedly empty");
          const auto& s = rect->source;
          const auto& d = rect->destination;
          Check(s.left >= 0 && s.top >= 0 && s.right <= 1 && s.bottom <= 1 && s.left < s.right && s.top < s.bottom,
                "Source rectangle escaped normalized bounds");
          Check(d.left >= 0 && d.top >= 0 && d.right <= bounds.width && d.bottom <= bounds.height && d.left < d.right &&
                    d.top < d.bottom,
                "Destination rectangle escaped output bounds");
        }
      }
    }
  }
  const auto cover = FitWindowsVideo({200, 100}, {100, 100}, {.fit = ImageFit::Cover});
  Check(cover && cover->source.left == 0.25F && cover->source.right == 0.75F,
        "Cover no longer crops the centered source");
  Check(!FitWindowsVideo({0, 100}, {100, 100}, {}), "Empty video accepted");
  Check(!FitWindowsVideo({100, 100}, {0, 100}, {}), "Empty output accepted");
  std::cout << "Windows video geometry regression passed (225 fit/alignment/size combinations)\n";
}

void Playback(const std::filesystem::path& file) {
  Check(std::filesystem::is_regular_file(file), "Video file does not exist");
  std::deque<std::function<void()>> queue;
  auto state =
      std::make_shared<PlayerState>([&](std::function<void()> callback) { queue.push_back(std::move(callback)); });
  auto player = PlayerAccess::Create(state);
  auto factory = CreateWindowsVideoFactory();
  HWND parent = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC", L"MediaPlayer video test", WS_POPUP,
                                -10000, -10000, 2500, 1000, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
  Check(parent != nullptr, "Test host window creation failed");
  std::shared_ptr<VideoWindow> view;
  bool bound = false;
  const auto cleanup = [&] {
    if (bound)
      factory.disconnect(*view, player);
    if (view)
      factory.dispose(*view);
    state->Detach();
    DestroyWindow(parent);
  };
  try {
    state->Attach(CreateWindowsPlayer());
    view = factory.create(parent, {}, {});
    factory.connect(*view, player);
    bound = true;
    SetWindowPos(factory.view(view), nullptr, 0, 0, 2500, 390, SWP_NOZORDER | SWP_NOACTIVATE);
    ShowWindow(parent, SW_SHOWNOACTIVATE);
    bool failed = false, ended = false;
    state->error_handlers.emplace(1, [&](const MediaError& error) {
      std::cerr << error.message << " (" << error.platform_code << ")\n";
      failed = true;
    });
    state->ended_handlers.emplace(1, [&] { ended = true; });
    Check(player.SetMuted(true) && player.Load(MediaSource(File(file.u8string()))) && player.Play(),
          "Video playback commands rejected");
    const std::array fits{ImageFit::Contain, ImageFit::Cover, ImageFit::Fill, ImageFit::None, ImageFit::ScaleDown};
    std::size_t rendered = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(25);
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
      // Render clears dirty only after both platform frame transfer and presentation succeed.
      if (!view->dirty && rendered < fits.size()) {
        std::cout << "Platform frame transfer/presentation passed for fit " << static_cast<int>(fits[rendered]) << '\n';
        ++rendered;
        if (rendered < fits.size()) {
          factory.update(*view, {.fit = fits[rendered]});
          SetWindowPos(factory.view(view), nullptr, 0, 0, rendered % 2 ? 681 : 2500, rendered % 2 ? 391 : 390,
                       SWP_NOZORDER | SWP_NOACTIVATE);
        }
      }
      MsgWaitForMultipleObjectsEx(0, nullptr, 16, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }
    Check(!failed && ended && rendered == fits.size() && player.Snapshot().has_video == true,
          "Platform video playback did not render every fit and finish naturally");
    cleanup();
    std::cout << "Windows platform video playback, resized output, and natural end passed\n";
  } catch (...) {
    cleanup();
    throw;
  }
}

} // namespace

int wmain(int argc, wchar_t** argv) {
  try {
    Geometry();
    if (argc > 1)
      Playback(std::filesystem::path(argv[1]));
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
