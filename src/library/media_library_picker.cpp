#include <huxerui/media_library.h>
#include <huxerui/media_player.h>
#include <huxerui/animation.h>
#include <huxerui/gesture.h>
#include <huxerui/navigation.h>
#include <huxerui/presentation.h>
#include <huxerui/resource.h>
#include <huxerui/scroll.h>
#include <huxerui/theme.h>
#include <huxerui/window.h>
#include <media_resources.h>

#include "library/access.h"
#include "library/payload.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <unordered_set>
#if defined(__ANDROID__)
#include <android/api-level.h>
#endif
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

namespace huxerui::media {
namespace {

constexpr std::size_t page_size = 60;
constexpr std::size_t grid_prefetch_rows = 3;
using Texture = std::shared_ptr<ExternalTexture>;
namespace labels = ::media::strings;

enum class PickerIcon { Close, Back, Expand, Check, Play, Pause, Library };

ImageVariant Icon(PickerIcon kind) {
  auto build = [](PickerIcon kind) {
    return VectorAsset::Create({24, 24}, [kind](VectorBuilder& builder) {
      Path path;
      switch (kind) {
        case PickerIcon::Close:
          path.MoveTo({6, 6}).LineTo({18, 18}).MoveTo({18, 6}).LineTo({6, 18});
          break;
        case PickerIcon::Back:
          path.MoveTo({15, 5}).LineTo({8, 12}).LineTo({15, 19});
          break;
        case PickerIcon::Expand:
          path.MoveTo({6, 9}).LineTo({12, 15}).LineTo({18, 9});
          break;
        case PickerIcon::Check:
          path.MoveTo({5, 12}).LineTo({10, 17}).LineTo({19, 7});
          break;
        case PickerIcon::Play:
          path.MoveTo({8, 5}).LineTo({19, 12}).LineTo({8, 19}).Close();
          builder.FillPath(path, Color::White());
          return;
        case PickerIcon::Pause:
          path.MoveTo({9, 5}).LineTo({9, 19}).MoveTo({15, 5}).LineTo({15, 19});
          break;
        case PickerIcon::Library:
          path = Path::RoundedRect({3, 4, 18, 16}, CornerRadii(3));
          path.MoveTo({4, 17}).LineTo({9, 12}).LineTo({13, 16}).LineTo({16, 13}).LineTo({20, 17});
          path.MoveTo({15, 9}).LineTo({15.1F, 9});
          break;
      }
      builder.StrokePath(path, Color::White(), {.width = 2, .cap = StrokeCap::Round, .join = StrokeJoin::Round});
    });
  };
  static const std::array icons{build(PickerIcon::Close), build(PickerIcon::Back), build(PickerIcon::Expand),
      build(PickerIcon::Check), build(PickerIcon::Play), build(PickerIcon::Pause), build(PickerIcon::Library)};
  return icons[static_cast<std::size_t>(kind)];
}

ButtonStyle DefaultPickerButton(ButtonStyle style, bool primary, const ThemeSpec& theme) {
  const auto& colors = theme.colors;
  style.background = primary ? colors.primary : colors.surface_container_high;
  style.label_style.foreground = primary ? colors.on_primary : colors.on_surface;
  style.disabled_background = colors.surface_container_highest;
  style.disabled_label = colors.on_surface_variant;
  style.minimum_height = 44;
  style.padding = EdgeInsets::Symmetric(20, 8);
  style.corner_radii = CornerRadii(theme.shapes.small);
  return style;
}

View PickerButton(StringVariant label, std::function<void()> action, bool enabled, const ButtonStyle& style) {
  ThemeDefinition override;
  override.Set(style);
  return Theme(std::move(override), Button(std::move(label)).OnClick(std::move(action)).With(Enabled{enabled}));
}

struct SelectionColors {
  Color outline;
  Color background;
  Color foreground;
};

ButtonStyle SecondaryButtonStyle(const MediaLibraryPickerStyle& style, const ButtonStyle& inherited,
                                 const ThemeSpec& theme) {
  if (style.secondary_button) return *style.secondary_button;
  return DefaultPickerButton(inherited, false, theme);
}

struct Dismissed : Event<void()> {};

struct FinishDismissal {
  bool closing;

  class Extension final : public NodeExtension {
  public:
    Extension(ViewNode& node, const FinishDismissal& value) { Update(node, value); }

    void Update(ViewNode&, const FinishDismissal& value) {
      if (closing_ != value.closing) reported_ = false;
      closing_ = value.closing;
    }

    FrameResult OnFrame(ViewNode& node, const FrameInfo& frame) override {
      if (!closing_ || reported_) return {};
      if (!frame.reduced_motion && node.PresentationOpacity() > 0) return {.needs_frame = true};
      reported_ = true;
      EmitEvent<Dismissed>();
      return {};
    }

  private:
    bool closing_ = false;
    bool reported_ = false;
  };

  bool operator==(const FinishDismissal&) const = default;
};

struct ObserveSize {
  State<Size> size;

  class Extension final : public NodeExtension {
  public:
    Extension(ViewNode&, const ObserveSize& value) : size_(value.size) {}
    void Update(ViewNode&, const ObserveSize& value) { size_ = value.size; }

    FrameResult OnFrame(ViewNode& node, const FrameInfo&) override {
      size_ = node.LayoutSize();
      return {};
    }

  private:
    State<Size> size_;
  };
};

[[huxerui::composable]]
View AnimatedPanel(View content, bool open, Point offset, std::function<void()> dismissed) {
  auto mounted = UseState(false);
  Lifecycle([mounted] { mounted = true; });
  const auto& motion = UseTheme().motion;
  const TweenSpec timing{open ? motion.normal : motion.fast, open ? Easing::EaseOut : Easing::EaseIn};
  return Stack {
    std::move(content),
  }.With(Align(HorizontalAlignment::Stretch, VerticalAlignment::Stretch),
         Transition{AnimateTo(open && mounted.Get() ? 1.0F : 0.0F, timing)}.Opacity(0, 1).Offset(offset, {}),
         FinishDismissal{!open}, Semantics{.hidden = !open})
   .On<Dismissed>(std::move(dismissed));
}

struct Thumbnails {
  struct Entry {
    Texture texture;
    std::uint64_t used;
  };
  std::map<std::pair<std::string, std::uint32_t>, Entry> entries;
  std::uint64_t clock = 0;

  Texture Get(const MediaAssetId& id, std::uint32_t pixels) {
    auto entry = entries.find({id.value, pixels});
    if (entry == entries.end()) return {};
    entry->second.used = ++clock;
    return entry->second.texture;
  }

  void Put(const MediaAssetId& id, std::uint32_t pixels, Texture texture) {
    constexpr std::size_t max_cached = 96;
    entries.insert_or_assign({id.value, pixels}, Entry{std::move(texture), ++clock});
    if (entries.size() > max_cached)
      entries.erase(std::ranges::min_element(entries, {}, [](const auto& item) { return item.second.used; }));
  }
};

struct Access {
  PermissionStatus photos = PermissionStatus::Unavailable;
  PermissionStatus videos = PermissionStatus::Unavailable;

