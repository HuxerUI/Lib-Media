#include <gtk/gtk.h>

#include <limits>

#include <huxerui/linux/external_texture.h>
#include <huxerui/window.h>

#include "player/state.h"
#include "player/video_geometry.h"

namespace huxerui::media::detail {
namespace {

class LinuxPlayer final : public PlayerBackend {
public:
  ~LinuxPlayer() override {
    Clear();
  }

  void Load(const MediaSource& source, std::uint64_t generation) override {
    Clear();
    generation_ = generation;
    source_ = source;
    GFile* file = nullptr;
    const auto& input = PlayerAccess::Source(source);
    if (auto http = std::get_if<MediaHttpSource>(&input)) {
      if (!http->headers.empty()) {
        Error(MediaErrorCode::Unsupported, "Custom HTTP headers are unavailable in the GTK media backend");
        return;
      }
      file = g_file_new_for_uri(http->url.c_str());
    } else {
      auto local = std::get_if<File>(&input) ? std::optional<File>(std::get<File>(input))
                                             : std::get<FileReference>(input).AsFile();
      if (!local) {
        Error(MediaErrorCode::Unsupported, "The file reference has no GTK-readable local path");
        return;
      }
      file = g_file_new_for_path(local->Path().c_str());
    }
    media_ = GTK_MEDIA_FILE(gtk_media_file_new());
    g_signal_connect(media_, "notify", G_CALLBACK(+[](GObject*, GParamSpec* spec, gpointer data) {
                       static_cast<LinuxPlayer*>(data)->Notify(spec->name);
                     }),
                     this);
    g_signal_connect(media_, "invalidate-contents",
                     G_CALLBACK(+[](GdkPaintable*, gpointer data) { static_cast<LinuxPlayer*>(data)->PublishFrame(); }),
                     this);
    gtk_media_stream_set_volume(Stream(), volume_);
    gtk_media_stream_set_muted(Stream(), muted_);
    gtk_media_file_set_file(media_, file);
    g_object_unref(file);
  }

