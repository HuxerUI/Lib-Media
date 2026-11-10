#include "decoded_preview.h"
#include "media_video_ios.h"

#import <AVFoundation/AVFoundation.h>
#import <CoreVideo/CoreVideo.h>

#include <huxerui/ios/external_texture.h>
#include <stdexcept>

namespace media_preview {
namespace {

// Accessed on the main queue. A loaded AVPlayer can also use the application audio session.
std::size_t audio_users = 0;
bool audio_activated = false;

} // namespace

class PreviewAudioUse {
public:
  explicit PreviewAudioUse(bool activate) {
    if (activate) {
      @autoreleasepool {
        auto* session = AVAudioSession.sharedInstance;
        NSError* error = nil;
        if ((![session.category isEqualToString:AVAudioSessionCategoryPlayback] &&
             ![session setCategory:AVAudioSessionCategoryPlayback error:&error]) ||
            ![session setActive:YES error:&error]) {
          const auto* message = error.localizedDescription.UTF8String;
          throw std::runtime_error(message ? message : "Cannot activate the iOS preview audio session");
        }
        audio_activated = true;
      }
    }
    ++audio_users;
  }

  ~PreviewAudioUse() {
    dispatch_async(dispatch_get_main_queue(), ^{
      if (--audio_users || !audio_activated) return;
      audio_activated = false;
      NSError* error = nil;
      if (![AVAudioSession.sharedInstance setActive:NO
                                       withOptions:AVAudioSessionSetActiveOptionNotifyOthersOnDeactivation
                                             error:&error])
        NSLog(@"Cannot deactivate the iOS preview audio session: %@", error);
    });
  }
};

std::shared_ptr<PreviewAudioUse> RetainPreviewAudioSession(bool activate) {
  return std::make_shared<PreviewAudioUse>(activate);
}

std::shared_ptr<huxerui::ExternalTexture> MakeFrameTexture(huxerui::Size size, huxerui::media::VideoStorage) {
  return std::make_shared<huxerui::ios::PixelBufferTexture>(size);
}

void PublishFrame(huxerui::ExternalTexture& texture, const huxerui::media::VideoFrame& frame) {
  @autoreleasepool {
    if (frame.Storage() == huxerui::media::VideoStorage::Platform) {
      const auto buffer = huxerui::media::ios::PixelBuffer(frame);
      if (!buffer) throw std::runtime_error("Expected an iOS pixel buffer");
      static_cast<huxerui::ios::PixelBufferTexture&>(texture).Publish(buffer);
      return;
    }
    const auto pixels = ConvertFramePixels(frame);
    const auto [width, height] = DisplayDimensions(frame.format.visible, frame.format.rotation);
    NSDictionary* attributes = @{
      (__bridge NSString*)kCVPixelBufferIOSurfacePropertiesKey: @{},
      (__bridge NSString*)kCVPixelBufferMetalCompatibilityKey: @YES
    };
    CVPixelBufferRef buffer = nullptr;
    const auto created = CVPixelBufferCreate(kCFAllocatorDefault, width, height, kCVPixelFormatType_32BGRA,
                                             (__bridge CFDictionaryRef)attributes, &buffer);
    if (created != kCVReturnSuccess) throw std::runtime_error("Cannot allocate the iOS preview pixel buffer");
    const std::unique_ptr<std::remove_pointer_t<CVPixelBufferRef>, decltype(&CVPixelBufferRelease)>
        owned(buffer, CVPixelBufferRelease);
    if (CVPixelBufferLockBaseAddress(buffer, 0) != kCVReturnSuccess)
      throw std::runtime_error("Cannot lock the iOS preview pixel buffer");

    auto* base = static_cast<std::byte*>(CVPixelBufferGetBaseAddress(buffer));
    const auto stride = CVPixelBufferGetBytesPerRow(buffer);
    for (unsigned y = 0; y < height; ++y) {
      auto* row = base + y * stride;
      const auto* rgba = pixels.data() + std::size_t(y) * width * 4;
      for (unsigned x = 0; x < width; ++x) {
        row[x * 4] = rgba[x * 4 + 2];
        row[x * 4 + 1] = rgba[x * 4 + 1];
        row[x * 4 + 2] = rgba[x * 4];
        row[x * 4 + 3] = rgba[x * 4 + 3];
      }
    }
    CVPixelBufferUnlockBaseAddress(buffer, 0);
    static_cast<huxerui::ios::PixelBufferTexture&>(texture).Publish(buffer);
  }
}

} // namespace media_preview
