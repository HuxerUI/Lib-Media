#include "decoded_preview.h"

#include <huxerui/media_codec.h>
#include <huxerui/media_playback.h>

#include <algorithm>
#include <chrono>
#include <deque>

using namespace huxerui;
using namespace huxerui::media;
using namespace std::chrono_literals;

namespace media_preview {
namespace {

struct AudioInput {
  explicit AudioInput(std::shared_ptr<const File> path) : file(std::move(path)), source(Open(*file)) {
    const auto track = std::ranges::find(source.Tracks(), TrackKind::Audio, &MediaTrack::kind);
    if (track == source.Tracks().end()) throw std::runtime_error("The file has no audio track");
    description = *track;
    Require(source.SelectTracks(std::span(&description.id, 1)));
    ReadTimeline();
    if (!range && description.padding_samples.value_or(0)) {
      FindDuration();
      if (!range) throw std::runtime_error("Cannot locate the audio tail trim");
    }
  }

  void FindDuration() {
    if (source.Duration()) return;
    auto point = source.FindSeekPoint(description.id, {0, 1});
    if (!point.Succeeded() && point.Error().code != ContainerErrorCode::IndexUnavailable)
      Require(std::move(point));
    ReadTimeline();
  }

  void ReadTimeline() {
    range = source.PresentationRange(description.id);
    origin = range ? range->start : source.StartTime().value_or(MediaTimestamp{0, 1});
    if (range) duration = Seconds(range->end) - Seconds(range->start);
    else if (const auto time = source.Duration()) duration = Seconds(*time);
  }

  std::optional<MediaTrack> Seek(MediaTimestamp target) {
    auto point = source.Seek(description.id, target);
    if (point.Succeeded() && !point.Value()) point = source.Seek(description.id, target, SeekDirection::AtOrAfter);
    if (!point.Succeeded() && point.Error().code == ContainerErrorCode::IndexUnavailable) return {};
    if (!Require(std::move(point))) return {};
    auto item = Require(source.Read());
    if (!item || !std::holds_alternative<MediaTrack>(*item))
      throw std::runtime_error("Audio seek did not return its track description");
    return std::get<MediaTrack>(std::move(*item));
  }

  std::shared_ptr<const File> file;
  MediaDemuxer source;
  MediaTrack description;
  std::optional<MediaPresentationRange> range;
  MediaTimestamp origin;
  float duration = 0;
};

std::int64_t AddSamples(std::int64_t start, std::uint64_t samples) {
  if (samples > std::uint64_t(INT64_MAX) || start > INT64_MAX - static_cast<std::int64_t>(samples))
    throw std::runtime_error("Audio sample clock overflow");
  return start + static_cast<std::int64_t>(samples);
}

class AudioDecode {
public:
  explicit AudioDecode(std::shared_ptr<AudioInput> input)
      : input_(std::move(input)), target_(input_->origin) {
    Configure(input_->description);
  }

  Task<bool> Seek(MediaTimestamp target) {
    const auto track = co_await OnFileThread([input = input_, target] { return input->Seek(target); });
    if (!track) co_return false;
    target_ = target;
    pending_track_.reset();
    pending_.reset();
    frame.reset();
    windows_.clear();
    ended = false;
    Configure(*track);
    head_ = false;
    co_return true;
  }