  static bool Readable(PermissionStatus value) {
    return value == PermissionStatus::Granted || value == PermissionStatus::Limited;
  }
  static bool CanRequest(PermissionStatus value) {
    return value != PermissionStatus::Granted && value != PermissionStatus::Unavailable;
  }
  bool Readable() const { return Readable(photos) || Readable(videos); }
  bool Limited() const { return photos == PermissionStatus::Limited || videos == PermissionStatus::Limited; }
  bool NeedsSettings() const {
    return photos == PermissionStatus::PermanentlyDenied || videos == PermissionStatus::PermanentlyDenied ||
        photos == PermissionStatus::Restricted || videos == PermissionStatus::Restricted;
  }
};

Task<Access> CheckAccess(ApplicationHandle application, MediaFilter filter) {
  Access access;
  if (filter != MediaFilter::Videos) access.photos = co_await application.CheckPermissionAsync(Permission::PhotosRead);
  if (filter != MediaFilter::Images) access.videos = co_await application.CheckPermissionAsync(Permission::VideosRead);
  co_return access;
}

bool HasSeparateMediaPermissions() {
#if defined(__ANDROID__)
  return android_get_device_api_level() >= __ANDROID_API_U__;
#else
  return false;
#endif
}

std::string DurationLabel(std::chrono::milliseconds duration) {
  const auto seconds = std::max<std::int64_t>(0, std::chrono::duration_cast<std::chrono::seconds>(duration).count());
  const auto two = [](auto value) { return (value < 10 ? "0" : "") + std::to_string(value); };
  return seconds >= 3600 ? std::to_string(seconds / 3600) + ":" + two(seconds / 60 % 60) + ":" + two(seconds % 60)
                         : std::to_string(seconds / 60) + ":" + two(seconds % 60);
}

struct LoadedThumbnail {
  std::weak_ptr<Thumbnails> cache;
  MediaAssetId id;
  std::uint32_t pixels = 0;
  std::weak_ptr<ExternalTexture> texture;
};

[[huxerui::composable]]
View Thumbnail(MediaLibrary library, MediaAssetId id, std::uint32_t pixels, std::shared_ptr<Thumbnails> cache,
               ImageFit fit = ImageFit::Cover) {
  auto tasks = UseTaskScope();
  auto image = UseState(LoadedThumbnail{});
  const auto& loaded = image.Get();
  auto texture = loaded.id == id && loaded.pixels == pixels && loaded.cache.lock() == cache
      ? loaded.texture.lock() : Texture{};
  if (!texture) texture = cache->Get(id, pixels);
  const std::weak_ptr<ExternalTexture> current = texture;
  Lifecycle([=] {
    auto cached = current.lock();
    if (!cached) cached = cache->Get(id, pixels);
    TaskHandle task;
    if (cached) image = LoadedThumbnail{cache, id, pixels, cached};
    else task = tasks.Launch([=]() -> Task<void> {
      auto result = co_await library.LoadImageAsync(id, {pixels, pixels});
      if (result.Succeeded() && result.Value()) {
        auto texture = std::move(result).Value();
        cache->Put(id, pixels, texture);
        image = LoadedThumbnail{cache, id, pixels, texture};
      }
    });
    return [task] { task.Cancel(); };
  }, id, pixels, cache);
  if (texture) return Image(std::move(texture)).Fit(fit);
  return Spacer().With(Background(fit == ImageFit::Contain ? Color::Black()
                                                          : UseTheme().colors.surface_container_highest));
}

struct Zoom {
  float scale = 1;
  Point offset;
  bool operator==(const Zoom&) const = default;
};

Rect FitImage(Size viewport, Size image, Zoom zoom) {
  const float fit = std::min(viewport.width / image.width, viewport.height / image.height);
  const float width = image.width * fit * zoom.scale;
  const float height = image.height * fit * zoom.scale;
  const float max_x = std::max(0.0F, (width - viewport.width) / 2);
  const float max_y = std::max(0.0F, (height - viewport.height) / 2);
  return {(viewport.width - width) / 2 + std::clamp(zoom.offset.x, -max_x, max_x),
          (viewport.height - height) / 2 + std::clamp(zoom.offset.y, -max_y, max_y), width, height};
}

[[huxerui::composable]]
View AssetPreview(MediaLibrary library, MediaAsset asset, std::shared_ptr<Thumbnails> cache,
                  std::uint32_t pixels, State<Zoom> zoom, State<bool> pinching, bool active,
                  ButtonStyle secondary_button) {
  auto tasks = UseTaskScope();
  auto player = UseMediaPlayer();
  auto application = UseApplication();
  auto icon_style = UseEnvironment<IconButtonStyle>();
  const auto theme = UseTheme();
  auto image = UseState(cache->Get(asset.id, pixels));
  auto viewport = UseState(std::make_shared<Size>()).Get();
  auto busy = UseState(false);
  auto image_loading = UseState(true);
  auto loaded = UseState(false);
  auto status = UseState(std::optional<StringResource>{});
  auto progress = UseState(std::optional<float>{});
  auto retry = UseState(std::uint64_t{0});
  auto opening = UseState(TaskHandle{});
  const bool video = asset.kind == MediaAssetKind::Video;
  const bool sharp = zoom->scale >= 1.8F;
  Lifecycle([=] {
    if (!active) {
      opening.Get().Cancel();
      busy = false;
      progress = std::nullopt;
      if (loaded.Get()) player.Pause();
    }
  }, active);
  Lifecycle([=] {
    TaskHandle task;
    if (active && (!video || !image.Get())) {
      task = tasks.Launch([=]() -> Task<void> {
        image_loading = true;
        status = std::nullopt;
        const std::uint32_t limit = video ? 512 : sharp ? 3072 : 1536;
        auto result = co_await library.LoadImageAsync(asset.id, {limit, limit, !video},
            [progress](double value) { progress = static_cast<float>(value); });
        progress = std::nullopt;
        image_loading = false;
        if (result.Succeeded() && result.Value()) image = std::move(result).Value();
        else if (!video && !image.Get()) status = labels::picker_preview_error;
      });
    } else {
      image_loading = false;
    }
    return [task] { task.Cancel(); };
  }, asset.id, sharp, video, active, cache, retry.Get());
  player.OnError([status](const MediaError&) { status = labels::picker_playback_error; });
  application.OnLifecycleChanged([=](ApplicationLifecycleState value) {
    if (value != ApplicationLifecycleState::Active && video && loaded.Get()) player.Pause();
  });
  auto play = [=] {
    if (!active || busy.Get()) return;
    progress = std::nullopt;
    if (loaded.Get()) {
      const auto snapshot = player.Snapshot();
      if (snapshot.status == MediaPlayerStatus::Failed) {
        player.Clear();
        loaded = false;
      } else {
        if (snapshot.play_when_ready) player.Pause();
        else player.Play();
        return;
      }
    }
    busy = true;
    status = std::nullopt;
    opening = tasks.Launch([=]() -> Task<void> {
      auto result = co_await library.OpenAssetAsync(asset.id,
          [progress](double value) { progress = static_cast<float>(value); });
      busy = false;
      progress = std::nullopt;
      if (result.Succeeded()) {
        if (player.Load(MediaSource(std::move(result).Value()))) {
          loaded = true;
          if (application.LifecycleState() == ApplicationLifecycleState::Active) player.Play();
        } else status = labels::picker_playback_error;
      } else status = labels::picker_playback_error;
    });
  };
  auto seek_draft = UseState(std::optional<float>{});
  auto texture = image.Get();
  if (!texture) texture = cache->Get(asset.id, pixels);
  const Zoom transform = zoom.Get();
  auto updateZoom = [=](float scale, Point focus, Point pan) {
    auto value = zoom.Get();
    const float next = std::clamp(value.scale * scale, 1.0F, 5.0F);
    if (next <= 1.0F) { zoom = Zoom{}; return; }
    const float ratio = next / value.scale;
    const auto size = *viewport;
    value.offset.x = ratio * value.offset.x + (1 - ratio) * (focus.x - size.width / 2) + pan.x;
    value.offset.y = ratio * value.offset.y + (1 - ratio) * (focus.y - size.height / 2) + pan.y;
    value.scale = next;
    if (texture && size.width > 0 && size.height > 0) {
      auto fit = FitImage(size, texture->IntrinsicSize(), value);
      value.offset = {fit.x - (size.width - fit.width) / 2, fit.y - (size.height - fit.height) / 2};
    }
    zoom = value;
  };
  auto visual = active && loaded.Get() ? View(Stack {
      VideoSurface(player),
      // The player surface is display-only here; the pager owns touch input over the video.
      Spacer().On<ViewEvents::Pointer>([](const PointerEvent&) {}),
    }.With(Align(HorizontalAlignment::Stretch, VerticalAlignment::Stretch))) : video && texture
      ? View(Image(texture).Fit(ImageFit::Contain))
      : View(Canvas([=](PaintContext& paint, Size size) {
          *viewport = size;
          if (texture) paint.DrawImage(texture, FitImage(size, texture->IntrinsicSize(), transform));
        }));
  auto gestures = std::move(visual).With(Grow(), ClipChildren{}, Background(Color::Black()));
  if (!video && active) {
    if (transform.scale > 1.0F) {
      gestures = std::move(gestures).With(DragGesture{}).On<DragEvents::Changed>([=](const DragEvent& event) {
        const auto size = *viewport;
        updateZoom(1, {size.width / 2, size.height / 2}, event.delta);
      });
    }
    gestures = std::move(gestures).With(TransformGesture{}, MultiTapGesture{})
        .On<TransformEvents::Started>([pinching](const TransformEvent&) { pinching = true; })
        .On<TransformEvents::Ended>([pinching](const TransformEvent&) { pinching = false; })
        .On<TransformEvents::Canceled>([pinching](const TransformEvent&) { pinching = false; })
        .On<TransformEvents::Changed>([=](const TransformEvent& event) {
          updateZoom(event.scale, {event.centroid.x - event.pan.x, event.centroid.y - event.pan.y}, event.pan);
        }).On<MultiTapEvents::Recognized>([=](const MultiTapEvent& event) {
          updateZoom(zoom->scale > 1.0F ? 1.0F / zoom->scale : 2.0F, event.position, {});
        });
  }
  const auto playbackState = player.Snapshot();
  const auto& current = playbackState.progress;
  const bool playing = playbackState.play_when_ready;
  auto position = [](MediaTime value) {
    return DurationLabel(std::chrono::duration_cast<std::chrono::milliseconds>(value));
  };
  const auto duration = current.duration ? std::optional(position(*current.duration))
      : asset.duration ? std::optional(DurationLabel(*asset.duration)) : std::nullopt;
  auto play_button = IconButton(Icon(playing ? PickerIcon::Pause : PickerIcon::Play),
      playing ? labels::picker_pause : labels::picker_play).OnClick(play).With(Enabled{!busy.Get()});
  icon_style.minimum_interactive_size = 44;
  icon_style.icon_size = 20;
  ThemeDefinition control_style;
  control_style.Set(icon_style);
  auto retry_preview = [retry] { retry += 1; };
  return Column {
    std::move(gestures),
    video && active ? View(Row {
      Theme(control_style, play_button),
      Text(position(current.position), TextRole::Label),
      Slider(seek_draft->value_or(static_cast<float>(current.position.count())))
          .Range(0.0F, static_cast<float>(std::max(0.01, current.duration.value_or(MediaTime{}).count())))
          .OnChanged([seek_draft](float value) { seek_draft = value; })
          .OnCommitted([=](float value) { player.SeekTo(MediaTime(value)); seek_draft = std::nullopt; })
          .OnCanceled([seek_draft] { seek_draft = std::nullopt; })
          .With(Grow(), Enabled{playbackState.seekability == MediaSeekability::Seekable && current.duration.has_value()},
                Semantics{.label = labels::picker_video_position}),
      duration ? View(Text(*duration, TextRole::Label)) : View{},
    }.With(Spacing(8), Padding(EdgeInsets::Symmetric(12, 4)), CrossAlign(CrossAxisAlignment::Center),
           Background(theme.colors.surface))) : View{},
    !active ? View{} : progress.Get() ? View(ProgressBar(*progress.Get()))
        : (busy.Get() || (image_loading.Get() && !texture)) ? View(ProgressBar()) : View{},
    active && status.Get() ? View(Row {
      Text(*status.Get()).With(Grow(), Semantics{.live_region = SemanticLiveRegion::Polite}),
      PickerButton(labels::picker_retry, video ? std::function<void()>(play) : retry_preview,
                   !busy.Get(), secondary_button),
    }.With(Padding(12), Spacing(8), CrossAlign(CrossAxisAlignment::Center))) : View{},
  }.With(CrossAlign(CrossAxisAlignment::Stretch));
}

struct PickerSession {
  explicit PickerSession(MediaLibrary value) : library(value) {}
  MediaLibrary library;
  std::vector<MediaAsset> assets;
  std::optional<std::string> cursor;
  std::optional<MediaAlbumId> album;
  std::vector<MediaAlbum> albums;
  std::vector<MediaAsset> selected;
  std::optional<MediaAsset> preview;
  std::vector<MediaAsset> preview_selection;
  std::uint64_t preview_session = 0;
  bool preview_from_selection = false;
  bool preview_open = false;
  std::shared_ptr<Thumbnails> thumbnails = std::make_shared<Thumbnails>();
  std::uint64_t revision = 0;
  Access access;
  bool checking = true;
  bool supported = true;
  bool switching_album = false;
  bool album_switch_failed = false;
  std::optional<MediaAlbum> requested_album;
  StringVariant album_name = labels::picker_recents;
  TaskHandle album_switch;
  bool loading = false;
  bool loading_albums = false;
  bool albums_ready = false;
  bool requesting_access = false;
  bool submitting = false;
  bool finished = false;
  bool retired = false;
  std::optional<float> progress;
  std::optional<StringResource> message;
  bool page_failed = false;
  bool albums_failed = false;
  TaskHandle page;
  TaskHandle album_request;
  TaskHandle confirmation;

