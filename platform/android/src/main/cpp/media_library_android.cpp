#include <huxerui/android/platform_registry.h>
#include <huxerui/android/external_texture.h>
#include "library/backend.h"

extern "C" JNIEXPORT jobject JNICALL
Java_org_huxerui_lib_media_MediaLibraryBrowser_imagePayload(
    JNIEnv* env, jclass, jobject bitmap, jint width, jint height) {
  try {
    auto texture = std::make_shared<huxerui::android::BitmapTexture>(
        huxerui::Size{static_cast<float>(width), static_cast<float>(height)});
    texture->Publish(env, bitmap);
    texture->Finish();
    return huxerui::android::PlatformPayloadToJava(env, huxerui::PlatformPayload(std::move(texture))).Release();
  } catch (const std::exception& error) {
    if (!env->ExceptionCheck()) {
      auto type = env->FindClass("java/io/IOException");
      if (type) env->ThrowNew(type, error.what());
    }
    return nullptr;
  }
}

namespace huxerui::media::detail {
void InstallMediaLibrary(ApplicationContext& context) {
  android::JavaWindowPlatformModuleFactory<PlatformChannel> factory;
  factory.class_name = "org.huxerui.lib.media.MediaLibraryModule$Factory";
  factory.create = [](PlatformChannel channel) { return channel; };
  context.RegisterPlatformModule<PlatformChannel>(media_library_type, std::move(factory));
}
} // namespace huxerui::media::detail
