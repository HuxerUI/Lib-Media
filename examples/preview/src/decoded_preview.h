#pragma once

#include <functional>

#include <huxerui/external_texture.h>
#include <huxerui/media_format.h>
#include <huxerui/resource.h>
#include <huxerui/task.h>

namespace media_preview {
#if defined(__ANDROID__) || defined(MEDIA_PREVIEW_MACOS) || defined(MEDIA_PREVIEW_IOS)
inline constexpr bool frame_texture_supported = true;
#else
inline constexpr bool frame_texture_supported = false;
#endif

std::shared_ptr<huxerui::ExternalTexture> MakeFrameTexture(huxerui::Size size);
huxerui::Bytes ConvertFramePixels(const huxerui::media::VideoFrame& frame);
void PublishFrame(huxerui::ExternalTexture& texture, const huxerui::media::VideoFrame& frame);
class PreviewAudioUse;
#if defined(MEDIA_PREVIEW_IOS)
inline constexpr bool preview_audio_session_supported = true;
std::shared_ptr<PreviewAudioUse> RetainPreviewAudioSession(bool activate);
#else
inline constexpr bool preview_audio_session_supported = false;
inline std::shared_ptr<PreviewAudioUse> RetainPreviewAudioSession(bool) { return {}; }
#endif

huxerui::Task<std::string> PlayDecodedClip(std::shared_ptr<huxerui::Resources> resources,
                                          std::shared_ptr<huxerui::ExternalTexture> texture, bool with_audio,
                                          std::function<bool()> paused);
}