  const std::vector<MediaAsset>& PreviewItems() const { return preview_from_selection ? preview_selection : assets; }
};

struct PickerActions {
  std::shared_ptr<PickerSession> session;
  State<std::uint64_t> updates;
  TaskScope tasks;
  ScrollController scroll;
  ApplicationHandle application;
  MediaFilter filter;
  std::uint32_t limit;
  std::size_t columns;
  float row_stride;

  void Update() const { updates += 1; }

  void Preview(const MediaAsset& asset, bool from_selection = false) const {
    if (session->submitting || session->checking || session->switching_album || session->finished) return;
    if (session->preview && (!from_selection || !session->preview_open || session->preview->id == asset.id)) return;
    session->preview = asset;
    session->preview_from_selection = from_selection;
    if (from_selection) session->preview_selection = session->selected;
    session->preview_open = true;
    ++session->preview_session;
    Update();
  }

  void ClearPreview() const {
    session->preview_open = false;
    session->preview.reset();
    session->preview_selection.clear();
  }

  void SwitchAlbum(std::optional<MediaAlbum> album) const {
    auto& state = *session;
    const auto id = album ? std::optional(album->id) : std::nullopt;
    if (state.switching_album || state.checking || state.submitting || state.finished || id == state.album) return;
    state.page.Cancel();
    state.loading = false;
    state.page_failed = false;
    state.requested_album = std::move(album);
    state.album_switch_failed = false;
    state.switching_album = true;
    state.message.reset();
    Update();
    state.album_switch = tasks.Launch([actions = *this, id]() -> Task<void> {
      auto& state = *actions.session;
      auto result = co_await state.library.ListAssetsAsync({actions.filter, id, page_size, {}});
      state.switching_album = false;
      if (result.Succeeded()) {
        auto page = std::move(result).Value();
        state.album = id;
        state.album_name = state.requested_album ? StringVariant(state.requested_album->name)
                                                : StringVariant(labels::picker_recents);
        state.requested_album.reset();
        state.assets = std::move(page.assets);
        state.cursor = std::move(page.next_cursor);
        actions.scroll.ScrollTo(0);
      } else if (result.Error().code == MediaLibraryErrorCode::PermissionDenied) {
        actions.Refresh();
        co_return;
      } else {
        state.album_switch_failed = true;
      }
      actions.Update();
    });
  }

