#import <AVFoundation/AVFoundation.h>
#import <AudioToolbox/AudioToolbox.h>

#include <huxerui/file.h>
#include <huxerui/media_format.h>

#include <array>
#include <cmath>
#include <cstring>
#include <stdexcept>

using namespace huxerui;
using namespace huxerui::media;

namespace {

using Sample = std::unique_ptr<std::remove_pointer_t<CMSampleBufferRef>, decltype(&CFRelease)>;

void Check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

AVAssetTrack* LoadTrack(AVURLAsset* asset, AVMediaType type) {
  const auto done = dispatch_semaphore_create(0);
  __block NSArray<AVAssetTrack*>* tracks = nil;
  [asset loadTracksWithMediaType:type completionHandler:^(NSArray<AVAssetTrack*>* loaded, NSError*) {
    tracks = loaded;
    dispatch_semaphore_signal(done);
  }];
  Check(dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)) == 0 && tracks.count == 1,
        "AVFoundation could not load the expected media track");
  return tracks.firstObject;
}

AVAssetReader* OpenReader(AVURLAsset* asset, NSArray<AVAssetReaderTrackOutput*>* outputs) {
  auto* reader = [[AVAssetReader alloc] initWithAsset:asset error:nil];
  for (AVAssetReaderTrackOutput* output in outputs) {
    Check([reader canAddOutput:output], "AVFoundation could not open the media track");
    [reader addOutput:output];
  }
  Check([reader startReading], "AVFoundation could not start reading the asset");
  return reader;
}

MediaTimestamp Timestamp(CMTime time) {
  Check(CMTIME_IS_NUMERIC(time) && time.timescale > 0, "AVFoundation returned invalid MOV timing");
  return {time.value, static_cast<std::uint32_t>(time.timescale)};
}

std::array<int, 4> Chroma(CVPixelBufferRef image) {
  Check(image && CVPixelBufferGetPixelFormatType(image) == kCVPixelFormatType_422YpCbCr10BiPlanarVideoRange &&
        CVPixelBufferGetPlaneCount(image) == 2 && CVPixelBufferGetWidth(image) == 64 && CVPixelBufferGetHeight(image) == 48 &&
        CVPixelBufferGetWidthOfPlane(image, 1) == 32 && CVPixelBufferGetHeightOfPlane(image, 1) == 48,
        "AVFoundation did not decode full-height ten-bit 4:2:2 chroma");
  Check(CVPixelBufferLockBaseAddress(image, kCVPixelBufferLock_ReadOnly) == kCVReturnSuccess,
        "AVFoundation could not map decoded chroma");
  const auto* bytes = static_cast<const std::byte*>(CVPixelBufferGetBaseAddressOfPlane(image, 1));
  const auto stride = CVPixelBufferGetBytesPerRowOfPlane(image, 1);
  std::array<int, 4> codes{};
  for (unsigned i = 0; i < codes.size(); ++i) {
    std::uint16_t word;
    std::memcpy(&word, bytes + (24 + i / 2) * stride + 16 * 4 + (i % 2) * 2, sizeof(word));
    codes[i] = word >> 6;
  }
  Check(CVPixelBufferUnlockBaseAddress(image, kCVPixelBufferLock_ReadOnly) == kCVReturnSuccess,
        "AVFoundation could not unmap decoded chroma");
  return codes;
}

struct MovieReadback {
  std::vector<std::pair<MediaTimestamp, MediaTimestamp>> pictures;
  std::vector<std::array<int, 4>> chroma;
  Bytes pcm;
};