  void Clear() noexcept override {
    if (media_) {
      g_signal_handlers_disconnect_by_data(media_, this);
      gtk_media_stream_pause(Stream());
      gtk_media_file_clear(media_);
      g_clear_object(&media_);
    }
    if (renderer_) {
      if (gsk_renderer_is_realized(renderer_))
        gsk_renderer_unrealize(renderer_);
      g_clear_object(&renderer_);
    }
    if (texture_)
      texture_->Finish();
    texture_.reset();
    source_.reset();
    ready_ = false;
    failed_ = false;
    seek_id_ = 0;
  }
  void Play() override {
    if (media_)
      gtk_media_stream_play(Stream());
  }
  void Pause() override {
    if (media_)
      gtk_media_stream_pause(Stream());
  }
  void Seek(MediaTime position, std::uint64_t seek_id) override {
    seek_id_ = seek_id;
    if (position.count() > static_cast<double>(std::numeric_limits<gint64>::max()) / G_USEC_PER_SEC) {
      if (emit)
        emit({.generation = generation_,
              .kind = PlayerEventKind::Error,
              .seek_id = seek_id,
              .error = {MediaErrorCode::SeekFailed, "HuxerUI GTK seek time exceeds the platform range", {}, false}});
      return;
    }
    gtk_media_stream_seek(Stream(), static_cast<gint64>(position.count() * G_USEC_PER_SEC));
    if (!gtk_media_stream_is_seeking(Stream()))
      CompleteSeek();
  }
  void SetVolume(double volume, bool muted) override {
    volume_ = volume;
    muted_ = muted;
    if (media_) {
      gtk_media_stream_set_volume(Stream(), volume);
      gtk_media_stream_set_muted(Stream(), muted);
    }
  }
  void Poll() override {
    if (ready_)
      Send(PlayerEventKind::Update);
  }
  std::shared_ptr<ExternalTexture> Texture() override {
    return texture_;
  }

private:
  GtkMediaStream* Stream() const {
    return GTK_MEDIA_STREAM(media_);
  }
  PlayerSnapshot Snapshot() const {
    PlayerSnapshot value;
    if (!media_)
      return value;
    value.progress.position = MediaTime(static_cast<double>(gtk_media_stream_get_timestamp(Stream())) / G_USEC_PER_SEC);
    const auto duration = gtk_media_stream_get_duration(Stream());
    if (duration > 0)
      value.progress.duration = MediaTime(static_cast<double>(duration) / G_USEC_PER_SEC);
    if (ready_) {
      value.seekability = gtk_media_stream_is_seekable(Stream()) && duration > 0 ? MediaSeekability::Seekable
                                                                                 : MediaSeekability::NotSeekable;
      value.has_audio = gtk_media_stream_has_audio(Stream()) != FALSE;
      value.has_video = gtk_media_stream_has_video(Stream()) != FALSE;
      const int width = gdk_paintable_get_intrinsic_width(GDK_PAINTABLE(media_));
      const int height = gdk_paintable_get_intrinsic_height(GDK_PAINTABLE(media_));
      if (width > 0 && height > 0)
        value.video_size = Size{static_cast<float>(width), static_cast<float>(height)};
    }
    return value;
  }
  void Send(PlayerEventKind kind, std::uint64_t seek_id = 0) {
    if (emit)
      emit({.generation = generation_, .kind = kind, .snapshot = Snapshot(), .seek_id = seek_id});
  }
  void Error(MediaErrorCode code, const char* message, std::string platform_code = {}) {
    if (failed_)
      return;
    failed_ = true;
    if (emit)
      emit({.generation = generation_,
            .kind = PlayerEventKind::Error,
            .error = {code, std::string("HuxerUI media: ") + message, std::move(platform_code), true}});
  }
  void CompleteSeek() {
    if (const auto id = std::exchange(seek_id_, 0))
      Send(PlayerEventKind::Seeked, id);
  }
  void Notify(const char* property) {
    if (!media_ || failed_)
      return;
    const std::string_view name(property);
    if (name == "error") {
      if (const auto* error = gtk_media_stream_get_error(Stream())) {
        MediaErrorCode code = MediaErrorCode::Unknown;
        if (error->domain == G_IO_ERROR) {
          if (error->code == G_IO_ERROR_NOT_FOUND)
            code = MediaErrorCode::NotFound;
          if (error->code == G_IO_ERROR_PERMISSION_DENIED)
            code = MediaErrorCode::PermissionDenied;
          if (error->code == G_IO_ERROR_NOT_SUPPORTED)
            code = MediaErrorCode::Unsupported;
        }
        Error(code, "GTK could not play the media source", std::to_string(error->code));
      }
    } else if (name == "prepared" && gtk_media_stream_is_prepared(Stream()) && !ready_) {
      ready_ = true;
      Send(PlayerEventKind::Ready);
    } else if (name == "ended" && gtk_media_stream_get_ended(Stream())) {
      Send(PlayerEventKind::Ended);
    } else if (name == "playing" && ready_) {
      Send(gtk_media_stream_get_playing(Stream()) ? PlayerEventKind::Playing : PlayerEventKind::Paused);
    } else if (name == "seeking" && !gtk_media_stream_is_seeking(Stream()))
      CompleteSeek();
  }
  void PublishFrame() {
    if (!media_ || failed_)
      return;
    const auto width = gdk_paintable_get_intrinsic_width(GDK_PAINTABLE(media_));
    const auto height = gdk_paintable_get_intrinsic_height(GDK_PAINTABLE(media_));
    if (width <= 0 || height <= 0)
      return;
    auto* snapshot = gtk_snapshot_new();
    gdk_paintable_snapshot(GDK_PAINTABLE(media_), GDK_SNAPSHOT(snapshot), width, height);
    auto* node = gtk_snapshot_free_to_node(snapshot);
    if (!node)
      return;
    ::GdkTexture* frame = nullptr;
    if (gsk_render_node_get_node_type(node) == GSK_TEXTURE_NODE) {
      frame = GDK_TEXTURE(g_object_ref(gsk_texture_node_get_texture(node)));
    } else {
      if (!renderer_) {
        renderer_ = gsk_gl_renderer_new();
        GError* error = nullptr;
        if (!gsk_renderer_realize_for_display(renderer_, gdk_display_get_default(), &error)) {
          if (error)
            g_error_free(error);
          gsk_render_node_unref(node);
          Error(MediaErrorCode::Output, "GTK could not create the video texture renderer");
          return;
        }
      }
      graphene_rect_t viewport = GRAPHENE_RECT_INIT(0, 0, static_cast<float>(width), static_cast<float>(height));
      frame = gsk_renderer_render_texture(renderer_, node, &viewport);
    }
    gsk_render_node_unref(node);
    if (!frame) {
      Error(MediaErrorCode::Output, "GTK could not render a video frame");
      return;
    }
    const Size size{static_cast<float>(width), static_cast<float>(height)};
    const bool changed = !texture_ || texture_->IntrinsicSize() != size;
    if (changed) {
      if (texture_)
        texture_->Finish();
      texture_ = std::make_shared<huxerui::linux::GdkTexture>(size);
    }
    texture_->Publish(frame);
    g_object_unref(frame);
    if (changed)
      Send(PlayerEventKind::Update);
  }

  GtkMediaFile* media_ = nullptr;
  GskRenderer* renderer_ = nullptr;
  std::shared_ptr<huxerui::linux::GdkTexture> texture_;
  std::optional<MediaSource> source_;
  std::uint64_t generation_ = 0, seek_id_ = 0;
  double volume_ = 1;
  bool muted_ = false, ready_ = false, failed_ = false;
};

} // namespace

void InstallPlatform(ApplicationContext& context) {
  context.RegisterPlatformModule<std::shared_ptr<PlayerBackend>>(
      player_type, [](UiWindow&) -> std::shared_ptr<PlayerBackend> { return std::make_shared<LinuxPlayer>(); });
}

} // namespace huxerui::media::detail

namespace huxerui::media {

View VideoSurface(MediaPlayer player, VideoSurfaceProperties properties) {
  detail::ValidateVideoProperties(properties);
  return Scope([player, properties] {
    auto state = detail::PlayerAccess::State(player);
    Lifecycle(
        [player] {
          detail::PlayerAccess::Bind(player, [](const auto&) {});
          return [player] { detail::PlayerAccess::Unbind(player); };
        },
        player);
    std::shared_ptr<ExternalTexture> texture = state->observed_texture;
    return Canvas([texture, properties](PaintContext& paint, Size size) {
      if (!texture)
        return;
      paint.PushClip({0, 0, size.width, size.height});
      paint.DrawImage(texture, detail::FitVideo(texture->IntrinsicSize(), size, properties));
      paint.PopClip();
    });
  });
}

} // namespace huxerui::media
