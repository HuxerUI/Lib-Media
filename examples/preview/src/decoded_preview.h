#pragma once

#include <functional>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include <huxerui/external_texture.h>
#include <huxerui/file.h>
#include <huxerui/media_container.h>
#include <huxerui/resource.h>
#include <huxerui/task.h>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

namespace media_preview {
#if defined(__APPLE__) && TARGET_OS_OSX
inline constexpr bool preview_reordered_export = true;
#else
inline constexpr bool preview_reordered_export = false;
#endif

#if defined(__ANDROID__) || defined(__APPLE__) || defined(__EMSCRIPTEN__) || defined(_WIN32) || defined(__linux__)
inline constexpr bool frame_texture_supported = true;
#else
inline constexpr bool frame_texture_supported = false;
#endif

#if defined(__ANDROID__) || (defined(_WIN32) && !defined(HUXERUI_WINDOWS_7_COMPAT)) || (defined(__APPLE__) && (TARGET_OS_OSX || TARGET_OS_IOS))
inline constexpr bool gpu_video_scaling_supported = true;
#else
inline constexpr bool gpu_video_scaling_supported = false;
#endif

#ifdef __EMSCRIPTEN__
inline constexpr bool gpu_video_supported = true;
#else
inline constexpr bool gpu_video_supported = gpu_video_scaling_supported;
#endif

// Creates a texture for one preview run; publication copies or retains pixels through the public SDK.
std::shared_ptr<huxerui::ExternalTexture> MakeFrameTexture(huxerui::Size size,
    huxerui::media::VideoStorage storage = huxerui::media::VideoStorage::Cpu);
// Copies the visible CPU YUV region into owned RGBA bytes using the preview's SDR color policy.
huxerui::Bytes ConvertFramePixels(const huxerui::media::VideoFrame& frame);
inline std::pair<unsigned, unsigned> DisplayDimensions(
    huxerui::media::VideoRectangle area, huxerui::media::VideoRotation rotation) {
  using huxerui::media::VideoRotation;
  if (rotation == VideoRotation::Clockwise90 || rotation == VideoRotation::Clockwise270)
    return {area.height, area.width};
  return {area.width, area.height};
}
void PublishFrame(huxerui::ExternalTexture& texture, const huxerui::media::VideoFrame& frame);
class PreviewAudioUse;
#if defined(__APPLE__) && TARGET_OS_IOS
inline constexpr bool preview_audio_session_supported = true;
std::shared_ptr<PreviewAudioUse> RetainPreviewAudioSession(bool activate);
#else
inline constexpr bool preview_audio_session_supported = false;
inline std::shared_ptr<PreviewAudioUse> RetainPreviewAudioSession(bool) { return {}; }
#endif

struct ClipControls {
  std::function<void(huxerui::Size, const std::shared_ptr<huxerui::ExternalTexture>&)> reset_video;
  std::function<void(const huxerui::media::VideoFrame&)> publish;
  std::function<bool()> paused;
  std::function<std::optional<float>()> take_seek;
  std::function<void(float position, float duration)> position;
  std::function<void(bool can_copy, const std::optional<huxerui::media::MediaAudioMetadata>&)> audio_info;
};

template<class T, class E> T Require(huxerui::Result<T, E> result) {
  if (!result.Succeeded()) throw std::runtime_error(result.Error().message);
  if constexpr (!std::is_void_v<T>) return std::move(result).Value();
}

template<class Operation> huxerui::Task<std::invoke_result_t<Operation>> OnFileThread(Operation operation) {
#ifdef __EMSCRIPTEN__
  co_return operation();
#else
  co_return co_await huxerui::RunWorker(std::move(operation));
#endif
}

huxerui::media::MediaDemuxer Open(const huxerui::File& file);
std::int64_t Ticks(huxerui::media::MediaTimestamp time, std::uint32_t scale);
std::int64_t CeilTicks(huxerui::media::MediaTimestamp time, std::uint32_t scale);
float Seconds(huxerui::media::MediaTimestamp time);
huxerui::media::MediaTimestamp TimeAt(float seconds);
bool TrimAudio(huxerui::media::AudioFrame& frame, huxerui::media::MediaTimestamp target,
               std::optional<huxerui::media::MediaTimestamp> end);
std::shared_ptr<const huxerui::File> TemporaryFile(const huxerui::File& directory, std::string_view suffix);

// Staged inputs and completed exports are removed when their last owner releases them.
huxerui::Task<std::shared_ptr<const huxerui::File>> StageClip(
    huxerui::File temporary, std::optional<huxerui::FileReference> selected,
    std::shared_ptr<huxerui::Resources> resources);
huxerui::Task<void> PlayContainer(std::shared_ptr<const huxerui::File> file, ClipControls controls,
    huxerui::media::VideoStorage storage = huxerui::media::VideoStorage::Cpu);
huxerui::Task<void> PlayAudio(std::shared_ptr<const huxerui::File> file, ClipControls controls);
huxerui::Task<std::shared_ptr<const huxerui::File>> CopyAudioClip(
    std::shared_ptr<const huxerui::File> file, huxerui::File temporary);
huxerui::Task<std::shared_ptr<const huxerui::File>> ExportWave(
    std::shared_ptr<const huxerui::File> file, huxerui::File temporary);
huxerui::Task<std::shared_ptr<const huxerui::File>> CopyClip(
    std::shared_ptr<const huxerui::File> file, huxerui::File temporary,
    std::optional<huxerui::media::ContainerFormat> format = {});
huxerui::Task<std::shared_ptr<const huxerui::File>> ExportClip(
    std::shared_ptr<const huxerui::File> file, huxerui::File temporary, bool reorder = false,
    huxerui::media::VideoStorage storage = huxerui::media::VideoStorage::Cpu);
}