  Task<bool> Pump() {
    if (ended || frame) co_return false;
    if (auto item = Require(decoder_->TryRead())) {
      if (auto* pcm = std::get_if<AudioFrame>(&*item)) {
        if (windows_.empty()) throw std::runtime_error("Decoded PCM has no packet interval");
        const auto window = windows_.front();
        windows_.pop_front();
        if (TrimAudio(*pcm, window.start, window.end)) frame = std::move(*pcm);
      } else if (std::holds_alternative<EndOfStream>(*item)) {
        if (pending_track_) {
          Configure(*pending_track_);
          pending_track_.reset();
        } else ended = true;
      }
      co_return true;
    }
    if (sealed_) co_return false;
    if (!pending_) {
      auto item = co_await OnFileThread([input = input_] { return Require(input->source.Read()); });
      if (!item || std::holds_alternative<MediaTrack>(*item)) {
        if (item) pending_track_ = std::get<MediaTrack>(std::move(*item));
        Require(decoder_->Finish());
        sealed_ = true;
        co_return true;
      }
      pending_ = std::get<ContainerPacket>(std::move(*item));
    }
    auto& packet = *pending_;
    if (!packet.pts || !packet.duration) throw std::runtime_error("Audio packets require PTS and duration");
    const auto rate = format_.sample_rate;
    auto pts = Ticks(*packet.pts, rate);
    const auto duration = Ticks(*packet.duration, rate);
    if (duration <= 0) throw std::runtime_error("Audio packets require positive duration");
    const auto container = input_->source.Format();
    if (next_pts_ && (container == ContainerFormat::Matroska || container == ContainerFormat::WebM)) {
      const auto distance = std::uint64_t(std::max(pts, *next_pts_)) - std::uint64_t(std::min(pts, *next_pts_));
      if (distance <= rate / 1000) pts = *next_pts_;
    }
    const auto end = AddSamples(pts, duration);
    const auto discard = packet.discard_padding ? Ticks(*packet.discard_padding, rate) : 0;
    if (discard < -duration || discard > duration) throw std::runtime_error("Packet trim exceeds its audio duration");
    if (Require(decoder_->TrySubmit({packet.data, {pts, rate}, {}, packet.duration, true})) == SubmitStatus::WouldBlock)
      co_return false;

    if (std::exchange(head_, false)) {
      const auto begin = MediaTimestamp{AddSamples(pts, track_.priming_samples.value_or(0)), rate};
      if (begin.Compare(target_) > 0) target_ = begin;
    }
    const auto start = std::max(CeilTicks(target_, rate), AddSamples(pts, discard < 0 ? -discard : 0));
    auto stop = end - std::max<std::int64_t>(0, discard);
    if (input_->range) stop = std::min(stop, CeilTicks(input_->range->end, rate));
    windows_.push_back({{start, rate}, {stop, rate}});
    next_pts_ = end;
    pending_.reset();
    co_return true;
  }

  std::optional<AudioFrame> frame;
  bool ended = false;

private:
  void Configure(const MediaTrack& track) {
    if (!track.sample_rate || (track.channel_count != 1 && track.channel_count != 2))
      throw std::runtime_error("Audio preview requires mono or stereo with a known sample rate");
    track_ = track;
    format_ = {*track.sample_rate, AudioSampleFormat::S16, AudioPacking::Interleaved,
        track.channel_count == 1 ? AudioChannelLayout::Mono : AudioChannelLayout::Stereo};
    decoder_.emplace(Require(AudioDecoder::Create({
        .initialization_data = track.initialization_data, .output = format_, .codec = track.codec})));
    next_pts_.reset();
    head_ = true;
    sealed_ = false;
  }

  std::shared_ptr<AudioInput> input_;
  MediaTimestamp target_;
  MediaTrack track_;
  AudioFormat format_;
  std::optional<AudioDecoder> decoder_;
  std::optional<MediaTrack> pending_track_;
  std::optional<ContainerPacket> pending_;
  std::optional<std::int64_t> next_pts_;
  std::deque<MediaPresentationRange> windows_;
  bool head_ = true;
  bool sealed_ = false;
};

std::string_view AudioCopySuffix(const MediaDemuxer& reader) {
  const auto tracks = reader.Tracks();
  if (tracks.size() != 1 || tracks.front().kind != TrackKind::Audio) return {};
  switch (reader.Format()) {
    case ContainerFormat::Flac: return ".flac";
    case ContainerFormat::Mp3: return ".mp3";
    case ContainerFormat::Mp4:
      if (tracks.front().codec == CodecId::Aac || tracks.front().codec == CodecId::Alac) return ".m4a";
      return {};
    default: return {};
  }
}

struct AudioCopy {
  AudioCopy(std::shared_ptr<const File> input, const File& temporary)
      : file(std::move(input)), reader(Open(*file)), fragmented(reader.Layout() == ContainerLayout::Fragmented) {
    const auto format = reader.Format();
    const auto suffix = AudioCopySuffix(reader);
    if (suffix.empty()) throw std::runtime_error("Audio packet copy requires FLAC, MP3, or a single AAC/ALAC MP4 track");
    output = TemporaryFile(temporary, suffix);
#ifdef __EMSCRIPTEN__
    const auto layout = ContainerLayout::Default;
#else
    const auto layout = format == ContainerFormat::Mp4 ? ContainerLayout::FastStart : ContainerLayout::Default;
#endif
    mux.emplace(Require(MediaMuxer::Open(
        Require(output->OpenRandomAccess(FileAccessMode::ReadWrite)), format, reader.Tracks(), layout)));
  }

