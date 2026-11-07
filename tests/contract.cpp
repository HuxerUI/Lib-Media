#include <huxerui/media_player.h>

#include <cmath>
#include <deque>
#include <iostream>
#include <limits>

#include "player/state.h"
#include "player/video_geometry.h"

using namespace huxerui;
using namespace huxerui::media;
using namespace huxerui::media::detail;

namespace {
int assertions = 0;
void Check(bool condition, const char* message) {
  ++assertions;
  if (!condition)
    throw std::runtime_error(message);
}
template <class Error, class Function> void Throws(Function function) {
  try {
    function();
  } catch (const Error&) {
    ++assertions;
    return;
  }
  throw std::runtime_error("Expected exception was not thrown");
}

class FakePlayer final : public PlayerBackend {
public:
  std::uint64_t generation = 0;
  std::uint64_t seek_id = 0;
  int plays = 0, pauses = 0, clears = 0, seeks = 0;
  MediaTime target;
  PlayerSnapshot snapshot{
      .progress = {MediaTime(0), MediaTime(10), std::vector<MediaTimeRange>{{MediaTime(0), MediaTime(10)}}},
      .seekability = MediaSeekability::Seekable,
      .has_audio = true,
      .has_video = false};
  void Load(const MediaSource&, std::uint64_t value) override {
    generation = value;
  }
  void Clear() noexcept override {
    ++clears;
  }
  void Play() override {
    ++plays;
  }
  void Pause() override {
    ++pauses;
  }
  void Seek(MediaTime position, std::uint64_t id) override {
    target = position;
    seek_id = id;
    ++seeks;
  }
  void SetVolume(double, bool) override {}
  void Poll() override {
    Send(PlayerEventKind::Update);
  }
  void Send(PlayerEventKind kind) {
    emit({.generation = generation, .kind = kind, .snapshot = snapshot, .seek_id = seek_id});
  }
};

struct Fixture {
  std::deque<std::function<void()>> queue;
  std::shared_ptr<PlayerState> state =
      std::make_shared<PlayerState>([this](std::function<void()> callback) { queue.push_back(std::move(callback)); });
  MediaPlayer player = PlayerAccess::Create(state);
  std::shared_ptr<FakePlayer> backend = std::make_shared<FakePlayer>();
  void Drain() {
    int iterations = 0;
    while (!queue.empty()) {
      Check(++iterations < 1000, "Event queue did not converge");
      auto callback = std::move(queue.front());
      queue.pop_front();
      callback();
    }
  }
  void Load() {
    Check(player.Load(MediaSource(MediaHttpSource{"https://example.test/media.wav", {}})), "Load rejected");
  }
  void Ready() {
    Load();
    backend->Send(PlayerEventKind::Ready);
    Drain();
  }
  Fixture() {
    state->Attach(backend);
    Drain();
  }
  ~Fixture() {
    state->Detach();
  }
};
} // namespace

