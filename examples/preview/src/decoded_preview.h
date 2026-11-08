#pragma once

#include <functional>

#include <huxerui/external_texture.h>
#include <huxerui/media_format.h>
#include <huxerui/resource.h>
#include <huxerui/task.h>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

namespace media_preview {
#if defined(__ANDROID__) || defined(__APPLE__) || defined(__EMSCRIPTEN__) || defined(_WIN32) || defined(__linux__)
inline constexpr bool frame_texture_supported = true;
#else
inline constexpr bool frame_texture_supported = false;
#endif

// Creates a texture for one preview run; publication copies or retains pixels through the public SDK.
std::shared_ptr<huxerui::ExternalTexture> MakeFrameTexture(huxerui::Size size);
// Copies the visible CPU YUV region into owned RGBA bytes using the preview's SDR color policy.
huxerui::Bytes ConvertFramePixels(const huxerui::media::VideoFrame& frame);
void PublishFrame(huxerui::ExternalTexture& texture, const huxerui::media::VideoFrame& frame);
class PreviewAudioUse;
#if defined(__APPLE__) && TARGET_OS_IOS
inline constexpr bool preview_audio_session_supported = true;
std::shared_ptr<PreviewAudioUse> RetainPreviewAudioSession(bool activate);
#else
inline constexpr bool preview_audio_session_supported = false;
inline std::shared_ptr<PreviewAudioUse> RetainPreviewAudioSession(bool) { return {}; }
#endif

// Tasks keep their resources and texture alive and observe pause intent through the supplied callback.
huxerui::Task<std::string> PlayDecodedClip(std::shared_ptr<huxerui::Resources> resources,
                                          std::shared_ptr<huxerui::ExternalTexture> texture, bool with_audio,
                                          std::function<bool()> paused);
huxerui::Task<std::string> PlayEncodedClip(std::shared_ptr<huxerui::ExternalTexture> texture,
                                          std::function<bool()> paused);
}