  void Refresh(bool check_capability = false) const {
    auto& state = *session;
    state.page.Cancel();
    state.album_request.Cancel();
    state.album_switch.Cancel();
    state.switching_album = false;
    state.album_switch_failed = false;
    state.albums_ready = false;
    state.loading_albums = false;
    std::optional<MediaAssetId> anchor;
    const auto previous_offset = scroll.Offset();
    const auto row = static_cast<std::size_t>(previous_offset / row_stride);
    const auto first_visible = std::min(row * columns, state.assets.size());
    auto target = first_visible + page_size;
    if (state.preview && !state.preview_from_selection) {
      const auto item = std::ranges::find(state.assets, state.preview->id, &MediaAsset::id);
      target = std::max(target, static_cast<std::size_t>(std::distance(state.assets.begin(), item)) + page_size);
    }
    target = std::min(target, state.assets.size());
    if (first_visible < state.assets.size()) anchor = state.assets[first_visible].id;
    ++state.revision;
    state.thumbnails = std::make_shared<Thumbnails>();
    state.loading = true;
    state.checking = true;
    state.message.reset();
    state.page_failed = false;
    state.albums_failed = false;
    Update();
    state.page = tasks.Launch([actions = *this, target, anchor, previous_offset, check_capability]() -> Task<void> {
      auto& state = *actions.session;
      if (check_capability) {
        auto capabilities = co_await state.library.CapabilitiesAsync();
        state.supported = capabilities.Succeeded() && capabilities.Value().can_browse;
        if (!capabilities.Succeeded()) state.page_failed = true;
      }
      if (state.supported) state.access = co_await CheckAccess(actions.application, actions.filter);
      if (!state.supported || !state.access.Readable()) {
        state.assets.clear();
        state.cursor.reset();
        state.selected.clear();
        actions.ClearPreview();
      } else {
        std::vector<MediaAssetId> check;
        check.reserve(state.selected.size() + state.preview_selection.size() + 1);
        for (const auto& item : state.selected) check.push_back(item.id);
        for (const auto& item : state.preview_selection) check.push_back(item.id);
        if (state.preview) check.push_back(state.preview->id);
        if (!check.empty()) {
          auto visible = co_await state.library.FindAssetsAsync(std::move(check));
          if (visible.Succeeded()) {
            std::unordered_set<std::string> ids;
            for (const auto& asset : visible.Value()) ids.insert(asset.id.value);
            const auto removed = std::erase_if(state.selected, [&](const MediaAsset& item) {
              return !ids.contains(item.id.value);
            });
            std::erase_if(state.preview_selection, [&](const MediaAsset& item) { return !ids.contains(item.id.value); });
            if (state.preview && !ids.contains(state.preview->id.value)) actions.ClearPreview();
            if (removed) state.message = labels::picker_removed;
          }
        }
        MediaAssetQuery query{.filter = actions.filter, .album_id = state.album, .page_size = page_size};
        std::vector<MediaAsset> assets;
        bool retried = false;
        while (true) {
          auto result = co_await state.library.ListAssetsAsync(query);
          if (!result.Succeeded()) {
            if (!retried && (result.Error().code == MediaLibraryErrorCode::StaleCursor ||
                            (result.Error().code == MediaLibraryErrorCode::NotFound && query.album_id))) {
              retried = true;
              if (result.Error().code == MediaLibraryErrorCode::NotFound) {
                state.album.reset();
                state.album_name = labels::picker_recents;
                query.album_id.reset();
              }
              query.cursor.reset();
              assets.clear();
              continue;
            }
            state.page_failed = true;
            query.cursor.reset();
            break;
          }
          auto page = std::move(result).Value();
          for (auto& asset : page.assets) assets.push_back(std::move(asset));
          query.cursor = std::move(page.next_cursor);
          if (!query.cursor || assets.size() >= target) break;
        }
        state.assets = std::move(assets);
        state.cursor = std::move(query.cursor);
        if (state.preview && !state.preview_from_selection &&
            std::ranges::find(state.assets, state.preview->id, &MediaAsset::id) == state.assets.end())
          actions.ClearPreview();
        if (anchor) {
          auto index = std::ranges::find(state.assets, *anchor, &MediaAsset::id);
          if (index != state.assets.end()) {
            const float row_offset = std::distance(state.assets.begin(), index) / actions.columns *
                actions.row_stride;
            actions.scroll.ScrollTo(row_offset + std::fmod(previous_offset, actions.row_stride));
          } else actions.scroll.ScrollTo(0);
        } else actions.scroll.ScrollTo(0);
      }
      state.checking = false;
      state.loading = false;
      actions.Update();
    });
  }

  void More(bool retry = false) const {
    auto& state = *session;
    if (state.loading || state.switching_album || state.retired || state.finished || (state.page_failed && !retry)) return;
    if (!state.cursor) {
      if (retry) Refresh();
      return;
    }
    state.loading = true;
    state.page_failed = false;
    Update();
    state.page = tasks.Launch([actions = *this]() -> Task<void> {
      auto& state = *actions.session;
      auto result = co_await state.library.ListAssetsAsync({actions.filter, state.album, page_size, state.cursor});
      state.loading = false;
      if (result.Succeeded()) {
        auto page = std::move(result).Value();
        for (auto& asset : page.assets) state.assets.push_back(std::move(asset));
        state.cursor = std::move(page.next_cursor);
      } else if (result.Error().code == MediaLibraryErrorCode::StaleCursor ||
                 result.Error().code == MediaLibraryErrorCode::PermissionDenied) {
        actions.Refresh();
        co_return;
      } else {
        state.page_failed = true;
      }
      actions.Update();
    });
  }

  void Albums() const {
    auto& state = *session;
    if (state.loading_albums || state.albums_ready || state.checking) return;
    state.loading_albums = true;
    state.albums_failed = false;
    Update();
    state.album_request = tasks.Launch([actions = *this]() -> Task<void> {
      auto& state = *actions.session;
      auto result = co_await state.library.ListAlbumsAsync(actions.filter);
      state.loading_albums = false;
      if (result.Succeeded()) {
        state.albums = std::move(result).Value();
        state.albums_ready = true;
        if (state.album) {
          const auto found = std::ranges::find(state.albums, *state.album, &MediaAlbum::id);
          if (found != state.albums.end()) state.album_name = found->name;
        }
      } else {
        state.albums.clear();
        state.albums_failed = true;
      }
      actions.Update();
    });
  }

  void RequestAccess(MediaFilter requested) const {
    if (session->requesting_access) return;
    session->requesting_access = true;
    Update();
    tasks.Launch([actions = *this, requested]() -> Task<void> {
      auto& state = *actions.session;
#if defined(__APPLE__) && TARGET_OS_IOS
      if (state.access.Limited()) {
        auto result = co_await detail::MediaLibraryAccess::ManageLimitedAccessAsync(state.library);
        state.requesting_access = false;
        actions.Refresh();
        if (!result.Succeeded()) { state.message = labels::picker_access_error; actions.Update(); }
        co_return;
      }
#endif
      const auto access = state.access;
      auto request = [application = actions.application](Permission permission, PermissionStatus status) -> Task<void> {
        if (status == PermissionStatus::PermanentlyDenied || status == PermissionStatus::Restricted)
          co_await application.OpenPermissionSettingsAsync(permission);
        else if (Access::CanRequest(status))
          co_await application.RequestPermissionAsync(permission);
      };
      if (requested != MediaFilter::Videos) co_await request(Permission::PhotosRead, access.photos);
      if (requested == MediaFilter::Videos) {
        co_await request(Permission::VideosRead, access.videos);
      }
#if defined(__ANDROID__)
      else if (requested == MediaFilter::ImagesAndVideos) {
        const auto status = co_await actions.application.CheckPermissionAsync(Permission::VideosRead);
        co_await request(Permission::VideosRead, status);
      }
#endif
      state.requesting_access = false;
      actions.Refresh();
    });
  }

  void Toggle(const MediaAsset& asset) const {
    auto& state = *session;
    if (state.submitting || state.checking || state.switching_album || state.finished) return;
    auto found = std::ranges::find(state.selected, asset.id, &MediaAsset::id);
    if (found != state.selected.end()) state.selected.erase(found);
    else if (limit == 1) state.selected = {asset};
    else if (state.selected.size() < limit) state.selected.push_back(asset);
    else {
      state.message = labels::picker_limit;
      Update();
      return;
    }
    state.message.reset();
    Update();
  }

