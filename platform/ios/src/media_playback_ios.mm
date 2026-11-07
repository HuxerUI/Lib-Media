#include "decoded_playback/device.h"

#import <AVFoundation/AVFoundation.h>

#include <algorithm>
#include <atomic>
#include <deque>

namespace huxerui::media::detail {
namespace {

enum class OutputLoss { None, Interruption, Route, Configuration, MediaServices, Retired };

struct RunStatus {
  std::atomic<OutputLoss> loss{OutputLoss::None};
  std::function<void()> wakeup;
};

void Invalidate(const std::shared_ptr<RunStatus>& status, OutputLoss reason) {
  auto valid = OutputLoss::None;
  if (status->loss.compare_exchange_strong(valid, reason)) status->wakeup();
}

bool SameOutputs(NSArray<AVAudioSessionPortDescription*>* before, NSArray<AVAudioSessionPortDescription*>* after) {
  if (before.count != after.count) return false;
  for (NSUInteger index = 0; index < before.count; ++index) {
    if (![before[index].UID isEqualToString:after[index].UID]) return false;
  }
  return true;
}

PlaybackError OutputError(OutputLoss reason) {
  const char* message = "The iOS audio engine is no longer running";
  switch (reason) {
    case OutputLoss::Interruption: message = "The iOS audio session was interrupted"; break;
    case OutputLoss::Route: message = "The iOS audio output route changed"; break;
    case OutputLoss::Configuration: message = "The iOS audio engine configuration changed"; break;
    case OutputLoss::MediaServices: message = "The iOS audio media services were invalidated"; break;
    case OutputLoss::None:
    case OutputLoss::Retired: break;
  }
  return {PlaybackErrorCode::DeviceFailure, message, {}};
}

PlaybackError AudioEngineError(NSException* error) {
  const auto* message = error.reason.UTF8String;
  const auto* name = error.name.UTF8String;
  return {PlaybackErrorCode::DeviceFailure, message ? message : "The iOS audio engine failed", name ? name : ""};
}

class IosPlaybackDevice final : public PlaybackDevice {
public:
  PlaybackResult<bool> Open(const AudioFormat& format, std::size_t capacity,
                            std::function<void()> output_lost) override {
    @autoreleasepool {
      @try {
        auto* session = AVAudioSession.sharedInstance;
        if (!session.currentRoute.outputs.count)
          return PlaybackFailure<bool>(PlaybackErrorCode::DeviceFailure, "Activate an iOS audio session with an output route");
        engine_ = [[AVAudioEngine alloc] init];
        player_ = [[AVAudioPlayerNode alloc] init];
        format_ = [[AVAudioFormat alloc] initStandardFormatWithSampleRate:format.sample_rate
                                                                channels:format.ChannelCount()];
        if (!engine_ || !player_ || !format_)
          return PlaybackFailure<bool>(PlaybackErrorCode::Unsupported, "Cannot configure iOS PCM output");

        status_ = std::make_shared<RunStatus>();
        status_->wakeup = std::move(output_lost);
        WatchOutput(session);
        [engine_ attachNode:player_];
        [engine_ connect:player_ to:engine_.mainMixerNode format:format_];
        NSError* error = nil;
        if (![engine_ startAndReturnError:&error]) {
          const auto* message = error.localizedDescription.UTF8String;
          return PlaybackFailure<bool>(PlaybackErrorCode::DeviceFailure, message ? message : "Cannot start the iOS audio engine",
                                 std::to_string(error.code));
        }
        capacity_ = capacity;
        quantum_ = std::max(1u, format.sample_rate / 50);
        return CheckOutput();
      } @catch (NSException* error) {
        return PlaybackResult<bool>::Failure(AudioEngineError(error));
      }
    }
  }

  PlaybackResult<std::size_t> Write(std::span<const std::int16_t> samples) override {
    @autoreleasepool {
      using R = PlaybackResult<std::size_t>;
      auto output = CheckOutput();
      if (!output.Succeeded()) return R::Failure(std::move(output).Error());
      const auto channels = format_.channelCount;
      const auto count = std::min({samples.size() / channels, quantum_, capacity_ - std::size_t(written_ - presented_)});
      if (!count) return R::Success(0);
      @try {
        auto* buffer = [[AVAudioPCMBuffer alloc] initWithPCMFormat:format_ frameCapacity:AVAudioFrameCount(count)];
        if (!buffer.floatChannelData)
          return PlaybackFailure<std::size_t>(PlaybackErrorCode::ResourceExhausted, "Cannot allocate an iOS PCM buffer");
        buffer.frameLength = AVAudioFrameCount(count);
        for (std::size_t channel = 0; channel < channels; ++channel) {
          auto* plane = buffer.floatChannelData[channel];
          for (std::size_t index = 0; index < count; ++index)
            plane[index] = samples[index * channels + channel] / 32768.f;
        }

        auto completed = std::make_shared<std::atomic_bool>(false);
        auto status = status_;
        buffers_.push_back({written_ + count, buffer, completed});
        [player_ scheduleBuffer:buffer completionCallbackType:AVAudioPlayerNodeCompletionDataPlayedBack
             completionHandler:^(AVAudioPlayerNodeCompletionCallbackType type) {
               if (type == AVAudioPlayerNodeCompletionDataPlayedBack && status->loss.load() == OutputLoss::None)
                 completed->store(true);
             }];
        written_ += count;
        return R::Success(count);
      } @catch (NSException* error) {
        return R::Failure(AudioEngineError(error));
      }
    }
  }