MovieReadback ReadMovie(const File& file, FourCharCode codec) {
  @autoreleasepool {
    auto* path = [NSString stringWithUTF8String:file.Path().c_str()];
    auto* asset = [AVURLAsset URLAssetWithURL:[NSURL fileURLWithPath:path] options:nil];
    auto* video = LoadTrack(asset, AVMediaTypeVideo);
    auto* audio = LoadTrack(asset, AVMediaTypeAudio);
    const auto description = (__bridge CMFormatDescriptionRef)video.formatDescriptions.firstObject;
    Check(description && CMFormatDescriptionGetMediaSubType(description) == codec,
          "AVFoundation opened a different ProRes profile");
    auto* packets = [[AVAssetReaderTrackOutput alloc] initWithTrack:video outputSettings:nil];
    auto* samples = [[AVAssetReaderTrackOutput alloc] initWithTrack:audio outputSettings:nil];
    auto* reader = OpenReader(asset, @[packets, samples]);
    MovieReadback result;
    while (const auto buffer = Sample([packets copyNextSampleBuffer], CFRelease)) {
      const auto count = CMSampleBufferGetNumSamples(buffer.get());
      if (!count) continue;
      Check(count == 1, "AVFoundation did not return one picture per ProRes sample");
      result.pictures.emplace_back(Timestamp(CMSampleBufferGetPresentationTimeStamp(buffer.get())),
                                   Timestamp(CMSampleBufferGetDuration(buffer.get())));
    }
    constexpr unsigned rate = 8000, frame_bytes = 6;
    std::int64_t frames = 0;
    while (const auto buffer = Sample([samples copyNextSampleBuffer], CFRelease)) {
      const auto count = CMSampleBufferGetNumSamples(buffer.get());
      if (!count) continue;
      const auto format = CMAudioFormatDescriptionGetStreamBasicDescription(CMSampleBufferGetFormatDescription(buffer.get()));
      Check(format && format->mFormatID == kAudioFormatLinearPCM && format->mSampleRate == rate &&
            format->mBitsPerChannel == 24 && format->mChannelsPerFrame == 2 && format->mBytesPerFrame == frame_bytes &&
            !(format->mFormatFlags & kAudioFormatFlagIsBigEndian), "AVFoundation read a different LPCM format");
      Check(Timestamp(CMSampleBufferGetPresentationTimeStamp(buffer.get())) == MediaTimestamp{frames, rate},
            "AVFoundation read a discontinuity in the MOV audio clock");
      const auto data = CMSampleBufferGetDataBuffer(buffer.get());
      const auto size = std::size_t(count) * frame_bytes;
      const auto offset = result.pcm.size();
      result.pcm.resize(offset + size);
      Check(data && CMBlockBufferGetDataLength(data) == size &&
            !CMBlockBufferCopyDataBytes(data, 0, size, result.pcm.data() + offset), "AVFoundation could not read LPCM bytes");
      frames += count;
    }
    Check(reader.status == AVAssetReaderStatusCompleted && frames == 4000 && result.pictures.size() == 4,
          "AVFoundation did not read four pictures and 4000 LPCM sample frames");
    for (std::size_t i = 0; i < result.pictures.size(); ++i) {
      const auto& [pts, duration] = result.pictures[i];
      Check(pts == MediaTimestamp{static_cast<std::int64_t>(i), 8} && duration == MediaTimestamp{1, 8},
            "AVFoundation read unexpected video presentation intervals");
    }
    auto* pictures = [[AVAssetReaderTrackOutput alloc] initWithTrack:video outputSettings:@{
        (__bridge NSString*)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_422YpCbCr10BiPlanarVideoRange)
    }];
    auto* decoder = OpenReader(asset, @[pictures]);
    while (const auto buffer = Sample([pictures copyNextSampleBuffer], CFRelease)) {
      const auto count = CMSampleBufferGetNumSamples(buffer.get());
      if (!count) continue;
      Check(count == 1 && result.chroma.size() < result.pictures.size() &&
            Timestamp(CMSampleBufferGetPresentationTimeStamp(buffer.get())) == result.pictures[result.chroma.size()].first,
            "AVFoundation decoded the wrong picture interval");
      result.chroma.push_back(Chroma(CMSampleBufferGetImageBuffer(buffer.get())));
    }
    Check(decoder.status == AVAssetReaderStatusCompleted && result.chroma.size() == result.pictures.size(),
          "AVFoundation did not decode every ProRes picture");
    return result;
  }
}

void CheckStatus(OSStatus status, const char* action) {
  if (status) throw std::runtime_error(std::string(action) + " (" + std::to_string(status) + ")");
}

std::vector<std::int32_t> ReadM4aAudio(const File& file, MediaTimestamp expected, unsigned channels) {
  @autoreleasepool {
    auto* path = [NSString stringWithUTF8String:file.Path().c_str()];
    auto* asset = [AVURLAsset URLAssetWithURL:[NSURL fileURLWithPath:path] options:nil];
    auto* track = LoadTrack(asset, AVMediaTypeAudio);
    auto* output = [[AVAssetReaderTrackOutput alloc] initWithTrack:track outputSettings:@{
        AVFormatIDKey: @(kAudioFormatLinearPCM), AVLinearPCMBitDepthKey: @32,
        AVLinearPCMIsFloatKey: @NO, AVLinearPCMIsBigEndianKey: @NO, AVLinearPCMIsNonInterleaved: @NO,
    }];
    auto* reader = OpenReader(asset, @[output]);
    std::vector<std::int32_t> samples;
    while (const auto buffer = Sample([output copyNextSampleBuffer], CFRelease)) {
      const auto count = CMSampleBufferGetNumSamples(buffer.get());
      if (!count) continue;
      const auto frames = static_cast<std::int64_t>(samples.size() / channels);
      const auto description = CMSampleBufferGetFormatDescription(buffer.get());
      const auto format = CMAudioFormatDescriptionGetStreamBasicDescription(description);
      Check(format && format->mSampleRate == expected.timescale && format->mChannelsPerFrame == channels &&
            format->mBitsPerChannel == 32 && count <= expected.ticks - frames,
            "AVFoundation read a different M4A format or sample count");
      Check(Timestamp(CMSampleBufferGetOutputPresentationTimeStamp(buffer.get())) == MediaTimestamp{frames, expected.timescale},
            "AVFoundation read discontinuous M4A presentation timing");
      const auto offset = samples.size();
      const auto values = std::size_t(count) * channels;
      const auto bytes = values * sizeof(std::int32_t);
      samples.resize(offset + values);
      const auto data = CMSampleBufferGetDataBuffer(buffer.get());
      Check(data && CMBlockBufferGetDataLength(data) == bytes &&
            !CMBlockBufferCopyDataBytes(data, 0, bytes, samples.data() + offset), "AVFoundation could not read M4A PCM");
    }
    Check(reader.status == AVAssetReaderStatusCompleted && samples.size() / channels == std::size_t(expected.ticks),
          "AVFoundation lost valid M4A samples");
    return samples;
  }
}