  void Confirm(std::function<void(std::vector<FileReference>)> completed) const {
    auto& state = *session;
    if (state.submitting || state.selected.empty() || state.checking || state.switching_album || state.finished) return;
    state.submitting = true;
    state.message.reset();
    std::vector<MediaAssetId> ids;
    ids.reserve(state.selected.size());
    for (const auto& item : state.selected) ids.push_back(item.id);
    state.confirmation = tasks.Launch([actions = *this, ids = std::move(ids), completed = std::move(completed)]()
        -> Task<void> {
      auto& state = *actions.session;
      std::vector<FileReference> files;
      for (const auto& id : ids) {
        auto file = co_await state.library.OpenAssetAsync(id, [actions](double value) {
          actions.session->progress = static_cast<float>(value);
          actions.Update();
        });
        state.progress.reset();
        if (!file.Succeeded()) {
          state.message = labels::picker_confirm_error;
          if (file.Error().code == MediaLibraryErrorCode::NotFound) {
            std::erase_if(state.selected, [&](const MediaAsset& item) { return item.id == id; });
            if (state.preview && state.preview->id == id) actions.ClearPreview();
            state.message = labels::picker_removed;
          }
          break;
        }
        files.push_back(std::move(file).Value());
      }
      if (files.size() == ids.size()) {
        auto visible = co_await state.library.FindAssetsAsync(ids);
        if (!visible.Succeeded()) {
          state.message = labels::picker_confirm_error;
        } else if (visible.Value().size() != ids.size()) {
          std::unordered_set<std::string> remaining;
          for (const auto& item : visible.Value()) remaining.insert(item.id.value);
          std::erase_if(state.selected, [&](const MediaAsset& item) { return !remaining.contains(item.id.value); });
          if (state.preview && std::ranges::find(ids, state.preview->id) != ids.end() &&
              !remaining.contains(state.preview->id.value)) actions.ClearPreview();
          state.message = labels::picker_removed;
        } else {
          state.finished = true;
          state.submitting = false;
          actions.Update();
          if (completed) completed(std::move(files));
          co_return;
        }
      }
      state.submitting = false;
      actions.Update();
    });
    Update();
  }

