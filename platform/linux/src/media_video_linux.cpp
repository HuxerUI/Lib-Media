#include "format/video_frame_transformer.h"

#include <huxerui/linux/external_texture.h>
#include <gtk/gtk.h>

namespace huxerui::media::detail {

VideoTextureResult PublishVideoPixels(Bytes pixels, VideoTextureOptions options) {
  const auto release = [](gpointer value) { delete static_cast<Bytes*>(value); };
  auto owned = std::make_unique<Bytes>(std::move(pixels));
  auto* data = g_bytes_new_with_free_func(owned->data(), owned->size(), release, owned.get());
  owned.release();
  const auto bytes = std::unique_ptr<GBytes, decltype(&g_bytes_unref)>(data, g_bytes_unref);
  const auto unref = [](GdkTexture* value) { g_object_unref(value); };
  const auto image = std::unique_ptr<GdkTexture, decltype(unref)>(gdk_memory_texture_new(
      int(options.width), int(options.height), GDK_MEMORY_R8G8B8A8, bytes.get(), std::size_t(options.width) * 4), unref);
  auto texture = std::make_shared<linux::GdkTexture>(Size{float(options.width), float(options.height)});
  texture->Publish(image.get());
  texture->Finish();
  return VideoTextureResult::Success(std::move(texture));
}

} // namespace huxerui::media::detail