std::vector<std::int32_t> ReadAudio(const File& file, MediaTimestamp expected, unsigned channels) {
  const auto path = file.Path();
  // The asset reader applies the movie edit list when presenting M4A audio.
  if (path.ends_with(".m4a")) return ReadM4aAudio(file, expected, channels);
  const auto url = CFURLCreateFromFileSystemRepresentation(nullptr,
      reinterpret_cast<const UInt8*>(path.data()), path.size(), false);
  if (!url) throw std::runtime_error("Core Audio could not create the audio file URL");
  ExtAudioFileRef audio = nullptr;
  const auto status = ExtAudioFileOpenURL(url, &audio);
  CFRelease(url);
  CheckStatus(status, "Core Audio could not open the audio file");
  const std::unique_ptr<std::remove_pointer_t<ExtAudioFileRef>, decltype(&ExtAudioFileDispose)> owner(audio, ExtAudioFileDispose);
  AudioStreamBasicDescription source{};
  UInt32 size = sizeof(source);
  CheckStatus(ExtAudioFileGetProperty(audio, kExtAudioFileProperty_FileDataFormat, &size, &source),
              "Core Audio could not read the audio format");
  if (source.mSampleRate != expected.timescale || source.mChannelsPerFrame != channels)
    throw std::runtime_error("Core Audio read a different rate or channel count");

  const auto stride = channels * std::uint32_t(sizeof(std::int32_t));
  const AudioStreamBasicDescription pcm{source.mSampleRate, kAudioFormatLinearPCM,
      kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked, stride, 1, stride, channels, 32, 0};
  CheckStatus(ExtAudioFileSetProperty(audio, kExtAudioFileProperty_ClientDataFormat, sizeof(pcm), &pcm),
              "Core Audio could not configure decoded PCM");
  constexpr UInt32 block = 1024;
  std::vector<std::int32_t> buffer(block * channels), samples;
  const auto limit = static_cast<std::size_t>(expected.ticks) * channels;
  for (;;) {
    auto frames = block;
    AudioBufferList list{1, {{channels, block * stride, buffer.data()}}};
    CheckStatus(ExtAudioFileRead(audio, &frames, &list), "Core Audio could not read decoded PCM");
    if (!frames) break;
    const auto values = frames * channels;
    if (values > limit - samples.size()) throw std::runtime_error("Core Audio produced extra audio samples for " + file.Path());
    samples.insert(samples.end(), buffer.begin(), buffer.begin() + values);
  }
  if (samples.size() != limit) throw std::runtime_error("Core Audio lost valid audio samples for " + file.Path());
  return samples;
}

} // namespace

void CheckProResReadback(const File& source, const File& export_file) {
  const auto input = ReadMovie(source, kCMVideoCodecType_AppleProRes422HQ);
  const auto output = ReadMovie(export_file, kCMVideoCodecType_AppleProRes422);
  Check(input.pictures == output.pictures && input.pcm == output.pcm, "MOV export changed the A/V timeline or LPCM bytes");
  for (std::size_t i = 0; i < input.chroma.size(); ++i) {
    const auto& chroma = input.chroma[i];
    Check(chroma[2] - chroma[0] > 250 && chroma[1] - chroma[3] > 180, "The HQ fixture must distinguish adjacent chroma rows");
    for (std::size_t c = 0; c < chroma.size(); ++c)
      Check(std::abs(chroma[c] - output.chroma[i][c]) <= 32, "ProRes export changed the full-height chroma pattern");
  }
}

void CheckAudioReadback(const File& source, const File& output, MediaTimestamp expected, unsigned channels) {
  if (ReadAudio(source, expected, channels) != ReadAudio(output, expected, channels))
    throw std::runtime_error("Core Audio read different PCM after the container copy");
}