  void CancelConfirmation() const {
    session->confirmation.Cancel();
    session->submitting = false;
    session->progress.reset();
    Update();
  }
};

View SelectionBadge(std::size_t order, bool show_order, const SelectionColors& colors) {
  constexpr float diameter = 24.0F;
  const auto background = order ? colors.background : Color::Rgb(0, 0, 0, 0.44F);
  const auto content = show_order
      ? View(Text(std::to_string(order), TextRole::Label).With(FontSize(12), Foreground(colors.foreground)))
      : order ? View(Image(Icon(PickerIcon::Check)).Tint(colors.foreground).With(Frame{.width = 15, .height = 15}))
              : View{};
  return Stack {
    content,
  }.With(Frame{.width = diameter, .height = diameter}, Background(background), CornerRadius(diameter / 2),
         Border{order ? background : Color::Rgb(255, 255, 255, 0.94F), 1.5F},
         Align(HorizontalAlignment::Center, VerticalAlignment::Center), Semantics{.hidden = true});
}

View SelectControl(std::size_t order, bool multiple, bool enabled, std::function<void()> toggle,
                   const SelectionColors& colors, const ThemeSpec& theme) {
  return Stack {
    SelectionBadge(order, multiple && order > 0, colors),
  }.With(Frame{.width = 44, .height = 44}, Align(HorizontalAlignment::Center, VerticalAlignment::Center),
         Focusable{}, theme.interactions.indication, Enabled{enabled},
         Semantics{.role = SemanticRole::Checkbox,
                   .label = order ? labels::picker_deselect_item : labels::picker_select_item,
                   .state_description = order
                       ? std::optional(StringVariant::Format(labels::picker_selection_position, order)) : std::nullopt,
                   .checked = order ? SemanticCheckedState::Checked : SemanticCheckedState::Unchecked,
                   .descendants = SemanticDescendantPolicy::Exclude})
   .OnClick(std::move(toggle));
}

[[huxerui::composable]]
View AssetCell(PickerActions actions, MediaAsset asset, std::size_t order, bool load_more, std::uint32_t pixels,
               SelectionColors colors) {
  auto session = actions.session;
  auto cache = session->thumbnails;
  const auto& theme = UseTheme();
  Lifecycle([actions, load_more] {
    if (load_more) actions.More();
  }, load_more, session->revision);
  const bool video = asset.kind == MediaAssetKind::Video;
  auto open = [actions, asset] { actions.Preview(asset); };
  auto thumbnail = Thumbnail(session->library, asset.id, pixels, cache);
  const bool enabled = !session->submitting && !session->checking && !session->switching_album && !session->finished;
  auto border = order ? View(Canvas([color = colors.outline](PaintContext& paint, Size size) {
    paint.StrokePath(Path::RoundedRect({1, 1, std::max(0.0F, size.width - 2), std::max(0.0F, size.height - 2)},
                                     CornerRadii{}), color, {.width = 2});
  }).With(Semantics{.hidden = true})) : View{};
  return Stack {
    std::move(thumbnail).OnClick(open).With(Focusable{}, Enabled{enabled},
        Semantics{.role = SemanticRole::Button, .label = video ? labels::picker_preview_video : labels::picker_preview_photo,
                  .value = asset.duration ? std::optional(StringVariant(DurationLabel(*asset.duration))) : std::nullopt}),
    border,
    Column {
      Row {
        Spacer(),
        SelectControl(order, actions.limit > 1, enabled, [actions, asset] {
          if (!actions.session->preview) actions.Toggle(asset);
        }, colors, theme),
      },
      Spacer(),
      video ? View(Row {
        Row {
          Image(Icon(PickerIcon::Play)).Tint(Color::White()).With(Frame{.width = 12, .height = 12}),
          asset.duration ? View(Text(DurationLabel(*asset.duration), TextRole::Label)
              .With(FontSize(11), Foreground(Color::White()))) : View{},
        }.With(Spacing(3), Padding(EdgeInsets::Symmetric(5, 3)), CrossAlign(CrossAxisAlignment::Center),
               Background(Color::Rgb(0, 0, 0, 0.64F)), CornerRadius(theme.shapes.extra_small),
               Semantics{.hidden = true}),
        Spacer(),
      }.With(Padding(EdgeInsets{.bottom = 6, .left = 6}))) : View{},
    }.With(CrossAlign(CrossAxisAlignment::Stretch)).Key("controls"),
  }.With(Align(HorizontalAlignment::Stretch, VerticalAlignment::Stretch), ClipChildren{});
}

View PickerState(View illustration, StringVariant title, std::optional<StringVariant> detail,
                 View action, const ThemeSpec& theme) {
  return Column {
    illustration,
    Text(std::move(title), TextRole::Title).Align(TextAlign::Center),
    detail ? View(Text(*detail).Align(TextAlign::Center).With(Foreground(theme.colors.on_surface_variant))) : View{},
    action,
  }.With(MainAlign(MainAxisAlignment::Center), CrossAlign(CrossAxisAlignment::Center),
         Padding(24), Spacing(12), Semantics{.live_region = SemanticLiveRegion::Polite});
}

View StateIcon(const ThemeSpec& theme) {
  return Stack {
    Image(Icon(PickerIcon::Library)).Tint(theme.colors.on_primary_container)
        .With(Frame{.width = 26, .height = 26}),
  }.With(Frame{.width = 56, .height = 56}, Background(theme.colors.primary_container),
         Align(HorizontalAlignment::Center, VerticalAlignment::Center), CornerRadius(theme.shapes.large),
         Semantics{.hidden = true});
}

View AlbumRow(View cover, StringVariant title, bool selected, std::function<void()> activate, const ThemeSpec& theme) {
  return Row {
    cover,
    Text(title).LineLimit(1).Overflow(TextOverflow::Ellipsis).With(Grow(), Foreground(theme.colors.on_surface)),
    selected ? View(Image(Icon(PickerIcon::Check)).Tint(theme.colors.primary).With(Frame{.width = 20, .height = 20}))
             : View{},
  }.With(CrossAlign(CrossAxisAlignment::Center), Padding(EdgeInsets::Symmetric(16, 8)), Spacing(12),
         Background(selected ? theme.colors.surface_container_high : theme.colors.surface),
         Focusable{}, theme.interactions.indication,
         Semantics{.role = SemanticRole::Button, .label = title, .selected = selected,
                   .descendants = SemanticDescendantPolicy::Exclude})
   .OnClick(std::move(activate));
}

View SelectionTray(const PickerActions& actions, std::uint32_t pixels, const SelectionColors& colors,
                   const ThemeSpec& theme) {
  const auto& state = *actions.session;
  return VirtualList(state.selected.size(), [actions, pixels, colors, theme](std::size_t index) -> View {
    const auto& state = *actions.session;
    if (index >= state.selected.size()) return Spacer();
    const auto asset = state.selected[index];
    auto thumbnail = Thumbnail(state.library, asset.id, pixels, state.thumbnails);
    const bool selected = state.preview && state.preview->id == asset.id;
    return Stack {
      thumbnail,
    }.With(Frame{.width = 46, .height = 46}, Align(HorizontalAlignment::Stretch, VerticalAlignment::Stretch),
           CornerRadius(theme.shapes.extra_small), Border{selected ? colors.outline : Color::Transparent(), 2},
           Focusable{}, theme.interactions.indication, Enabled{!state.submitting && !state.switching_album},
           Semantics{.role = SemanticRole::Button, .label = asset.kind == MediaAssetKind::Video
                       ? labels::picker_preview_video : labels::picker_preview_photo,
                     .state_description = StringVariant::Format(labels::picker_selection_position, index + 1),
                     .selected = selected, .descendants = SemanticDescendantPolicy::Exclude})
     .OnClick([actions, asset] { actions.Preview(asset, true); }).Key(asset.id.value);
  }).ScrollAxis(Axis::Horizontal).ItemExtent(46).CacheExtent(46)
    .With(Frame{.height = 56}, Spacing(8), Padding(EdgeInsets{.top = 10, .right = 16, .left = 16}),
          Semantics{.label = labels::picker_selected_tray});
}

[[huxerui::composable]]
View AlbumPopup(PopupContext popup, PickerActions actions, State<Size> picker_size, State<Size> bar_size,
                std::uint32_t pixels, State<bool> open, std::function<void()> dismissed) {
  [[maybe_unused]] const auto updates = actions.updates.Get();
  const auto& state = *actions.session;
  const auto theme = UseTheme();
  const bool visible = open.Get();
  Lifecycle([actions, visible] {
    if (visible) actions.Albums();
  }, visible, state.checking, state.revision);
  auto choose = [actions, open](std::optional<MediaAlbum> album) {
    if (!open.Get()) return;
    open = false;
    actions.SwitchAlbum(std::move(album));
  };
  auto list = VirtualList(state.albums.size() + 1, [actions, choose, theme, pixels](std::size_t index) -> View {
    const auto& state = *actions.session;
    if (index == 0) {
      auto cover = Stack {
        Image(Icon(PickerIcon::Library)).Tint(theme.colors.on_primary_container)
            .With(Frame{.width = 26, .height = 26}),
      }.With(Frame{.width = 48, .height = 48}, Align(HorizontalAlignment::Center, VerticalAlignment::Center),
             Background(theme.colors.primary_container), CornerRadius(theme.shapes.small));
      return AlbumRow(cover, labels::picker_recents, !state.album, [choose] { choose({}); }, theme).Key("recents");
    }
    if (index > state.albums.size()) return Spacer();
    const auto& item = state.albums[index - 1];
    auto cover = Thumbnail(state.library, item.cover_id, pixels, state.thumbnails)
        .With(Frame{.width = 48, .height = 48}, CornerRadius(theme.shapes.small));
    return AlbumRow(cover, item.name, state.album == item.id, [choose, item] { choose(item); }, theme)
        .Key(item.id.value);
  }).ItemExtent(64);
  View content = list;
  float height = 64.0F * static_cast<float>(std::min<std::size_t>(state.albums.size() + 1, 5));
  if (state.albums_failed) {
    height = 172;
    const auto retry_style = SecondaryButtonStyle(UseEnvironment<MediaLibraryPickerStyle>(),
                                                   UseEnvironment<ButtonStyle>(), theme);
    content = PickerState(StateIcon(theme), labels::picker_albums_error, {},
        PickerButton(labels::picker_retry, [actions] { actions.Albums(); }, visible, retry_style), theme);
  } else if (!state.albums_ready && state.albums.empty()) {
    height = 120;
    content = PickerState(ProgressCircle(), labels::picker_loading_albums, {}, {}, theme);
  }
  const Size size = picker_size.Get();
  const float available_height = std::max(0.0F, size.height - bar_size->height);
  return Stack {
    AnimatedPanel(std::move(content).With(Background(theme.colors.surface)), visible, {0, -14},
        [popup, dismissed = std::move(dismissed)] { popup.Dismiss(); dismissed(); }),
  }.With(Frame{.width = size.width, .height = std::min(height, available_height)},
         Align(HorizontalAlignment::Stretch, VerticalAlignment::Stretch),
         CornerRadius(0, 0, theme.shapes.medium, theme.shapes.medium), ClipChildren{},
         Semantics{.label = labels::picker_albums, .hidden = !visible});
}

[[huxerui::composable]]
View PreviewPager(PickerActions actions, std::uint32_t pixels, ButtonStyle secondary_button) {
  auto& session = *actions.session;
  auto center = UseState(*session.preview);
  auto position = UseState(std::size_t{1});
  auto zoom = UseState(Zoom{});
  auto pinching = UseState(false);
  const auto current = center.Get();
  const auto index = position.Get();
  const auto& items = session.PreviewItems();
  const auto found = std::ranges::find(items, current.id, &MediaAsset::id);
  std::array<std::optional<MediaAsset>, 3> pages;
  pages[1] = current;
  if (found != items.end()) {
    if (found != items.begin()) pages[0] = *std::prev(found);
    if (std::next(found) != items.end()) pages[2] = *std::next(found);
  }
  // Keep the committed target in its slot until the pager settles, even if the library changes.
  if (index != 1) pages[index] = session.preview;
  const auto near_end = found != items.end() && std::distance(found, items.end()) <= 6;
  Lifecycle([actions, near_end] {
    if (near_end && !actions.session->preview_from_selection && actions.session->preview_open) actions.More();
  }, current.id, near_end);

  std::vector<View> views;
  views.reserve(pages.size());
  for (std::size_t slot = 0; slot < pages.size(); ++slot) {
    const auto& asset = pages[slot];
    if (slot == 1) {
      views.push_back(AssetPreview(session.library, current, session.thumbnails, pixels, zoom, pinching,
          index == 1 && session.preview_open && !session.checking, secondary_button).Key(current.id.value));
    } else if (asset) {
      views.push_back(Thumbnail(session.library, asset->id, pixels, session.thumbnails, ImageFit::Contain)
          .With(Background(Color::Black())).Key(asset->id.value));
    } else {
      views.push_back(Spacer().With(Background(Color::Black())));
    }
  }
  return Pager(std::move(views), index)
      .DragEnabled(session.preview_open && !session.checking && !session.submitting && !pinching.Get() &&
                   zoom->scale <= 1.0F && (pages[0] || pages[2]))
      .OnChanged([actions, position, pages](std::size_t target) {
        auto& state = *actions.session;
        if (!state.preview_open || state.submitting || state.checking || target == 1 || !pages[target]) return;
        position = target;
        state.preview = pages[target];
        actions.Update();
      })
      .OnSettled([actions, center, position, zoom, pinching](std::size_t target) {
        const auto& state = *actions.session;
        if (!state.preview_open || !state.preview || target == 1 || target != position.Get()) return;
        center = *state.preview;
        position = 1;
        zoom = Zoom{};
        pinching = false;
      }).With(Background(Color::Black())).Key(current.id.value);
}

[[huxerui::composable]]
View PickerContent(MediaLibraryPickerOptions options) {
  auto library = UseMediaLibrary();
  auto application = UseApplication();
  auto retained = UseState(std::make_shared<PickerSession>(library));
  auto updates = UseState(std::uint64_t{0});
  auto scroll = UseScrollController();
  auto tasks = UseTaskScope();
  auto popup = UsePopup();
  auto anchor = UseViewHandle();
  auto album_button_focus = UseViewHandle();
  auto popup_id = UseState(std::optional<LayerId>{});
  auto popup_open = UseState(false);
  auto picker_size = UseState(Size{});
  auto bar_size = UseState(Size{});
  const auto theme = UseTheme();
  const auto& picker_style = UseEnvironment<MediaLibraryPickerStyle>();
  const auto secondary_button = SecondaryButtonStyle(picker_style, UseEnvironment<ButtonStyle>(), theme);
  const auto confirm_button = picker_style.confirm_button.value_or(
      DefaultPickerButton(UseEnvironment<ButtonStyle>(), true, theme));
  const auto badge_background = picker_style.selection_badge_background.value_or(theme.colors.primary);
  const SelectionColors selection_colors{picker_style.selection_outline.value_or(badge_background), badge_background,
      picker_style.selection_badge_foreground.value_or(theme.colors.on_primary)};
  auto icon_style = UseEnvironment<IconButtonStyle>();
  auto bar_style = UseEnvironment<TopAppBarStyle>();
  const bool compact = UseViewportClass() == ViewportClass::Compact;
  const std::size_t columns = picker_style.grid_columns.value_or(compact ? 3 : 5);
  const double width = picker_size->width;
  const double gap = std::min<double>(picker_style.grid_spacing, width / columns);
  const float spacing = static_cast<float>(gap);
  const float cell_extent = width > 0
      ? static_cast<float>((width - gap * (columns - 1)) / columns) : 108.0F;
  const float row_stride = cell_extent + spacing;
  const auto density = UseService<Resources>()->Configuration().display_scale;
  const auto pixels = static_cast<std::uint32_t>(std::clamp(std::ceil(cell_extent * density / 32) * 32, 64.0F, 512.0F));
  const auto cover_pixels = static_cast<std::uint32_t>(std::clamp(std::ceil(48 * density / 32) * 32, 64.0F, 512.0F));
  PickerActions actions{retained.Get(), updates, tasks, scroll, application, options.filter, options.selection_limit,
                        columns, row_stride};
  auto& state = *actions.session;
  [[maybe_unused]] const auto revision = updates.Get();
  Lifecycle([actions] {
    actions.session->retired = false;
    actions.Refresh(true);
    return [session = actions.session] { session->retired = true; };
  });
  const auto layer = popup_id.Get();
  Lifecycle([popup, layer] {
    return [popup, layer] { if (layer) popup.Dismiss(*layer); };
  }, layer);
  const auto dismiss_albums = [popup_open] { popup_open = false; };
  const auto albums_dismissed = [popup_id, album_button_focus, tasks] {
    popup_id = std::nullopt;
    tasks.Post([album_button_focus] { album_button_focus.RequestFocus(); });
  };
  library.OnChanged([actions] {
    if (actions.session->finished || actions.session->retired) return;
    if (actions.application.LifecycleState() != ApplicationLifecycleState::Background) actions.Refresh();
  }, columns, row_stride);
  application.OnLifecycleChanged([actions, popup, popup_id, dismiss_albums](ApplicationLifecycleState value) {
    if (value == ApplicationLifecycleState::Background && !actions.session->finished) {
      auto& state = *actions.session;
      state.page.Cancel();
      state.album_request.Cancel();
      state.album_switch.Cancel();
      if (const auto layer = popup_id.Get()) popup.Dismiss(*layer);
      popup_id = std::nullopt;
      dismiss_albums();
      if (!state.preview_open) actions.ClearPreview();
      if (state.submitting) actions.CancelConfirmation();
      state.checking = true;
      state.thumbnails = std::make_shared<Thumbnails>();
      ++state.revision;
      actions.Update();
    }
  }, columns);

  auto back = [actions, popup_id, dismiss_albums, canceled = std::move(options.on_cancel)] {
    auto& state = *actions.session;
    if (state.finished) return;
    if (state.submitting) actions.CancelConfirmation();
    else if (popup_id.Get()) dismiss_albums();
    else if (state.preview) state.preview_open = false;
    else {
      state.finished = true;
      state.page.Cancel();
      state.album_request.Cancel();
      state.album_switch.Cancel();
      if (canceled) canceled();
      return;
    }
    actions.Update();
  };
  const auto album_name = state.album_name;
  const std::size_t prefetch = grid_prefetch_rows * columns;
  auto grid = VirtualGrid(width > 0 ? state.assets.size() : 0, [actions, pixels, prefetch, selection_colors](std::size_t index) {
    const auto& state = *actions.session;
    if (index >= state.assets.size()) return View(Spacer());
    const auto& asset = state.assets[index];
    auto selected = std::ranges::find(state.selected, asset.id, &MediaAsset::id);
    const std::size_t order = selected == state.selected.end() ? 0 : std::distance(state.selected.begin(), selected) + 1;
    return AssetCell(actions, asset, order, state.cursor && index + prefetch >= state.assets.size(), pixels,
                     selection_colors).Key(asset.id.value);
  }).Columns(GridColumns::Fixed(columns)).RowExtent(cell_extent).ColumnSpacing(spacing).RowSpacing(spacing)
    .CacheExtent(grid_prefetch_rows * row_stride).Controller(scroll);
  const bool split_access = options.filter == MediaFilter::ImagesAndVideos && HasSeparateMediaPermissions();
  const auto request_access = [actions] { actions.RequestAccess(actions.filter); };
  const bool change_photos = Access::CanRequest(state.access.photos);
  const bool change_videos = Access::CanRequest(state.access.videos);
  const auto access_buttons = [&](bool enabled) -> View {
    return Flow {
      change_photos ? PickerButton(labels::picker_photo_access,
          [actions] { actions.RequestAccess(MediaFilter::Images); }, enabled, secondary_button) : View{},
      change_videos ? PickerButton(labels::picker_video_access,
          [actions] { actions.RequestAccess(MediaFilter::Videos); }, enabled, secondary_button) : View{},
    }.With(Spacing(8));
  };
  auto body = View{};
  if (state.checking) {
    body = PickerState(ProgressCircle(), labels::picker_loading, {}, {}, theme);
  } else if (state.page_failed && state.assets.empty() && !state.preview) {
    body = PickerState(StateIcon(theme), labels::picker_library_error, {},
        PickerButton(labels::picker_retry, [actions] { actions.Refresh(true); }, true, secondary_button), theme);
  } else if (!state.supported) {
    body = PickerState(StateIcon(theme), labels::picker_unavailable, {}, {}, theme);
  } else if (!state.access.Readable()) {
    body = PickerState(state.requesting_access ? View(ProgressCircle()) : StateIcon(theme),
        labels::picker_permission_title, labels::picker_permission_detail,
        split_access ? access_buttons(!state.requesting_access)
                     : PickerButton(state.access.NeedsSettings() ? labels::picker_open_settings : labels::picker_allow_access,
                           request_access, !state.requesting_access, secondary_button), theme);
  } else if (state.assets.empty()) {
    body = PickerState(StateIcon(theme), labels::picker_empty, labels::picker_empty_detail, {}, theme);
  }
  const bool show_grid = !body;
  if (!body) body = Column {};

  const auto preview = state.preview_open ? state.preview : std::nullopt;
  const auto order = preview ? std::ranges::find(state.selected, preview->id, &MediaAsset::id) : state.selected.end();
  const auto count = state.selected.size();
  const auto ready = state.supported && state.access.Readable() && !state.checking;
  const bool interactive = ready && !state.submitting && !state.switching_album && !state.finished;
  auto title = options.filter == MediaFilter::Images ? StringVariant(labels::picker_select_photos)
      : options.filter == MediaFilter::Videos ? StringVariant(labels::picker_select_videos)
                                             : StringVariant(labels::picker_select_media);
  if (options.title) title = *options.title;
  if (preview) {
    const auto& items = state.PreviewItems();
    auto index = std::ranges::find(items, preview->id, &MediaAsset::id);
    title = preview->kind == MediaAssetKind::Video ? labels::picker_video : labels::picker_photo;
    if (index != items.end() && (state.preview_from_selection || !state.cursor))
      title = StringVariant::Format(labels::picker_preview_position, std::distance(items.begin(), index) + 1, items.size());
  }
  const bool can_go_back = preview.has_value();
  const auto back_label = state.submitting ? labels::picker_cancel_preparation
      : can_go_back ? labels::picker_back : labels::picker_close;
  auto back_button = IconButton(Icon(can_go_back ? PickerIcon::Back : PickerIcon::Close), back_label).OnClick(back);
  bar_style.background = theme.colors.surface;
  bar_style.title_style.foreground = theme.colors.on_surface;
  bar_style.height = 56;
  bar_style.horizontal_padding = 8;
  if (picker_style.top_app_bar) bar_style = *picker_style.top_app_bar;
  auto open_albums = [actions, picker_size, bar_size, cover_pixels, popup, popup_id,
                      popup_open, anchor, dismiss_albums, albums_dismissed] {
    if (actions.session->preview || actions.session->switching_album || actions.session->finished) return;
    if (popup_id.Get()) {
      dismiss_albums();
      return;
    }
    PopupOptions popup_options;
    popup_options.placement = {AnchorSide::Below, AnchorAlignment::Start};
    popup_options.gap = 0;
    popup_options.viewport_margin = 0;
    popup_options.trap_focus = true;
    popup_options.on_dismiss_request = dismiss_albums;
    popup_id = popup.Show(anchor, PopupFactory([=](PopupContext context) {
      return AlbumPopup(context, actions, picker_size, bar_size, cover_pixels, popup_open, albums_dismissed);
    }), popup_options);
    popup_open = true;
  };
  auto album_button = Row {
    Text(album_name, TextRole::Label).LineLimit(1).Overflow(TextOverflow::Ellipsis)
        .With(Foreground(theme.colors.on_surface), Frame{.max_width = compact ? 104.0F : 156.0F}),
    Image(Icon(PickerIcon::Expand)).Tint(theme.colors.on_surface_variant).With(Frame{.width = 16, .height = 16},
        Rotation(AnimateTo(popup_open.Get() ? 180.0F : 0.0F, TweenSpec{theme.motion.fast}))),
  }.With(Spacing(4), Frame{.min_height = 44}, Padding(EdgeInsets::Symmetric(8, 0)),
         CrossAlign(CrossAxisAlignment::Center), Focusable{}, Enabled{ready && !state.submitting && !state.finished},
         theme.interactions.indication, CornerRadius(theme.shapes.small), album_button_focus,
         Semantics{.role = SemanticRole::Button, .label = labels::picker_choose_album, .value = album_name,
                   .expanded = popup_open.Get(),
                   .descendants = SemanticDescendantPolicy::Exclude})
   .OnClick(open_albums);
  std::vector<View> bar_actions;
  if (preview) {
    const auto number = order == state.selected.end() ? 0 : std::distance(state.selected.begin(), order) + 1;
    bar_actions.push_back(SelectControl(number, options.selection_limit > 1, interactive,
        [actions, item = *preview] { actions.Toggle(item); }, selection_colors, theme));
  } else if (ready) {
    bar_actions.push_back(album_button);
  }
  icon_style.minimum_interactive_size = 44;
  icon_style.icon_size = 22;
  ThemeDefinition navigation;
  navigation.Set(bar_style).Set(icon_style);
  auto confirm = [actions, completed = std::move(options.on_confirm)] { actions.Confirm(completed); };
  const bool manage_access = interactive && !state.preview && !state.requesting_access;
  View access_banner;
  if (ready && split_access && (change_photos || change_videos)) {
    access_banner = Column {
      state.access.Limited() ? View(Text(labels::picker_limited, TextRole::Label)
          .With(Foreground(theme.colors.on_surface_variant))) : View{},
      access_buttons(manage_access),
    }.With(Padding(EdgeInsets::Symmetric(16, 4)), Spacing(4), Background(theme.colors.surface_container_low));
  } else if (ready && state.access.Limited()) {
    access_banner = Row {
      Text(labels::picker_limited, TextRole::Label).With(Grow(), Foreground(theme.colors.on_surface_variant)),
      PickerButton(labels::picker_manage, request_access, manage_access, secondary_button),
    }.With(Padding(EdgeInsets::Symmetric(16, 4)), Spacing(8), CrossAlign(CrossAxisAlignment::Center),
           Background(theme.colors.surface_container_low));
  }
  return Column {
    Theme(navigation, TopAppBar(title, back_button, std::move(bar_actions)).With(anchor, ObserveSize{bar_size})),
    Divider(),
    std::move(access_banner),
    Stack {
      IndexedPages({std::move(grid), std::move(body)}, show_grid ? 0u : 1u)
          .With(Semantics{.hidden = state.preview.has_value()}),
      (state.loading || state.switching_album) && !state.preview ? View(Column {
        Spacer(),
        ProgressBar().With(Semantics{.label = labels::picker_loading_more}),
      }.With(CrossAlign(CrossAxisAlignment::Stretch))) : View{},
      state.preview && state.access.Readable() ? AnimatedPanel(
          PreviewPager(actions, pixels, secondary_button).Key(state.preview_session), state.preview_open, {0, 16},
          [actions] { actions.ClearPreview(); actions.Update(); }).Key("preview") : View{},
    }.With(Grow(), Align(HorizontalAlignment::Stretch, VerticalAlignment::Stretch), ClipChildren{}).Key("pages"),
    (show_grid || preview) && state.page_failed ? View(Row {
      Text(labels::picker_more_error, TextRole::Label).With(Grow()),
      PickerButton(labels::picker_retry, [actions] { actions.More(true); }, true, secondary_button),
    }.With(Spacing(8), CrossAlign(CrossAxisAlignment::Center), Padding(EdgeInsets::Symmetric(16, 4)),
           Background(theme.colors.surface))) : View{},
    state.album_switch_failed ? View(Row {
      Text(labels::picker_library_error, TextRole::Label).With(Grow()),
      PickerButton(labels::picker_retry, [actions] { actions.SwitchAlbum(actions.session->requested_album); },
                   interactive, secondary_button),
    }.With(Spacing(8), CrossAlign(CrossAxisAlignment::Center), Padding(EdgeInsets::Symmetric(16, 4)))) : View{},
    state.message ? View(Text(*state.message, TextRole::Label).With(Padding(EdgeInsets::Symmetric(16, 8)),
        Foreground(theme.colors.on_surface), Background(theme.colors.surface_container_high),
        Semantics{.live_region = SemanticLiveRegion::Polite})) : View{},
    ready ? View(Column {
      Divider(),
      count && options.selection_limit > 1 ? SelectionTray(actions, cover_pixels, selection_colors, theme).Key("selection") : View{},
      state.submitting ? (state.progress ? View(ProgressBar(*state.progress)) : View(ProgressBar())) : View{},
      Row {
        Text(state.submitting ? StringVariant(labels::picker_preparing)
                              : StringVariant::Format(labels::picker_selected_count, count, options.selection_limit),
             TextRole::Label)
            .With(Grow(), Foreground(theme.colors.on_surface_variant),
                  Semantics{.live_region = SemanticLiveRegion::Polite}),
        PickerButton(labels::picker_done, confirm, count > 0 && interactive, confirm_button),
      }.With(Spacing(12), Padding(EdgeInsets::Symmetric(16, 10)), CrossAlign(CrossAxisAlignment::Center)).Key("confirm"),
    }.With(CrossAlign(CrossAxisAlignment::Stretch), Background(theme.colors.surface),
           SafeAreaPadding{.top = false}).Key("footer")) : View{},
  }.With(CrossAlign(CrossAxisAlignment::Stretch), Background(theme.colors.background), ObserveSize{picker_size})
   .On<ViewEvents::BackRequested>(back);
}

} // namespace

[[huxerui::composable]]
View MediaLibraryPicker(MediaLibraryPickerOptions options) {
  detail::ValidatePickerOptions(options.filter, options.selection_limit);
  if (options.style.grid_columns == 0)
    throw std::invalid_argument("Media library picker grid columns must be positive");
  if (!std::isfinite(options.style.grid_spacing) || options.style.grid_spacing < 0)
    throw std::invalid_argument("Media library picker grid spacing must be finite and non-negative");
  const auto key = static_cast<std::uint64_t>(options.selection_limit) * 3 + static_cast<unsigned>(options.filter);
  return ProvideEnvironment(options.style, PickerContent(std::move(options)).Key(key));
}

} // namespace huxerui::media