int main() {
  try {
    Throws<std::invalid_argument>([] { MediaSource(MediaHttpSource{"not a URL", {}}); });
    Throws<std::invalid_argument>([] { MediaSource(MediaHttpSource{"file:///tmp/a", {}}); });
    Throws<std::invalid_argument>(
        [] { MediaSource(MediaHttpSource{"https://example.test/a", {{"X-Test", "a\r\nb"}}}); });
    Throws<std::invalid_argument>([] { MediaSource(MediaHttpSource{"https://example.test/a", {{"bad name", "a"}}}); });
    {
      Fixture f;
      Check(f.player == f.player, "Player identity changed");
      Check(!f.player.Play(), "Empty player accepted Play");
      Check(!f.player.SeekTo(MediaTime(0)), "Empty player accepted Seek");
      Throws<std::invalid_argument>([&] { return f.player.SetVolume(1.1); });
      Throws<std::invalid_argument>(
          [&] { return f.player.SetVolume(std::numeric_limits<double>::quiet_NaN()); });
      Throws<std::invalid_argument>([&] { return f.player.SeekTo(MediaTime(-1)); });
      Throws<std::invalid_argument>([&] { return f.player.SeekTo(MediaTime(INFINITY)); });
      f.Load();
      Check(f.player.Play(), "Loading player rejected Play");
      Check(f.backend->plays == 0, "Playback started before readiness");
      Check(f.player.Pause(), "Loading player rejected Pause");
      f.backend->Send(PlayerEventKind::Ready);
      f.Drain();
      Check(f.backend->plays == 0, "Pause did not cancel play intention");
      Check(f.player.Snapshot().status == MediaPlayerStatus::Ready, "Load did not reach Ready");
      Check(f.player.Play(), "Ready player rejected Play");
      f.backend->Send(PlayerEventKind::Playing);
      f.Drain();
      Check(f.player.Snapshot().status == MediaPlayerStatus::Playing, "Platform playback did not update state");
    }
    {
      Fixture f;
      f.Ready();
      Check(f.player.SeekTo(MediaTime(3)), "Seek rejected");
      auto first = f.backend->seek_id;
      Check(f.player.SeekTo(MediaTime(5)), "Second seek rejected");
      Check(f.player.SeekTo(MediaTime(8)), "Third seek rejected");
      Check(f.backend->seeks == 1, "Intermediate seeks were not coalesced");
      f.backend->snapshot.progress.position = MediaTime(3);
      f.backend->Send(PlayerEventKind::Seeked);
      f.Drain();
      Check(f.backend->seeks == 2 && f.backend->target == MediaTime(8), "Latest seek target was lost");
      Check(f.player.Snapshot().is_seeking, "Intermediate seek reported completion");
      f.state->Receive({.generation = f.backend->generation,
                        .kind = PlayerEventKind::Seeked,
                        .snapshot = f.backend->snapshot,
                        .seek_id = first});
      Check(f.player.Snapshot().is_seeking, "Stale seek callback completed a newer seek");
      f.backend->snapshot.progress.position = MediaTime(8);
      f.backend->Send(PlayerEventKind::Seeked);
      f.Drain();
      Check(!f.player.Snapshot().is_seeking && f.player.Snapshot().progress.position == MediaTime(8),
            "Seek completion was not published");
      Check(f.player.SeekTo(MediaTime(20)) && f.backend->target == MediaTime(10), "Seek was not clamped to duration");
    }
    {
      Fixture f;
      f.Ready();
      int ended = 0;
      f.state->ended_handlers.emplace(1, [&] { ++ended; });
      Check(f.player.Play(), "Play rejected");
      f.backend->Send(PlayerEventKind::Ended);
      f.backend->Send(PlayerEventKind::Ended);
      f.Drain();
      Check(ended == 1, "Ended was duplicated");
      Check(f.player.Snapshot().status == MediaPlayerStatus::Ended, "Ended state missing");
      Check(f.player.Play() && f.backend->target == MediaTime(0), "Replay did not seek to start");
    }
    {
      Fixture f;
      f.Ready();
      int ended = 0;
      f.state->ended_handlers.emplace(1, [&] { ++ended; });
      Check(f.player.SetLooping(true) && f.player.Play(), "Loop setup rejected");
      f.backend->Send(PlayerEventKind::Ended);
      f.Drain();
      Check(ended == 0 && f.player.Snapshot().is_seeking, "Loop emitted Ended");
      f.backend->Send(PlayerEventKind::Seeked);
      f.Drain();
      Check(f.backend->plays == 2, "Loop did not resume");
    }
    {
      Fixture f;
      f.Ready();
      int errors = 0;
      f.state->error_handlers.emplace(1, [&](const MediaError& error) {
        Check(!error.fatal, "Unsupported loop was fatal");
        ++errors;
      });
      f.backend->snapshot.seekability = MediaSeekability::NotSeekable;
      f.backend->Send(PlayerEventKind::Update);
      f.Drain();
      Check(f.player.SetLooping(true), "Loop preference rejected");
      f.backend->Send(PlayerEventKind::Update);
      f.Drain();
      Check(errors == 1, "Unsupported loop warning duplicated");
      Check(!f.player.SeekTo(MediaTime(1)), "Nonseekable source accepted Seek");
    }
    {
      Fixture f;
      f.Ready();
      const auto old_generation = f.backend->generation;
      f.Load();
      f.state->Receive({.generation = old_generation, .kind = PlayerEventKind::Ended, .snapshot = f.backend->snapshot});
      Check(f.player.Snapshot().status == MediaPlayerStatus::Loading, "Old source callback changed the new source");
      int errors = 0;
      f.state->error_handlers.emplace(1, [&](const MediaError&) { ++errors; });
      const MediaError error{MediaErrorCode::Decode, "decode failed", {}, true};
      f.state->Receive({.generation = f.backend->generation, .kind = PlayerEventKind::Error, .error = error});
      f.state->Receive({.generation = f.backend->generation, .kind = PlayerEventKind::Error, .error = error});
      f.Drain();
      Check(errors == 1 && f.player.Snapshot().error == error, "Fatal error was not retained exactly once");
      Check(!f.player.Play(), "Failed source accepted implicit retry");
      Check(f.player.Clear(), "Clear rejected");
      Check(!f.player.Snapshot().error && f.player.Snapshot().status == MediaPlayerStatus::Empty,
            "Clear did not reset source state");
    }
    {
      Fixture f;
      f.Ready();
      int callbacks = 0;
      f.state->progress_handlers.emplace(1, [&](const MediaProgress& progress) {
        Check(progress == f.player.Snapshot().progress, "Callback and snapshot diverged");
        ++callbacks;
        Check(f.player.Clear(), "Clear from a progress callback was rejected");
      });
      int empty_callbacks = 0;
      f.state->progress_handlers.emplace(2, [&](const MediaProgress&) {
        Check(f.player.Snapshot().status == MediaPlayerStatus::Empty, "Stale listener ran after source replacement");
        ++empty_callbacks;
      });
      f.backend->snapshot.progress.position = MediaTime(2);
      f.backend->Send(PlayerEventKind::Update);
      f.backend->snapshot.progress.position = MediaTime(3);
      f.backend->Send(PlayerEventKind::Update);
      f.Drain();
      Check(callbacks == 2 && empty_callbacks == 1, "Progress did not coalesce or repeated Clear did not converge");
    }
    {
      Fixture f;
      f.Ready();
      Check(f.player.Play() && f.player.Play() && f.backend->plays == 1, "Repeated Play was not idempotent");
      f.state->progress_handlers.emplace(1, [&](const MediaProgress&) {
        Check(f.player.Pause(), "Pause from a progress callback was rejected");
      });
      f.backend->snapshot.progress.position = MediaTime(2);
      f.backend->Send(PlayerEventKind::Update);
      f.Drain();
      Check(f.backend->pauses == 1, "Repeated Pause did not converge");
    }
    {
      Fixture f;
      f.Ready();
      int ended = 0, errors = 0;
      f.state->ended_handlers.emplace(1, [&] { ++ended; });
      f.state->error_handlers.emplace(1, [&](const MediaError& error) {
        Check(!error.fatal && error.code == MediaErrorCode::SeekFailed, "Loop failure was misclassified");
        ++errors;
      });
      Check(f.player.SetLooping(true) && f.player.Play(), "Loop setup rejected");
      f.backend->Send(PlayerEventKind::Ended);
      f.Drain();
      f.state->Receive({.generation = f.backend->generation,
                        .kind = PlayerEventKind::Error,
                        .seek_id = f.backend->seek_id,
                        .error = {MediaErrorCode::SeekFailed, "seek failed", {}, false}});
      f.Drain();
      Check(ended == 1 && errors == 1 && f.player.Snapshot().status == MediaPlayerStatus::Ended &&
                !f.player.Snapshot().is_seeking,
            "Failed loop did not finish naturally");
    }
    {
      Fixture f;
      f.Ready();
      Check(f.player.SetVolume(0.4) && f.player.SetMuted(true) && f.player.SetLooping(true), "Settings rejected");
      f.Load();
      Check(f.player.Snapshot().volume == 0.4 && f.player.Snapshot().muted && f.player.Snapshot().looping,
            "Load discarded preferences");
      int binds = 0;
      f.state->Bind([&](const std::shared_ptr<PlayerBackend>& player) { binds += player ? 1 : -1; });
      Throws<std::logic_error>([&] { f.state->Bind([](const auto&) {}); });
      f.state->Unbind();
      Check(binds == 0 && f.player.IsConnected(), "Unbinding video destroyed playback");
      f.state->Detach();
      Check(!f.player.IsConnected() && !f.player.Play() && !f.player.Clear(), "Detached handle remained active");
    }
    Check(FitVideo({1920, 1080}, {100, 100}, {}).height == 56.25F, "Contain geometry is incorrect");
    {
      Fixture f;
      f.Ready();
      const auto old_emit = f.backend->emit;
      const auto old_generation = f.backend->generation;
      f.state->Detach();
      f.state->Attach(f.backend);
      f.Load();
      old_emit({.generation = old_generation, .kind = PlayerEventKind::Ended});
      f.Drain();
      Check(f.player.Snapshot().status == MediaPlayerStatus::Loading,
            "Detached adapter callback escaped its attachment epoch");
    }
    Check(FitVideo({1920, 1080}, {100, 100}, {.fit = ImageFit::Fill}).width == 100, "Fill geometry is incorrect");
    std::cout << "MediaPlayer contract tests passed (" << assertions << " checks)\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
