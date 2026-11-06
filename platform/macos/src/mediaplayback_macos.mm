#include "playback/device.h"

#import <AVFoundation/AVFoundation.h>

#include <algorithm>
#include <atomic>
#include <deque>

namespace huxerui::media::detail {
namespace {

class MacPlaybackDevice final : public PlaybackDevice {
public:
  PlaybackResult<void> Open(const AudioFormat& format, std::size_t capacity, std::function<void()>) override {
    @autoreleasepool {
      channels_ = format.ChannelCount();
      quantum_ = std::max(1u, format.sample_rate / 50);
      capacity_ = capacity;
      engine_ = [[AVAudioEngine alloc] init];
      player_ = [[AVAudioPlayerNode alloc] init];
      format_ = [[AVAudioFormat alloc] initStandardFormatWithSampleRate:format.sample_rate channels:channels_];
      if (!engine_ || !player_ || !format_)
        return PlaybackFailure(PlaybackErrorCode::Unsupported, "Cannot create the macOS PCM format or engine");
      auto changed = std::make_shared<std::atomic_bool>(false);
      changed_ = changed;
      observer_ = [NSNotificationCenter.defaultCenter addObserverForName:AVAudioEngineConfigurationChangeNotification
          object:engine_ queue:nil usingBlock:^(NSNotification*) { changed->store(true); }];
      [engine_ attachNode:player_];
      [engine_ connect:player_ to:engine_.mainMixerNode format:format_];
      NSError* error = nil;
      if (![engine_ startAndReturnError:&error]) {
        const char* description = error.localizedDescription.UTF8String;
        return PlaybackFailure(PlaybackErrorCode::DeviceFailure, description ? description : "Cannot start the macOS audio engine",
                               std::to_string(error.code));
      }
      return PlaybackResult<void>::Success();
    }
  }

  PlaybackResult<std::size_t> Write(std::span<const std::int16_t> samples) override {
    @autoreleasepool {
      using R = PlaybackResult<std::size_t>;
      if (!Valid()) return R::Failure(RouteError());
      const auto frames = std::min({samples.size() / channels_, quantum_, capacity_ - std::size_t(written_ - presented_)});
      if (!frames) return R::Success(0);
      AVAudioPCMBuffer* buffer = [[AVAudioPCMBuffer alloc] initWithPCMFormat:format_ frameCapacity:AVAudioFrameCount(frames)];
      if (!buffer || !buffer.floatChannelData)
        return PlaybackFailure<std::size_t>(PlaybackErrorCode::ResourceExhausted, "Cannot allocate a macOS PCM buffer");
      buffer.frameLength = AVAudioFrameCount(frames);
      for (std::size_t channel = 0; channel < channels_; ++channel) {
        auto* plane = buffer.floatChannelData[channel];
        for (std::size_t index = 0; index < frames; ++index)
          plane[index] = samples[index * channels_ + channel] / 32768.f;
      }

      // The callback owns a ticket for this buffer. It cannot enter a retired session.
      auto played = std::make_shared<std::atomic_bool>(false);
      pending_.push_back({written_ + frames, played, buffer});
      [player_ scheduleBuffer:buffer completionCallbackType:AVAudioPlayerNodeCompletionDataPlayedBack
          completionHandler:^(AVAudioPlayerNodeCompletionCallbackType type) {
            if (type == AVAudioPlayerNodeCompletionDataPlayedBack) played->store(true);
          }];
      written_ += frames;
      return R::Success(frames);
    }
  }

  PlaybackResult<void> Start() override {
    @autoreleasepool {
      if (!Valid()) return PlaybackResult<void>::Failure(RouteError());
      [player_ play];
      return PlaybackResult<void>::Success();
    }
  }

  PlaybackResult<void> Pause() override {
    @autoreleasepool {
      if (!Valid()) return PlaybackResult<void>::Failure(RouteError());
      [player_ pause];
      return PlaybackResult<void>::Success();
    }
  }

  PlaybackResult<PlaybackProgress> Observe(bool) override {
    @autoreleasepool {
      while (!pending_.empty() && pending_.front().played->load()) {
        presented_ = pending_.front().end;
        pending_.pop_front();
      }
      if (!Valid()) return PlaybackResult<PlaybackProgress>::Failure(RouteError());
      return PlaybackResult<PlaybackProgress>::Success({presented_});
    }
  }

  PlaybackResult<void> Close() override {
    @autoreleasepool {
      if (observer_) [NSNotificationCenter.defaultCenter removeObserver:observer_];
      observer_ = nil;
      [player_ stop];
      [engine_ stop];
      pending_.clear();
      player_ = nil;
      engine_ = nil;
      format_ = nil;
      changed_.reset();
      return PlaybackResult<void>::Success();
    }
  }

private:
  bool Valid() const { return engine_.isRunning && changed_ && !changed_->load(); }
  static PlaybackError RouteError() {
    return {PlaybackErrorCode::DeviceFailure, "The macOS audio engine or output configuration changed", {}};
  }

  struct Buffer {
    std::uint64_t end;
    std::shared_ptr<std::atomic_bool> played;
    AVAudioPCMBuffer* storage;
  };

  AVAudioEngine* engine_ = nil;
  AVAudioPlayerNode* player_ = nil;
  AVAudioFormat* format_ = nil;
  id observer_ = nil;
  std::shared_ptr<std::atomic_bool> changed_;
  std::deque<Buffer> pending_;
  std::size_t channels_ = 0, quantum_ = 0, capacity_ = 0;
  std::uint64_t written_ = 0, presented_ = 0;
};
} // namespace

std::unique_ptr<PlaybackDevice> MakePlaybackDevice() { return std::make_unique<MacPlaybackDevice>(); }
} // namespace huxerui::media::detail
