#pragma once

#include "format.h"

namespace huxerui::media::detail {
CodecResult<AudioFrame> CopyAudio(const AudioFrame& source, AudioFormat requested);
CodecResult<VideoFrame> CopyVideo(VideoFormat format, MediaTimestamp pts,
    std::optional<MediaTimestamp> duration, std::vector<VideoPlane> planes, VideoPixelFormat requested);
// Preserves storage and output credit through retained frame planes and arbitrary slices.
void RetainFrame(DecodedFrame& frame, const std::shared_ptr<const void>& credit);
std::size_t FrameBytes(const DecodedFrame& frame);
} // namespace huxerui::media::detail