  void ExtendRange(const ContainerPacket& packet) {
    if (!packet.pts || !packet.duration) throw std::runtime_error("Fragmented audio packet is missing presentation timing");
    const auto start = *packet.pts;
    const auto duration = Ticks(*packet.duration, start.timescale);
    if (duration <= 0 || start.ticks > INT64_MAX - duration)
      throw std::runtime_error("Fragmented audio packet has an invalid presentation interval");
    const MediaTimestamp end{start.ticks + duration, start.timescale};
    if (!range) range = MediaPresentationRange{start, end};
    else {
      if (start.Compare(range->start) < 0) range->start = start;
      if (end.Compare(range->end) > 0) range->end = end;
    }
  }

  bool Pump() {
    const auto deadline = std::chrono::steady_clock::now() + 8ms;
    for (unsigned i = 0; i < 32; ++i) {
      auto item = Require(reader.Read());
      if (item && mux) {
        const auto& packet = std::get<ContainerPacket>(*item);
        if (Require(mux->TryWrite(packet)))
          throw std::runtime_error("Audio copy requested another track");
        if (fragmented) ExtendRange(packet);
        ++remaining;
      } else if (item) {
        if (!remaining) throw std::runtime_error("Audio copy added packets");
        --remaining;
      } else if (mux) {
        const auto& track = reader.Tracks().front();
        if (const auto indexed = reader.PresentationRange(track.id)) range = indexed;
        else if (range && track.presentation_edit) {
          const auto earlier = [](MediaTimestamp a, MediaTimestamp b) { return a.Compare(b) < 0; };
          range->start = std::max(range->start, track.presentation_edit->start, earlier);
          range->end = std::max(range->start, std::min(range->end, track.presentation_edit->end, earlier), earlier);
        }
        Require(mux->Finish());
        mux.reset();
        reader = Open(*output);
      } else {
        if (remaining || reader.PresentationRange(reader.Tracks().front().id) != range)
          throw std::runtime_error("Audio copy readback differs from its source");
        return true;
      }
      if (std::chrono::steady_clock::now() >= deadline) break;
    }
    return false;
  }

