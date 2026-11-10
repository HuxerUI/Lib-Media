#pragma once

#include <huxerui/media_codec.h>
#include <emscripten/val.h>
#include <utility>

namespace huxerui::media::detail {

struct BrowserVideoFrame {
  explicit BrowserVideoFrame(emscripten::val value) : value(std::move(value)) {}
  BrowserVideoFrame(BrowserVideoFrame&& other) : value(std::exchange(other.value, emscripten::val::null())) {}
  ~BrowserVideoFrame() { if (*this) value.call<void>("close"); }

  explicit operator bool() const { return !value.isNull() && !value.isUndefined(); }
  emscripten::val value;
};

VideoFrame RetainBrowserVideoFrame(BrowserVideoFrame image, VideoFormat format,
    MediaTimestamp pts, std::optional<MediaTimestamp> duration);
const emscripten::val* BrowserFrame(const VideoFrame& frame) noexcept;
CodecResult<void> ValidateBrowserFrame(const emscripten::val& image, const VideoFormat& format);

} // namespace huxerui::media::detail