  PlaybackResult<bool> Start() override {
    @autoreleasepool {
      auto output = CheckOutput();
      if (!output.Succeeded()) return output;
      @try {
        [player_ play];
        return CheckOutput();
      } @catch (NSException* error) {
        return PlaybackResult<bool>::Failure(AudioEngineError(error));
      }
    }
  }

  PlaybackResult<bool> Pause() override {
    @autoreleasepool {
      auto output = CheckOutput();
      if (!output.Succeeded()) return output;
      @try {
        [player_ pause];
        return CheckOutput();
      } @catch (NSException* error) {
        return PlaybackResult<bool>::Failure(AudioEngineError(error));
      }
    }
  }

  PlaybackResult<PlaybackProgress> Observe(bool) override {
    @autoreleasepool {
      auto output = CheckOutput();
      if (!output.Succeeded()) return PlaybackResult<PlaybackProgress>::Failure(std::move(output).Error());
      while (!buffers_.empty() && buffers_.front().completed->load()) {
        presented_ = buffers_.front().end;
        buffers_.pop_front();
      }
      output = CheckOutput();
      if (!output.Succeeded()) return PlaybackResult<PlaybackProgress>::Failure(std::move(output).Error());
      return PlaybackResult<PlaybackProgress>::Success({presented_});
    }
  }

  PlaybackResult<bool> Close() override {
    @autoreleasepool {
      if (status_) status_->loss.store(OutputLoss::Retired);
      for (id observer in observers_) [NSNotificationCenter.defaultCenter removeObserver:observer];
      observers_ = nil;
      auto result = PlaybackResult<bool>::Success(true);
      @try {
        [player_ stop];
        [engine_ stop];
      } @catch (NSException* error) {
        result = PlaybackResult<bool>::Failure(AudioEngineError(error));
      }
      buffers_.clear();
      player_ = nil;
      engine_ = nil;
      format_ = nil;
      status_.reset();
      return result;
    }
  }

private:
  PlaybackResult<bool> CheckOutput() const {
    const auto reason = status_ ? status_->loss.load() : OutputLoss::Retired;
    if (reason != OutputLoss::None || !engine_.isRunning)
      return PlaybackResult<bool>::Failure(OutputError(reason));
    return PlaybackResult<bool>::Success(true);
  }

  void WatchOutput(AVAudioSession* session) {
    observers_ = [NSMutableArray array];
    auto observe = [this](NSNotificationName name, id sender, void (^handler)(NSNotification*)) {
      auto observer = [NSNotificationCenter.defaultCenter addObserverForName:name object:sender queue:nil usingBlock:handler];
      [observers_ addObject:observer];
    };
    auto status = status_;
    observe(AVAudioSessionInterruptionNotification, session, ^(NSNotification* notification) {
      NSNumber* type = notification.userInfo[AVAudioSessionInterruptionTypeKey];
      if (type && type.unsignedIntegerValue == AVAudioSessionInterruptionTypeBegan)
        Invalidate(status, OutputLoss::Interruption);
    });
    observe(AVAudioSessionRouteChangeNotification, session, ^(NSNotification* notification) {
      const auto reason = [notification.userInfo[AVAudioSessionRouteChangeReasonKey] unsignedIntegerValue];
      AVAudioSessionRouteDescription* previous = notification.userInfo[AVAudioSessionRouteChangePreviousRouteKey];
      if (reason == AVAudioSessionRouteChangeReasonOldDeviceUnavailable ||
          reason == AVAudioSessionRouteChangeReasonNoSuitableRouteForCategory ||
          (previous && !SameOutputs(previous.outputs, session.currentRoute.outputs)))
        Invalidate(status, OutputLoss::Route);
    });
    observe(AVAudioEngineConfigurationChangeNotification, engine_, ^(NSNotification*) {
      Invalidate(status, OutputLoss::Configuration);
    });
    observe(AVAudioSessionMediaServicesWereLostNotification, session, ^(NSNotification*) {
      Invalidate(status, OutputLoss::MediaServices);
    });
    observe(AVAudioSessionMediaServicesWereResetNotification, session, ^(NSNotification*) {
      Invalidate(status, OutputLoss::MediaServices);
    });
  }

  struct Buffer {
    std::uint64_t end;
    AVAudioPCMBuffer* data;
    std::shared_ptr<std::atomic_bool> completed;
  };

  AVAudioEngine* engine_ = nil;
  AVAudioPlayerNode* player_ = nil;
  AVAudioFormat* format_ = nil;
  NSMutableArray* observers_ = nil;
  std::shared_ptr<RunStatus> status_;
  std::deque<Buffer> buffers_;
  std::size_t capacity_ = 0, quantum_ = 0;
  std::uint64_t written_ = 0, presented_ = 0;
};

} // namespace

std::unique_ptr<PlaybackDevice> MakePlaybackDevice() { return std::make_unique<IosPlaybackDevice>(); }
} // namespace huxerui::media::detail
