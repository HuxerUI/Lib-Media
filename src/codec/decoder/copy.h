#pragma once

#include "format.h"

#include <deque>

namespace huxerui::media::detail {
CodecResult<AudioFrame> CopyAudio(const AudioFrame& source, AudioFormat requested, float gain = 1.0f);

// Reassembles a decoder's contiguous PCM chunks on the submitted packet boundaries.
class PcmPackets {
public:
  PcmPackets(AudioFormat format, unsigned packet_samples, unsigned decoder_delay = 0);
  void Expect(MediaTimestamp pts) { pending_.push_back(pts); }
  CodecResult<void> Append(const AudioFrame& frame, bool trim_drain_padding = false);
  std::optional<AudioFrame> Read();
  bool Drained() const { return pending_.empty() && !samples_; }

private:
  AudioFormat format_;
  unsigned packet_samples_, stride_, samples_ = 0;
  unsigned leading_;
  std::vector<Bytes> planes_;
  std::deque<MediaTimestamp> pending_;
};

CodecResult<VideoFrame> CopyVideo(VideoFormat format, MediaTimestamp pts,
    std::optional<MediaTimestamp> duration, std::vector<VideoPlane> planes, VideoPixelFormat requested);
// Preserves storage and output credit through retained frame planes and arbitrary slices.
void RetainFrame(DecodedFrame& frame, const std::shared_ptr<const void>& credit);
std::size_t FrameBytes(const DecodedFrame& frame);
} // namespace huxerui::media::detail
