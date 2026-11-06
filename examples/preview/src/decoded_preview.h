#pragma once

#include <functional>

#include <huxerui/external_texture.h>
#include <huxerui/media_format.h>
#include <huxerui/resource.h>
#include <huxerui/task.h>

namespace media_preview {
#if defined(__ANDROID__) || defined(MEDIA_PREVIEW_MACOS)
inline constexpr bool frame_texture_supported = true;
#else
inline constexpr bool frame_texture_supported = false;
#endif

std::shared_ptr<huxerui::ExternalTexture> MakeFrameTexture(huxerui::Size size);
huxerui::Bytes ConvertFramePixels(const huxerui::media::VideoFrame& frame);
void PublishFrame(huxerui::ExternalTexture& texture, const huxerui::media::VideoFrame& frame);

huxerui::Task<std::string> PlayDecodedClip(std::shared_ptr<huxerui::Resources> resources,
                                          std::shared_ptr<huxerui::ExternalTexture> texture, bool with_audio,
                                          std::function<bool()> paused);
}
