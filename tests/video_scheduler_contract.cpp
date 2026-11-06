#include "playback/video_scheduler.h"

#include <iostream>
#include <stdexcept>

using namespace huxerui;
using namespace huxerui::media;
using namespace huxerui::media::detail;
using namespace std::chrono_literals;

namespace {
void Check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

VideoFrame Frame(std::int64_t pts, std::int64_t duration = 40) {
  return {.pts = {pts, 1000}, .duration = MediaTimestamp{duration, 1000}};
}

struct Harness {
  VideoScheduler scheduler;
  VideoScheduler::Epoch epoch = scheduler.Reset(MediaTimestamp{0, 1000});
  std::vector<std::int64_t> published;
  std::chrono::milliseconds upload_time{};

  void Submit(std::int64_t pts, std::int64_t duration = 40) {
    Check(scheduler.Submit(epoch, Frame(pts, duration)), "Video submission failed");
  }

  VideoScheduler::Update Tick(std::int64_t now, std::int64_t audio_position, PlaybackState state, bool paused = false) {
    PlaybackSnapshot audio{.state = state, .position = MediaTimestamp{audio_position, 1000}};
    const auto tick = VideoScheduler::Clock::time_point{} + std::chrono::milliseconds(now);
    const auto update = scheduler.Tick(epoch, &audio, paused, tick);
    if (update.frame) {
      published.push_back(update.frame->pts.ticks);
      scheduler.ConfirmPublished(tick + upload_time);
    }
    return update;
  }
};

void AudioEndsFirst() {
  Harness h;
  h.Submit(0);
  h.Submit(40);
  h.Tick(0, 0, PlaybackState::Playing);
  Check(h.published.empty(), "The accepted PCM anchor started video");
  h.Tick(10, 10, PlaybackState::Playing);
  h.Tick(30, 30, PlaybackState::Buffering);
  const auto buffered = h.Tick(90, 30, PlaybackState::Buffering);
  Check(buffered.position == 30ms && h.published == std::vector<std::int64_t>{0}, "Underflow advanced video");

  const auto drained = h.Tick(100, 30, PlaybackState::Ended);
  Check(drained.position == 30ms && h.published.size() == 1, "Audio EOS jumped the video clock");
  const auto following = h.Tick(110, 30, PlaybackState::Ended);
  Check(following.position == 40ms && h.published.back() == 40, "Video did not follow the drained audio clock");
  h.Submit(80);
  Check(h.scheduler.Finish(h.epoch), "Video EOS rejected");
  Check(!h.Tick(150, 30, PlaybackState::Ended).ended && h.published.back() == 80, "Video tail was skipped");
  Check(!h.Tick(189, 30, PlaybackState::Ended).ended, "Ended before the video display interval");
  Check(h.Tick(190, 30, PlaybackState::Ended).ended, "Video tail did not finish");
}

void VideoEndsFirst() {
  Harness h;
  h.Submit(0);
  Check(h.scheduler.Finish(h.epoch), "Video EOS rejected");
  h.Tick(1, 1, PlaybackState::Playing);
  Check(!h.Tick(150, 150, PlaybackState::Playing).ended, "Video EOS ended active audio");
  Check(h.Tick(200, 200, PlaybackState::Ended).ended, "Drained audio did not end playback");
  Check(h.published == std::vector<std::int64_t>{0}, "The last video frame was republished while audio drained");
}

void PauseDuringDrain() {
  Harness h;
  h.Submit(80);
  Check(h.scheduler.Finish(h.epoch), "Video EOS rejected");
  h.Tick(80, 80, PlaybackState::Playing);
  Check(!h.Tick(90, 90, PlaybackState::Ended, true).ended, "Audio drain bypassed pause");
  const auto paused = h.Tick(300, 90, PlaybackState::Ended, true);
  Check(paused.position == 90ms && !paused.ended, "Paused tail used elapsed wall time");
  const auto resumed = h.Tick(350, 90, PlaybackState::Ended);
  Check(resumed.position == paused.position && !resumed.ended, "Resuming jumped the video clock");
  Check(!h.Tick(389, 90, PlaybackState::Ended).ended, "Pause shortened the final display interval");
  Check(h.Tick(390, 90, PlaybackState::Ended).ended && h.published.size() == 1, "Resumed tail did not finish");
}

void DropExpiredWithLookahead() {
  Harness h;
  h.Submit(0);
  h.Submit(40);
  auto late = Frame(80);
  Check(!h.scheduler.Submit(h.epoch, std::move(late)) && late.pts == MediaTimestamp{80, 1000},
        "The bounded queue consumed a third frame");
  const auto tick = h.Tick(90, 90, PlaybackState::Playing);
  Check(tick.dropped == 1 && h.published.empty(), "An unsealed last frame was dropped or published");
  Check(h.scheduler.Finish(h.epoch), "Video EOS rejected");
  Check(h.Tick(91, 90, PlaybackState::Ended).dropped == 0 && h.published == std::vector<std::int64_t>{40},
        "The expired final frame was not preserved");
  Check(!h.Tick(130, 90, PlaybackState::Ended).ended, "A late final frame was not held for its display interval");
  Check(h.Tick(131, 90, PlaybackState::Ended).ended, "The late final frame never finished");
}

void DisplayIntervalStartsAfterUpload() {
  Harness h;
  h.upload_time = 20ms;
  h.Submit(0);
  Check(h.scheduler.Finish(h.epoch), "Video EOS rejected");
  h.Tick(1, 1, PlaybackState::Ended);
  Check(!h.Tick(60, 1, PlaybackState::Ended).ended, "Upload time counted toward the display interval");
  Check(h.Tick(61, 1, PlaybackState::Ended).ended, "Full display interval after upload did not end playback");
}

void RetireOldEpoch() {
  Harness h;
  auto bytes = std::make_shared<Bytes>(4);
  const std::weak_ptr<Bytes> held = bytes;
  auto frame = Frame(0);
  frame.planes.push_back({BufferReference(*bytes, bytes), 2, 1});
  Check(h.scheduler.Submit(h.epoch, std::move(frame)), "Cannot queue retained frame");
  bytes.reset();
  Check(!held.expired(), "Queue did not retain its frame");

  const auto old_epoch = h.epoch;
  h.epoch = h.scheduler.Reset(MediaTimestamp{0, 1000});
  Check(held.expired(), "Reset did not immediately release queued frames");
  auto delayed = Frame(40);
  Check(!h.scheduler.Submit(old_epoch, std::move(delayed)) && !h.scheduler.Finish(old_epoch), "Old video input crossed Reset");
  h.Submit(0);
  PlaybackSnapshot old_audio{.state = PlaybackState::Ended, .position = MediaTimestamp{100, 1000}};
  const auto ignored = h.scheduler.Tick(old_epoch, &old_audio, false, VideoScheduler::Clock::time_point{} + 100ms);
  Check(!ignored.frame && !ignored.position && !ignored.ended, "Old audio observation crossed Reset");
  h.Tick(1, 1, PlaybackState::Playing);
  Check(h.published == std::vector<std::int64_t>{0}, "New epoch did not publish");
  h.Submit(40);
  h.scheduler.Close();
  h.scheduler.Close();
  Check(!h.scheduler.CanSubmit(h.epoch) && !h.scheduler.Finish(h.epoch), "Close did not retire video input");
  h.Tick(50, 50, PlaybackState::Playing);
  Check(h.published.size() == 1, "A retired session continued publishing");
}
} // namespace

int main() {
  try {
    AudioEndsFirst();
    VideoEndsFirst();
    PauseDuringDrain();
    DropExpiredWithLookahead();
    DisplayIntervalStartsAfterUpload();
    RetireOldEpoch();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
