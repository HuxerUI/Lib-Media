#include <huxerui/web/platform_registry.h>

#include "player/channel_player.h"
#include "player/state.h"

namespace huxerui::media::detail {
namespace {

using emscripten::val;

val Properties(const VideoSurfaceProperties& properties) {
  const auto payload = EncodedVideoProperties::Encode({properties});
  auto value = val::object();
  for (const auto& [name, field] : payload.AsObject())
    value.set(name, std::string(field.AsString()));
  return value;
}

class WebPlayer final : public ChannelPlayer {
public:
  WebPlayer(PlatformChannel channel, val holder) : ChannelPlayer(std::move(channel)), holder_(std::move(holder)) {}
  void Play() override {
    holder_["instance"].call<void>("playNow");
  }
  void Pause() override {
    holder_["instance"].call<void>("pauseNow");
  }
  void Attach(val host, const VideoSurfaceProperties& properties) {
    holder_["instance"].call<void>("attach", host, Properties(properties));
  }
  void Detach() {
    holder_["instance"].call<void>("detach");
  }

private:
  val holder_;
};

struct WebVideo {
  val host = val::undefined();
  VideoSurfaceProperties properties;
  std::weak_ptr<WebPlayer> player;
};

} // namespace

void InstallPlatform(ApplicationContext& context) {
  context.RegisterPlatformModule<std::shared_ptr<PlayerBackend>>(player_type, [](UiWindow& ui_window) {
    auto bundle = val::module_property("createMediaPlayerBundle")();
    web::JavaScriptPlatformModuleFactory<std::shared_ptr<PlayerBackend>> factory{
        .factory = bundle["factory"],
        .create = [holder = bundle["holder"]](PlatformChannel channel) -> std::shared_ptr<PlayerBackend> {
          auto player = std::make_shared<WebPlayer>(std::move(channel), holder);
          player->Connect();
          return player;
        },
    };
    return factory(ui_window);
  });
  web::PlatformViewFactory<VideoSurfaceProperties, WebVideo, MediaPlayer> factory{
      .create =
          [](const VideoSurfaceProperties& properties, PlatformEventEmitter) {
            auto view = std::make_shared<WebVideo>();
            view->properties = properties;
            view->host = val::global("document").call<val>("createElement", std::string("div"));
            view->host["style"].set("background", "black");
            view->host["style"].set("overflow", "hidden");
            return view;
          },
      .view = [](const std::shared_ptr<WebVideo>& view) { return view->host; },
      .update =
          [](WebVideo& view, const VideoSurfaceProperties& properties) {
            view.properties = properties;
            if (auto player = view.player.lock())
              player->Attach(view.host, properties);
          },
      .dispose = [](WebVideo& view) { view.host.call<void>("remove"); },
      .connect =
          [](WebVideo& view, const MediaPlayer& player) {
            PlayerAccess::Bind(player, [&view](const std::shared_ptr<PlayerBackend>& backend) {
              if (auto old = view.player.lock())
                old->Detach();
              view.player.reset();
              if (backend) {
                auto current = std::static_pointer_cast<WebPlayer>(backend);
                view.player = current;
                current->Attach(view.host, view.properties);
              }
            });
          },
      .disconnect = [](WebVideo&, const MediaPlayer& player) { PlayerAccess::Unbind(player); },
  };
  context.RegisterPlatformView<VideoSurfaceProperties, MediaPlayer>(video_type, std::move(factory));
}

} // namespace huxerui::media::detail
