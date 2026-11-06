#include <huxerui/media_playback.h>

#include <cmath>
#include <emscripten/bind.h>
#include <stdexcept>

using namespace huxerui;
using namespace huxerui::media;
using emscripten::val;

namespace {
constexpr AudioFormat format{48000, AudioSampleFormat::S16, AudioPacking::Interleaved, AudioChannelLayout::Stereo};

template<class T> T Require(PlaybackResult<T> result) {
  if (!result.Succeeded()) throw std::runtime_error(result.Error().message);
  if constexpr (!std::is_void_v<T>) return std::move(result).Value();
}

const char* StateName(PlaybackState state) {
  switch (state) {
    case PlaybackState::Starting: return "Starting";
    case PlaybackState::Paused: return "Paused";
    case PlaybackState::Buffering: return "Buffering";
    case PlaybackState::Playing: return "Playing";
    case PlaybackState::Ended: return "Ended";
    case PlaybackState::Resetting: return "Resetting";
    case PlaybackState::Failed: return "Failed";
    case PlaybackState::Closed: return "Closed";
  }
  return "Unknown";
}

class PlaybackSample {
public:
  explicit PlaybackSample(bool video) : playback_(Open(cleaned_, video)), epoch_(playback_.Epoch()) {}

  bool Submit(int pts, unsigned count) { return Require(playback_.TrySubmitAudio(epoch_, Tone(pts, count))) == SubmitStatus::Accepted; }
  bool RejectsRetiredEpoch() {
    const auto result = playback_.TrySubmitAudio(retired_, Tone(0, 1));
    return !result.Succeeded() && result.Error().code == PlaybackErrorCode::InvalidState;
  }
  void Play() { Require(playback_.Play()); }
  void Pause() { Require(playback_.Pause()); }
  void Finish() { Require(playback_.FinishAudio(epoch_)); }
  bool SubmitVideo(int pts, unsigned duration) {
    auto pixels = std::make_shared<Bytes>(6, std::byte{128});
    const BufferReference data(*pixels, pixels);
    VideoFrame frame;
    frame.format.width = frame.format.height = 2;
    frame.format.visible = {0, 0, 2, 2};
    frame.format.pixel_format = VideoPixelFormat::I420;
    frame.pts = {pts, format.sample_rate};
    frame.duration = MediaTimestamp{duration, format.sample_rate};
    frame.planes = {{data.Slice(0, 4), 2, 1}, {data.Slice(4, 1), 1, 1}, {data.Slice(5, 1), 1, 1}};
    return Require(playback_.TrySubmitVideo(epoch_, frame)) == SubmitStatus::Accepted;
  }
  void FinishVideo() { Require(playback_.FinishVideo(epoch_)); }
  val PresentVideo() {
    auto value = val::object();
    value.set("pts", val::null());
    const auto update = Require(playback_.PresentVideo(epoch_, [&](const VideoFrame& frame) {
      value.set("pts", double(frame.pts.ticks));
    }));
    value.set("dropped", double(update.dropped));
    return value;
  }
  void Reset() {
    retired_ = epoch_;
    epoch_ = Require(playback_.Reset());
  }
  void Close() { playback_.Close(); }
  int Cleaned() const { return *cleaned_; }
  val Snapshot() const {
    const auto snapshot = playback_.Snapshot();
    auto value = val::object();
    value.set("state", StateName(snapshot.state));
    const auto position = snapshot.position ? snapshot.position->Rescale(format.sample_rate) : std::nullopt;
    value.set("position", position ? val(double(position->ticks)) : val::null());
    value.set("buffered", double(snapshot.audio_buffered_duration.ticks));
    value.set("underruns", double(snapshot.audio_underruns));
    value.set("error", snapshot.error ? snapshot.error->message : "");
    return value;
  }

private:
  static DecodedPlayback Open(const std::shared_ptr<int>& cleaned, bool video) {
    AudioPlaybackConfig config;
    config.format = format;
    config.cleanup_completed = [cleaned](PlaybackResult<void> result) { *cleaned += result.Succeeded() ? 1 : -1; };
    return Require(DecodedPlayback::Create({.audio = config, .video = video}));
  }

  static AudioFrame Tone(int pts, unsigned count) {
    auto samples = std::make_shared<std::vector<std::int16_t>>(count * format.ChannelCount());
    for (unsigned frame = 0; frame < count; ++frame) {
      const auto phase = (double(pts) + frame) * 6.283185307179586 * 440 / format.sample_rate;
      (*samples)[frame * 2] = (*samples)[frame * 2 + 1] = std::int16_t(1800 * std::sin(phase));
    }
    return {format, {pts, format.sample_rate}, count, {BufferReference(std::as_bytes(std::span(*samples)), samples)}};
  }

  std::shared_ptr<int> cleaned_ = std::make_shared<int>();
  DecodedPlayback playback_;
  PlaybackEpoch epoch_, retired_;
};
} // namespace

EMSCRIPTEN_BINDINGS(media_playback_sample) {
  emscripten::class_<PlaybackSample>("PlaybackSample")
      .constructor<bool>()
      .function("submit", &PlaybackSample::Submit)
      .function("rejectsRetiredEpoch", &PlaybackSample::RejectsRetiredEpoch)
      .function("play", &PlaybackSample::Play)
      .function("pause", &PlaybackSample::Pause)
      .function("finish", &PlaybackSample::Finish)
      .function("submitVideo", &PlaybackSample::SubmitVideo)
      .function("finishVideo", &PlaybackSample::FinishVideo)
      .function("presentVideo", &PlaybackSample::PresentVideo)
      .function("reset", &PlaybackSample::Reset)
      .function("close", &PlaybackSample::Close)
      .function("cleaned", &PlaybackSample::Cleaned)
      .function("snapshot", &PlaybackSample::Snapshot);
}

int main() {}
