#pragma once

#include <CoreVideo/CVPixelBuffer.h>
#include <huxerui/media_codec.h>

namespace huxerui::media::macos {

/// @brief Retains a Metal-compatible NV12 pixel buffer as a platform video frame.
/// @param image An IOSurface-backed NV12 buffer whose producer GPU writes have completed.
/// @param format Coded geometry and color information matching the buffer.
/// @param pts Presentation timestamp.
/// @param duration Optional display duration.
/// @return The retained frame, or an error for an incompatible image or invalid metadata.
/// Keep the image immutable until all retained frame copies and GPU reads have been released.
CodecResult<VideoFrame> RetainPixelBuffer(CVPixelBufferRef image, VideoFormat format,
    MediaTimestamp pts, std::optional<MediaTimestamp> duration = {});

/// @brief Borrows the pixel buffer held by a platform frame, or nullptr for a CPU frame.
/// Keep the frame alive through all CPU and GPU use of the returned buffer.
CVPixelBufferRef PixelBuffer(const VideoFrame& frame) noexcept;

} // namespace huxerui::media::macos
