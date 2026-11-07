#import <AVFoundation/AVFoundation.h>

#import <AppKit/AppKit.h>
#include <huxerui/macos/platform_registry.h>

#include <cmath>

#include "player/state.h"
#include "player/video_geometry.h"

@interface HUXMediaObservation : NSObject
@property(nonatomic, copy) void (^changed)(NSString *);
@end

@implementation HUXMediaObservation
- (void)observeValueForKeyPath:(NSString *)keyPath
                      ofObject:(id)object
                        change:(NSDictionary *)change
                       context:(void *)context {
  if (self.changed)
    self.changed(keyPath);
}
@end

@interface HUXMediaVideoView : NSView
@property(nonatomic, strong) AVPlayerLayer *videoLayer;
@property(nonatomic) huxerui::Size videoSize;
@property(nonatomic) huxerui::media::VideoSurfaceProperties properties;
- (void)updateVideoLayout;
@end

@implementation HUXMediaVideoView
- (instancetype)initWithFrame:(CGRect)frame {
  self = [super initWithFrame:frame];
  if (self) {
    self.wantsLayer = YES;
    self.layer.geometryFlipped = YES;
    self.layer.masksToBounds = YES;
    CGColorSpaceRef colorSpace = CGColorSpaceCreateDeviceRGB();
    const CGFloat components[] = {0, 0, 0, 1};
    CGColorRef black = CGColorCreate(colorSpace, components);
    CGColorSpaceRelease(colorSpace);
    self.layer.backgroundColor = black;
    CGColorRelease(black);
    self.videoLayer = [AVPlayerLayer playerLayerWithPlayer:nil];
    self.videoLayer.videoGravity = AVLayerVideoGravityResize;
    [self.layer addSublayer:self.videoLayer];
  }
  return self;
}
- (BOOL)isFlipped {
  return YES;
}
- (void)layout {
  [super layout];
  [self updateVideoLayout];
}
- (void)updateVideoLayout {
  auto rect = huxerui::media::detail::FitVideo(
      self.videoSize,
      {static_cast<float>(self.bounds.size.width),
       static_cast<float>(self.bounds.size.height)},
      self.properties);
  [CATransaction begin];
  [CATransaction setDisableActions:YES];
  self.videoLayer.frame = CGRectMake(rect.x, rect.y, rect.width, rect.height);
  [CATransaction commit];
}
@end

