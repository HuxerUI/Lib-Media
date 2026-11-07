#if defined(__ANDROID__) || defined(__EMSCRIPTEN__)

#include <atomic>
#include <cmath>

#include "channel_player.h"
#include "state.h"

namespace huxerui::media::detail {
namespace {

MediaErrorCode ErrorCode(std::string_view code) {
  if (code == "notFound")
    return MediaErrorCode::NotFound;
  if (code == "permissionDenied")
    return MediaErrorCode::PermissionDenied;
  if (code == "invalidSource")
    return MediaErrorCode::InvalidSource;
  if (code == "unsupported")
    return MediaErrorCode::Unsupported;
  if (code == "network")
    return MediaErrorCode::Network;
  if (code == "decode")
    return MediaErrorCode::Decode;
  if (code == "output")
    return MediaErrorCode::Output;
  if (code == "playbackNotAllowed")
    return MediaErrorCode::PlaybackNotAllowed;
  if (code == "seekFailed")
    return MediaErrorCode::SeekFailed;
  return MediaErrorCode::Unknown;
}

double Time(const PlatformPayload& payload) {
  const double value = payload.AsDouble();
  if (!std::isfinite(value) || value < 0)
    throw std::invalid_argument("HuxerUI platform media time is invalid");
  return value;
}

} // namespace

ChannelPlayer::ChannelPlayer(PlatformChannel channel) : channel_(std::move(channel)) {
  static std::atomic<std::uint64_t> next_identity{1};
  identity_ = next_identity++;
}
ChannelPlayer::~ChannelPlayer() {
  UnbindVideo();
  channel_.Close();
}

void ChannelPlayer::Connect() {
  std::weak_ptr<ChannelPlayer> weak = shared_from_this();
  channel_.On("media", [weak](const PlatformPayload& payload) {
    if (auto self = weak.lock()) {
      try {
        self->Receive(payload);
      } catch (const std::exception&) {
        if (self->emit)
          self->emit({.generation = self->generation_,
                      .kind = PlayerEventKind::Error,
                      .error = {MediaErrorCode::Decode, "HuxerUI platform media event is malformed", {}, true}});
      }
    }
  });
  Invoke("initialize", {{"identity", identity_}});
}

void ChannelPlayer::Invoke(std::string method, PlatformPayload::Object arguments) {
  const auto generation = generation_;
  arguments.emplace("generation", generation);
  std::weak_ptr<ChannelPlayer> weak = shared_from_this();
  channel_.Invoke<std::monostate>(
      std::move(method), PlatformPayload(std::move(arguments)),
      [weak, generation](PlatformResult<std::monostate> result) {
        if (auto error = std::get_if<PlatformError>(&result)) {
          if (auto self = weak.lock(); self && self->emit && self->generation_ == generation) {
            self->emit({.generation = generation,
                        .kind = PlayerEventKind::Error,
                        .error = {MediaErrorCode::Output, "HuxerUI platform media command failed", error->code, true}});
          }
        }
      });
}

void ChannelPlayer::Load(const MediaSource& source, std::uint64_t generation) {
  generation_ = generation;
  PlatformPayload::Object fields;
  PlatformPayload::Object headers;
  const auto& input = PlayerAccess::Source(source);
  if (const auto* http = std::get_if<MediaHttpSource>(&input)) {
    fields.emplace("kind", "http");
    fields.emplace("value", http->url);
    for (const auto& [name, value] : http->headers)
      headers.emplace(name, value);
  } else if (const auto* file = std::get_if<File>(&input)) {
    fields.emplace("kind", "file");
    fields.emplace("value", file->ToUri().ToString());
  } else {
    fields.emplace("kind", "reference");
    fields.emplace("value", std::get<FileReference>(input));
  }
  fields.emplace("headers", PlatformPayload(std::move(headers)));
  Invoke("load", std::move(fields));
}

void ChannelPlayer::Clear() noexcept {
  if (channel_.IsOpen())
    Invoke("clear");
}
void ChannelPlayer::Play() {
  Invoke("play");
}
void ChannelPlayer::Pause() {
  Invoke("pause");
}
void ChannelPlayer::Seek(MediaTime position, std::uint64_t seek_id) {
  Invoke("seek", {{"position", position.count()}, {"seekId", seek_id}});
}
void ChannelPlayer::SetVolume(double volume, bool muted) {
  Invoke("volume", {{"volume", volume}, {"muted", muted}});
}
void ChannelPlayer::Poll() {
  Invoke("poll");
}

void ChannelPlayer::BindVideo(PlatformChannel video) {
  video_ = std::move(video);
  video_.Invoke<std::monostate>("bind", PlatformPayload(identity_), [](PlatformResult<std::monostate>) {});
}
void ChannelPlayer::UnbindVideo() {
  if (video_.IsOpen())
    video_.Invoke<std::monostate>("unbind", [](PlatformResult<std::monostate>) {});
  video_ = {};
}

void ChannelPlayer::Receive(const PlatformPayload& payload) {
  if (!emit)
    return;
  const auto& fields = payload.AsObject();
  PlayerEvent event;
  event.generation = static_cast<std::uint64_t>(fields.at("generation").AsInteger());
  if (event.generation != generation_)
    return;
  const auto kind = fields.at("kind").AsString();
  if (kind == "error") {
    event.kind = PlayerEventKind::Error;
    event.error = {ErrorCode(fields.at("code").AsString()), std::string(fields.at("message").AsString()),
                   std::string(fields.at("platformCode").AsString()), fields.at("fatal").AsBoolean()};
    event.seek_id = static_cast<std::uint64_t>(fields.at("seekId").AsInteger());
    emit(std::move(event));
    return;
  }
  if (kind == "ready")
    event.kind = PlayerEventKind::Ready;
  else if (kind == "playing")
    event.kind = PlayerEventKind::Playing;
  else if (kind == "paused")
    event.kind = PlayerEventKind::Paused;
  else if (kind == "seeked")
    event.kind = PlayerEventKind::Seeked;
  else if (kind == "ended")
    event.kind = PlayerEventKind::Ended;
  else if (kind == "interrupted")
    event.kind = PlayerEventKind::Interrupted;
  else if (kind != "update")
    throw std::invalid_argument("HuxerUI platform media event kind is invalid");
  auto& value = event.snapshot;
  value.progress.position = MediaTime(Time(fields.at("position")));
  if (!fields.at("duration").IsNull())
    value.progress.duration = MediaTime(Time(fields.at("duration")));
  if (!fields.at("buffered").IsNull()) {
    value.progress.buffered_ranges.emplace();
    for (const auto& range : fields.at("buffered").AsList()) {
      const auto& pair = range.AsList();
      if (pair.size() != 2)
        throw std::invalid_argument("HuxerUI platform buffered range is invalid");
      const double start = Time(pair[0]), end = Time(pair[1]);
      if (end < start)
        throw std::invalid_argument("HuxerUI platform buffered range is reversed");
      value.progress.buffered_ranges->push_back({MediaTime(start), MediaTime(end)});
    }
  }
  if (!fields.at("seekable").IsNull())
    value.seekability = fields.at("seekable").AsBoolean() ? MediaSeekability::Seekable : MediaSeekability::NotSeekable;
  if (!fields.at("audio").IsNull())
    value.has_audio = fields.at("audio").AsBoolean();
  if (!fields.at("video").IsNull())
    value.has_video = fields.at("video").AsBoolean();
  if (!fields.at("width").IsNull() && !fields.at("height").IsNull()) {
    const double width = Time(fields.at("width")), height = Time(fields.at("height"));
    if (width > 0 && height > 0)
      value.video_size = Size{static_cast<float>(width), static_cast<float>(height)};
  }
  value.buffering = fields.at("buffering").AsBoolean();
  event.seek_id = static_cast<std::uint64_t>(fields.at("seekId").AsInteger());
  emit(std::move(event));
}

PlatformPayload EncodedVideoProperties::Encode(const EncodedVideoProperties& properties) {
  const auto& p = properties.value;
  const auto horizontal = p.horizontal_alignment == HorizontalAlignment::Start ? "start"
                          : p.horizontal_alignment == HorizontalAlignment::End ? "end"
                                                                               : "center";
  const auto vertical = p.vertical_alignment == VerticalAlignment::Start ? "start"
                        : p.vertical_alignment == VerticalAlignment::End ? "end"
                                                                         : "center";
  std::string fit;
  switch (p.fit) {
  case ImageFit::Contain:
    fit = "contain";
    break;
  case ImageFit::Cover:
    fit = "cover";
    break;
  case ImageFit::Fill:
    fit = "fill";
    break;
  case ImageFit::None:
    fit = "none";
    break;
  case ImageFit::ScaleDown:
    fit = "scale-down";
    break;
  }
  return PlatformPayload::Object{{"fit", fit}, {"horizontal", horizontal}, {"vertical", vertical}};
}

} // namespace huxerui::media::detail
#endif
