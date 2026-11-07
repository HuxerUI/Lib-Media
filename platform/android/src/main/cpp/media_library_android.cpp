#include <huxerui/android/platform_registry.h>
#include "library/backend.h"

namespace huxerui::media::detail {
void InstallMediaLibrary(ApplicationContext& context) {
  android::JavaWindowPlatformModuleFactory<PlatformChannel> factory;
  factory.class_name = "org.huxerui.lib.media.MediaLibraryModule$Factory";
  factory.create = [](PlatformChannel channel) { return channel; };
  context.RegisterPlatformModule<PlatformChannel>(media_library_type, std::move(factory));
}
} // namespace huxerui::media::detail
