#include <huxerui/android/platform_registry.h>

#include "media_video_android.h"
#include "player/channel_player.h"
#include "player/state.h"

namespace huxerui::media::detail {

void InstallPlatform(ApplicationContext& context) {
  InstallSurfacePreview(context);
  android::JavaPlatformModuleFactory<std::shared_ptr<PlayerBackend>> module;
  module.class_name = "org.huxerui.lib.media.MediaPlayerModule$Factory";
  module.create = [](PlatformChannel channel) -> std::shared_ptr<PlayerBackend> {
    auto player = std::make_shared<ChannelPlayer>(std::move(channel));
    player->Connect();
    return player;
  };
  context.RegisterPlatformModule<std::shared_ptr<PlayerBackend>>(player_type, std::move(module));
  android::JavaPlatformViewFactory<EncodedVideoProperties, MediaPlayer> view{
      .class_name = "org.huxerui.lib.media.VideoSurfaceView$Factory",
      .connect =
          [](const MediaPlayer& player, PlatformChannel channel) {
            auto current = std::make_shared<std::weak_ptr<PlayerBackend>>();
            PlayerAccess::Bind(player, [channel, current](const std::shared_ptr<PlayerBackend>& backend) {
              if (auto old = current->lock())
                old->UnbindVideo();
              *current = backend;
              if (backend)
                backend->BindVideo(channel);
            });
          },
      .disconnect = [](const MediaPlayer& player) { PlayerAccess::Unbind(player); },
  };
  context.RegisterPlatformView<EncodedVideoProperties, MediaPlayer>(video_type, std::move(view));
}

} // namespace huxerui::media::detail