  std::shared_ptr<const File> file;
  std::shared_ptr<const File> output;
  MediaDemuxer reader;
  const bool fragmented;
  std::optional<MediaMuxer> mux;
  std::optional<MediaPresentationRange> range;
  std::size_t remaining = 0;
};

DecodedPlayback AudioPlayback(const AudioFormat& format) {
  AudioPlaybackConfig config{.format = format};
  if constexpr (preview_audio_session_supported) {
    config.cleanup_completed = [use = RetainPreviewAudioSession(true)](PlaybackResult<void>) mutable { use.reset(); };
  }
  return Require(DecodedPlayback::Create({.audio = std::move(config)}));
}

} // namespace

#if defined(_MSC_VER) && !defined(__clang__)
// MSVC cannot generate the required coroutine tail transfer for this optimized playback loop.
#pragma optimize("", off)
#endif
Task<void> PlayAudio(std::shared_ptr<const File> file, ClipControls controls) {
  auto input = co_await OnFileThread([file] {
    auto audio = std::make_shared<AudioInput>(file);
    audio->FindDuration();
    return audio;
  });
  if (controls.audio_info)
    controls.audio_info(!AudioCopySuffix(input->source).empty(), input->description.audio_metadata);
  AudioDecode decode(input);
  std::optional<DecodedPlayback> playback;
  std::optional<AudioFormat> format;
  PlaybackEpoch epoch;
  bool playing = false, sealed = false;
  const auto origin = Seconds(input->origin);
  controls.position(0, input->duration);
  for (;;) {
    if (const auto seek = controls.take_seek(); seek && input->duration > 0) {
      const auto target = std::clamp(*seek, 0.0F, input->duration);
      if (co_await decode.Seek(TimeAt(origin + target))) {
        playback.reset();
        controls.position(target, input->duration);
      }
    }
    const bool wanted = !controls.paused();
    bool advanced = false;
    if (playback && wanted != playing) {
      Require(wanted ? playback->Play() : playback->Pause());
      playing = wanted;
    }
    if (wanted || !playback) {
      advanced = co_await decode.Pump();
      if (decode.frame) {
        if (playback && decode.frame->format != format && !sealed) {
          Require(playback->FinishAudio(epoch));
          sealed = true;
        }
        if (!playback || (sealed && playback->Snapshot().state == PlaybackState::Ended)) {
          format = decode.frame->format;
          playback.emplace(AudioPlayback(*format));
          epoch = playback->Epoch();
          sealed = false;
          playing = wanted;
          if (playing) Require(playback->Play());
        }
        if (!sealed && Require(playback->TrySubmitAudio(epoch, *decode.frame)) == SubmitStatus::Accepted) {
          decode.frame.reset();
          advanced = true;
        }
      }
      if (decode.ended && playback && !sealed) {
        Require(playback->FinishAudio(epoch));
        sealed = true;
      }
    }
    if (playback) {
      const auto snapshot = playback->Snapshot();
      if (snapshot.error) throw std::runtime_error(snapshot.error->message);
      if (snapshot.position) {
        const auto current = std::max(0.0F, Seconds(*snapshot.position) - origin);
        controls.position(input->duration > 0 ? std::min(current, input->duration) : current, input->duration);
      }
    }
    if (!advanced) co_await Delay(8ms);
  }
}
#if defined(_MSC_VER) && !defined(__clang__)
#pragma optimize("", on)
#endif

Task<std::shared_ptr<const File>> CopyAudioClip(std::shared_ptr<const File> file, File temporary) {
  auto copy = co_await OnFileThread([file, temporary] { return std::make_shared<AudioCopy>(file, temporary); });
  while (!co_await OnFileThread([copy] { return copy->Pump(); })) co_await Delay(0ms);
  co_return copy->output;
}

Task<std::shared_ptr<const File>> ExportWave(std::shared_ptr<const File> file, File temporary) {
  auto input = co_await OnFileThread([file] { return std::make_shared<AudioInput>(file); });
  AudioDecode decode(input);
  auto result = co_await OnFileThread([temporary] { return TemporaryFile(temporary, ".wav"); });
  std::shared_ptr<MediaMuxer> mux;
  std::optional<AudioFormat> format;
  std::int64_t samples = 0;
  auto deadline = std::chrono::steady_clock::now() + 15s;
  while (!decode.ended) {
    const auto advanced = co_await decode.Pump();
    if (decode.frame) {
      auto pcm = std::exchange(decode.frame, std::nullopt).value();
      if (!mux) {
        format = pcm.format;
        mux = co_await OnFileThread([result, format = *format] {
          const MediaTrack track{.id = 1, .kind = TrackKind::Audio, .codec = CodecId::PcmS16Le,
              .sample_rate = format.sample_rate, .channel_count = format.ChannelCount()};
          return std::make_shared<MediaMuxer>(Require(MediaMuxer::Open(
              Require(result->OpenRandomAccess(FileAccessMode::ReadWrite)), ContainerFormat::Wave, std::span(&track, 1))));
        });
      }
      if (pcm.format != format) throw std::runtime_error("WAV export requires a constant PCM rate and layout");
      co_await OnFileThread([mux, pcm, samples] {
        const auto rate = pcm.format.sample_rate;
        Require(mux->TryWrite({1, pcm.planes.front(), MediaTimestamp{samples, rate}, {},
                              MediaTimestamp{pcm.sample_count, rate}, {}}));
      });
      samples = AddSamples(samples, pcm.sample_count);
    }
    if (advanced) deadline = std::chrono::steady_clock::now() + 15s;
    else if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error("Timed out waiting for decoded audio");
    else co_await Delay(3ms);
  }
  if (!mux) throw std::runtime_error("Audio export produced no PCM");
  co_await OnFileThread([mux, result, samples, rate = format->sample_rate] {
    Require(mux->Finish());
    auto wave = Open(*result);
    while (Require(wave.Read())) {}
    if (wave.Format() != ContainerFormat::Wave || wave.Duration() != MediaTimestamp{samples, rate})
      throw std::runtime_error("WAV readback sample count differs");
  });
  co_return result;
}

} // namespace media_preview
