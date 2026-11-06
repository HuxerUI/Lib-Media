#include "playback/device.h"

#include <emscripten/val.h>

namespace huxerui::media::detail {
namespace {
using emscripten::val;

class WebPlaybackDevice final : public PlaybackDevice {
public:
  PlaybackResult<bool> Open(const AudioFormat& format, std::size_t capacity, std::function<void()>) override {
    if (state_.isUndefined()) {
      state_ = bridge_.call<val>("open", format.sample_rate, format.ChannelCount(), capacity);
    }
    return Checked(state_["ready"].as<bool>(), PlaybackErrorCode::Unsupported);
  }

  PlaybackResult<std::size_t> Write(std::span<const std::int16_t> samples) override {
    const auto view = val(emscripten::typed_memory_view(samples.size(), samples.data()));
    return Checked(bridge_.call<std::size_t>("write", state_, view));
  }

  PlaybackResult<bool> Start() override { return Checked(bridge_.call<bool>("transport", state_, true)); }
  PlaybackResult<bool> Pause() override { return Checked(bridge_.call<bool>("transport", state_, false)); }

  PlaybackResult<PlaybackProgress> Observe(bool) override {
    const auto count = bridge_.call<double>("observe", state_);
    return Checked(PlaybackProgress{std::uint64_t(count)});
  }

  PlaybackResult<bool> Close() override {
    if (state_.isUndefined()) return PlaybackResult<bool>::Success(true);
    if (!bridge_.call<bool>("close", state_)) return PlaybackResult<bool>::Success(false);
    auto error = state_["closeError"].as<std::string>();
    state_ = val::undefined();
    if (!error.empty()) return PlaybackFailure<bool>(PlaybackErrorCode::DeviceFailure, std::move(error), "AudioContext.close");
    return PlaybackResult<bool>::Success(true);
  }

private:
  template<class T> PlaybackResult<T> Checked(T value, PlaybackErrorCode code = PlaybackErrorCode::DeviceFailure) {
    auto error = bridge_.call<std::string>("error", state_);
    if (!error.empty()) return PlaybackFailure<T>(code, std::move(error), "Web Audio");
    return PlaybackResult<T>::Success(value);
  }

  val bridge_ = val::global("HuxerMediaPlayback");
  val state_ = val::undefined();
};
} // namespace

std::unique_ptr<PlaybackDevice> MakePlaybackDevice() { return std::make_unique<WebPlaybackDevice>(); }
} // namespace huxerui::media::detail