namespace huxerui::media::detail {
namespace {

class MacOSPlayer final : public PlayerBackend,
                          public std::enable_shared_from_this<MacOSPlayer> {
public:
  ~MacOSPlayer() override { Clear(); }

  void Load(const MediaSource &source, std::uint64_t generation) override {
    Clear();
    source_ = source;
    generation_ = generation;
    const auto &input = PlayerAccess::Source(source);
    NSURL *url = nil;
    if (auto http = std::get_if<MediaHttpSource>(&input)) {
      if (!http->headers.empty()) {
        Error(MediaErrorCode::Unsupported, 0, true);
        return;
      }
      url = [NSURL
          URLWithString:[NSString stringWithUTF8String:http->url.c_str()]];
    } else {
      auto file = std::get_if<File>(&input)
                      ? std::optional<File>(std::get<File>(input))
                      : std::get<FileReference>(input).AsFile();
      if (!file) {
        Error(MediaErrorCode::Unsupported, 0, true);
        return;
      }
      url = [NSURL
          fileURLWithPath:[NSString stringWithUTF8String:file->Path().c_str()]];
    }
    if (!url) {
      Error(MediaErrorCode::InvalidSource, 0, true);
      return;
    }
    item_ = [AVPlayerItem playerItemWithURL:url];
    player_ = [AVPlayer playerWithPlayerItem:item_];
    player_.volume = static_cast<float>(volume_);
    player_.muted = muted_;
    observation_ = [HUXMediaObservation new];
    std::weak_ptr<MacOSPlayer> weak = shared_from_this();
    observation_.changed = ^(NSString *key) {
      dispatch_async(dispatch_get_main_queue(), ^{
        if (auto current = weak.lock();
            current && current->generation_ == generation && current->item_)
          current->Changed(key);
      });
    };
    for (NSString *key in ItemKeys())
      [item_ addObserver:observation_
              forKeyPath:key
                 options:NSKeyValueObservingOptionNew
                 context:nullptr];
    [player_ addObserver:observation_
              forKeyPath:@"timeControlStatus"
                 options:NSKeyValueObservingOptionNew
                 context:nullptr];
    ended_ = [[NSNotificationCenter defaultCenter]
        addObserverForName:AVPlayerItemDidPlayToEndTimeNotification
                    object:item_
                     queue:[NSOperationQueue mainQueue]
                usingBlock:^(NSNotification *) {
                  if (auto current = weak.lock();
                      current && current->generation_ == generation &&
                      current->item_)
                    current->Send(PlayerEventKind::Ended);
                }];
    failed_ = [[NSNotificationCenter defaultCenter]
        addObserverForName:AVPlayerItemFailedToPlayToEndTimeNotification
                    object:item_
                     queue:[NSOperationQueue mainQueue]
                usingBlock:^(NSNotification *notification) {
                  if (auto current = weak.lock();
                      current && current->generation_ == generation &&
                      current->item_) {
                    NSError *error =
                        notification.userInfo
                            [AVPlayerItemFailedToPlayToEndTimeErrorKey];
                    current->Error(MediaErrorCode::Decode, error.code, true);
                  }
                }];
    if (view_)
      view_.videoLayer.player = player_;
    Changed(@"status");
  }

  void Clear() noexcept override {
    play_intent_ = false;
    ready_ = false;
    if (observation_) {
      observation_.changed = nil;
      for (NSString *key in ItemKeys())
        [item_ removeObserver:observation_ forKeyPath:key];
      [player_ removeObserver:observation_ forKeyPath:@"timeControlStatus"];
    }
    observation_ = nil;
    for (id token in @[ ended_ ?: [NSNull null], failed_ ?: [NSNull null] ]) {
      if (token != [NSNull null])
        [[NSNotificationCenter defaultCenter] removeObserver:token];
    }
    ended_ = nil;
    failed_ = nil;
    if (view_) {
      view_.videoLayer.player = nil;
      view_.videoSize = {};
      [view_ updateVideoLayout];
    }
    [item_ cancelPendingSeeks];
    [player_ pause];
    [player_ replaceCurrentItemWithPlayerItem:nil];
    player_ = nil;
    item_ = nil;
    source_.reset();
  }

  void Play() override {
    play_intent_ = true;
    [player_ play];
  }
  void Pause() override {
    play_intent_ = false;
    [player_ pause];
    if (ready_)
      Send(PlayerEventKind::Paused);
  }
  void SetVolume(double volume, bool muted) override {
    volume_ = volume;
    muted_ = muted;
    player_.volume = static_cast<float>(volume);
    player_.muted = muted;
  }
  void Seek(MediaTime position, std::uint64_t seek_id) override {
    std::weak_ptr<MacOSPlayer> weak = shared_from_this();
    const auto generation = generation_;
    [player_ seekToTime:CMTimeMakeWithSeconds(position.count(), 1000000)
        completionHandler:^(BOOL finished) {
          dispatch_async(dispatch_get_main_queue(), ^{
            if (auto current = weak.lock();
                current && current->generation_ == generation &&
                current->item_) {
              if (finished)
                current->Send(PlayerEventKind::Seeked, seek_id);
              else
                current->Error(MediaErrorCode::SeekFailed, 0, false, seek_id);
            }
          });
        }];
  }
  void Poll() override {
    if (ready_)
      Send(PlayerEventKind::Update);
  }
  void Attach(HUXMediaVideoView *view) {
    if (view_)
      view_.videoLayer.player = nil;
    view_ = view;
    if (view_) {
      view_.videoLayer.player = player_;
      UpdateVideo();
    }
  }

private:
  static NSArray<NSString *> *ItemKeys() {
    return @[
      @"status", @"duration", @"loadedTimeRanges", @"presentationSize",
      @"playbackBufferEmpty"
    ];
  }
  PlayerSnapshot Snapshot() const {
    PlayerSnapshot value;
    const double position = CMTimeGetSeconds(player_.currentTime);
    if (std::isfinite(position) && position >= 0)
      value.progress.position = MediaTime(position);
    const double duration = CMTimeGetSeconds(item_.duration);
    if (std::isfinite(duration) && duration >= 0)
      value.progress.duration = MediaTime(duration);
    if (ready_) {
      value.progress.buffered_ranges.emplace();
      for (NSValue *range in item_.loadedTimeRanges) {
        const auto time_range = range.CMTimeRangeValue;
        const double start = CMTimeGetSeconds(time_range.start),
                     end = CMTimeGetSeconds(CMTimeRangeGetEnd(time_range));
        if (std::isfinite(start) && std::isfinite(end) && start >= 0 &&
            end >= start)
          value.progress.buffered_ranges->push_back(
              {MediaTime(start), MediaTime(end)});
      }
      value.seekability =
          item_.seekableTimeRanges.count > 0 && value.progress.duration
              ? MediaSeekability::Seekable
              : MediaSeekability::NotSeekable;
      bool audio = false, video = false;
      for (AVPlayerItemTrack *track in item_.tracks) {
        audio = audio ||
                [track.assetTrack.mediaType isEqualToString:AVMediaTypeAudio];
        video = video ||
                [track.assetTrack.mediaType isEqualToString:AVMediaTypeVideo];
      }
      value.has_audio = audio;
      value.has_video = video;
      const auto size = item_.presentationSize;
      if (size.width > 0 && size.height > 0)
        value.video_size = Size{static_cast<float>(size.width),
                                static_cast<float>(size.height)};
    }
    value.buffering =
        player_.timeControlStatus ==
            AVPlayerTimeControlStatusWaitingToPlayAtSpecifiedRate ||
        item_.playbackBufferEmpty;
    return value;
  }
  void Send(PlayerEventKind kind, std::uint64_t seek_id = 0) {
    if (emit)
      emit({.generation = generation_,
            .kind = kind,
            .snapshot = Snapshot(),
            .seek_id = seek_id});
  }
  void Error(MediaErrorCode code, NSInteger platform_code, bool fatal,
             std::uint64_t seek_id = 0) {
    if (platform_code == NSFileReadNoSuchFileError)
      code = MediaErrorCode::NotFound;
    if (platform_code == NSFileReadNoPermissionError)
      code = MediaErrorCode::PermissionDenied;
    if (emit)
      emit({.generation = generation_,
            .kind = PlayerEventKind::Error,
            .seek_id = seek_id,
            .error = {code, "HuxerUI macOS media operation failed",
                      std::to_string(platform_code), fatal}});
  }
  void UpdateVideo() {
    if (!view_)
      return;
    const auto size = item_.presentationSize;
    view_.videoSize = {static_cast<float>(size.width),
                       static_cast<float>(size.height)};
    [view_ updateVideoLayout];
  }
  void Changed(NSString *key) {
    if ([key isEqualToString:@"status"]) {
      if (item_.status == AVPlayerItemStatusFailed)
        Error(MediaErrorCode::Decode, item_.error.code, true);
      else if (item_.status == AVPlayerItemStatusReadyToPlay && !ready_) {
        ready_ = true;
        UpdateVideo();
        Send(PlayerEventKind::Ready);
      }
    } else if ([key isEqualToString:@"timeControlStatus"] && ready_) {
      Send(player_.timeControlStatus == AVPlayerTimeControlStatusPlaying
               ? PlayerEventKind::Playing
           : player_.timeControlStatus == AVPlayerTimeControlStatusPaused
               ? PlayerEventKind::Paused
               : PlayerEventKind::Update);
    } else if (ready_) {
      UpdateVideo();
      Send(PlayerEventKind::Update);
    }
  }

  __strong AVPlayer *player_ = nil;
  __strong AVPlayerItem *item_ = nil;
  __strong HUXMediaObservation *observation_ = nil;
  __strong id ended_ = nil, failed_ = nil;
  __weak HUXMediaVideoView *view_ = nil;
  std::optional<MediaSource> source_;
  std::uint64_t generation_ = 0;
  double volume_ = 1;
  bool muted_ = false, ready_ = false, play_intent_ = false;
};

struct MacOSVideo {
  __strong HUXMediaVideoView *view;
  std::weak_ptr<MacOSPlayer> player;
};

} // namespace

void InstallPlatform(ApplicationContext &context) {
  context.RegisterPlatformModule<std::shared_ptr<PlayerBackend>>(
      player_type, [](UiWindow &) -> std::shared_ptr<PlayerBackend> {
        return std::make_shared<MacOSPlayer>();
      });
  macos::PlatformViewFactory<VideoSurfaceProperties, MacOSVideo, MediaPlayer>
      factory{
          .create =
              [](NSWindow *, const VideoSurfaceProperties &properties,
                 PlatformEventEmitter) {
                auto value = std::make_shared<MacOSVideo>();
                value->view =
                    [[HUXMediaVideoView alloc] initWithFrame:CGRectZero];
                value->view.properties = properties;
                return value;
              },
          .view =
              [](const std::shared_ptr<MacOSVideo> &value) {
                return value->view;
              },
          .update =
              [](MacOSVideo &value, const VideoSurfaceProperties &properties) {
                value.view.properties = properties;
                [value.view updateVideoLayout];
              },
          .dispose =
              [](MacOSVideo &value) {
                value.view.videoLayer.player = nil;
                value.view = nil;
              },
          .connect =
              [](MacOSVideo &value, const MediaPlayer &player) {
                PlayerAccess::Bind(
                    player,
                    [&value](const std::shared_ptr<PlayerBackend> &backend) {
                      if (auto old = value.player.lock())
                        old->Attach(nil);
                      value.player.reset();
                      if (backend) {
                        auto current =
                            std::static_pointer_cast<MacOSPlayer>(backend);
                        value.player = current;
                        current->Attach(value.view);
                      }
                    });
              },
          .disconnect =
              [](MacOSVideo &, const MediaPlayer &player) {
                PlayerAccess::Unbind(player);
              },
      };
  context.RegisterPlatformView<VideoSurfaceProperties, MediaPlayer>(
      video_type, std::move(factory));
}

} // namespace huxerui::media::detail
