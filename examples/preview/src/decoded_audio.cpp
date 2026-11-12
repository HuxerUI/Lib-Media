#include "decoded_preview.h"
#include "packed_pcm.h"

#include <huxerui/media_codec.h>
#include <huxerui/media_playback.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstring>
#include <deque>

using namespace huxerui;
using namespace huxerui::media;
using namespace std::chrono_literals;

namespace media_preview {
namespace {

std::int64_t AddSamples(std::int64_t start, std::uint64_t samples) {
  if (samples > std::uint64_t(INT64_MAX) || start > INT64_MAX - static_cast<std::int64_t>(samples))
    throw std::runtime_error("Audio sample clock overflow");
  return start + static_cast<std::int64_t>(samples);
}

AudioFrame UnpackPcm(const ContainerPacket& packet, CodecId codec, AudioFormat format, std::int64_t pts,
                     std::int64_t duration) {
  const auto layout = media::detail::DescribePackedPcm(codec);
  const auto sample_bytes = layout.bits / 8;
  const auto frame_bytes = sample_bytes * format.ChannelCount();
  const auto bytes = packet.data.AsBytes();
  if (!frame_bytes || !std::in_range<std::uint32_t>(duration) || bytes.size() % frame_bytes ||
      bytes.size() / frame_bytes != static_cast<std::uint32_t>(duration))
    throw std::runtime_error("PCM packet bytes do not match its sample duration");

  const auto sample_values = bytes.size() / sample_bytes;
  const auto output_bytes = format.BytesPerSample();
  auto output = std::make_shared<Bytes>(sample_values * output_bytes);
  for (std::size_t i = 0; i < sample_values; ++i) {
    const auto input = bytes.subspan(i * sample_bytes, sample_bytes);
    std::uint32_t word = 0;
    for (unsigned b = 0; b < sample_bytes; ++b)
      word = (word << 8) | std::to_integer<unsigned>(input[layout.big_endian ? b : sample_bytes - 1 - b]);
    float sample;
    if (layout.floating_point) sample = std::bit_cast<float>(word);
    else if (layout.bits == 8) sample = (static_cast<int>(word) - 128) / 128.0f;
    else sample = std::bit_cast<std::int32_t>(word << (32 - layout.bits)) / 2147483648.0f;
    auto* destination = output->data() + i * output_bytes;
    if (format.sample_format == AudioSampleFormat::F32) std::memcpy(destination, &sample, sizeof(sample));
    else {
      const auto integer = media::detail::QuantizePcm16(sample);
      std::memcpy(destination, &integer, sizeof(integer));
    }
  }
  return {format, {pts, format.sample_rate}, static_cast<std::uint32_t>(duration), {BufferReference(*output, output)}};
}

} // namespace

AudioInput::AudioInput(std::shared_ptr<const File> path) : file(std::move(path)), source(Open(*file)) {
  const auto audio = std::ranges::find(source.Tracks(), TrackKind::Audio, &MediaTrack::kind);
  if (audio == source.Tracks().end()) throw std::runtime_error("The file has no audio track");
  track = *audio;
  Require(source.SelectTracks(std::span(&track.id, 1)));
  if (source.Format() == ContainerFormat::Ogg && !source.Duration()) {
    auto point = source.FindSeekPoint(track.id, {0, 1});
    if (!point.Succeeded() && point.Error().code != ContainerErrorCode::IndexUnavailable)
      Require(std::move(point));
  }
  const auto coverage = source.PresentationRange(track.id);
  if (source.Layout() != ContainerLayout::Fragmented) range = coverage;
  origin = !track.edits.empty() ? MediaTimestamp{0, 1} : coverage ? coverage->start :
      source.StartTime().value_or(MediaTimestamp{0, 1});
  if (!track.edits.empty()) {
    for (const auto& edit : track.edits) duration += Seconds(edit.duration);
  } else if (range) {
    duration = Seconds(range->end) - Seconds(origin);
  } else if (const auto end = source.Duration()) {
    duration = Seconds(*end) - Seconds(origin);
  }
}

std::optional<AudioInput::Item> AudioInput::Read() {
  auto item = Require(source.Read());
  if (!item) {
    if (!duration) {
      if (const auto end = source.Duration()) duration = Seconds(*end) - Seconds(origin);
    }
    return {};
  }
  if (auto* packet = std::get_if<ContainerPacket>(&*item)) return std::move(*packet);
  const auto& start = std::get<TrackDecodeStart>(*item);
  const auto current = std::ranges::find(source.Tracks(), start.track_id, &MediaTrack::id);
  return AudioDecodeStart{current->descriptions.at(start.sample_description_index - 1), start.first_audible_pts};
}

bool AudioInput::Seek(MediaTimestamp target) {
  const auto* edit = EditAt(track.edits, target);
  const bool gap = edit && !edit->media_start;
  auto point = source.Seek(track.id, target, gap ? SeekDirection::AtOrAfter : SeekDirection::AtOrBefore);
  if (point.Succeeded() && !point.Value())
    point = source.Seek(track.id, target, gap ? SeekDirection::AtOrBefore : SeekDirection::AtOrAfter);
  if (!point.Succeeded() && point.Error().code == ContainerErrorCode::IndexUnavailable) return false;
  return Require(std::move(point)).has_value();
}

AudioFormat PlaybackFormat(const MediaSampleDescription& description) {
  if (!description.sample_rate || (description.channel_count != 1 && description.channel_count != 2))
    throw std::runtime_error("Audio preview requires mono or stereo with a known sample rate");
  return {*description.sample_rate, AudioSampleFormat::S16, AudioPacking::Interleaved,
      description.channel_count == 1 ? AudioChannelLayout::Mono : AudioChannelLayout::Stereo};
}

AudioDecode::AudioDecode(std::shared_ptr<AudioInput> input, AudioSampleFormat sample_format)
    : input_(std::move(input)), target_(input_->origin), sample_format_(sample_format) {
  if (!input_->track.edits.empty()) edits_.emplace(input_->track);
}

Task<bool> AudioDecode::Seek(MediaTimestamp target) {
  if (!co_await OnFileThread([input = input_, target] { return input->Seek(target); })) co_return false;
  target_ = target;
  decoder_.reset();
  pcm_codec_.reset();
  pending_start_.reset();
  pending_.reset();
  next_pts_.reset();
  frame.reset();
  windows_.clear();
  if (edits_) edits_->Reset();
  ended = sealed_ = drained_ = false;
  co_return true;
}

Task<bool> AudioDecode::Pump() {
  if (ended || frame) co_return false;
  if (edits_ && (decoder_ || pcm_codec_)) {
    frame = edits_->Read(target_, format_, drained_);
    if (frame) co_return true;
  }
  if (drained_) {
    ended = true;
    co_return true;
  }
  if (decoder_) {
    if (auto item = Require(decoder_->TryRead())) {
      if (auto* pcm = std::get_if<AudioFrame>(&*item)) {
        if (windows_.empty()) throw std::runtime_error("Decoded PCM has no packet interval");
        const auto window = windows_.front();
        windows_.pop_front();
        Publish(std::move(*pcm), window);
      } else if (std::holds_alternative<EndOfStream>(*item)) {
        if (pending_start_) {
          Configure(*pending_start_);
          pending_start_.reset();
        } else drained_ = true;
      }
      co_return true;
    }
  }
  if (sealed_) co_return false;
  if (!pending_) {
    auto item = co_await OnFileThread([input = input_] { return input->Read(); });
    if (item && std::holds_alternative<ContainerPacket>(*item)) {
      pending_ = std::get<ContainerPacket>(std::move(*item));
    } else {
      if (item) {
        auto start = std::get<AudioDecodeStart>(std::move(*item));
        if (!decoder_) {
          Configure(start);
          co_return true;
        }
        pending_start_ = std::move(start);
      }
      if (decoder_) Require(decoder_->Finish());
      else drained_ = true;
      sealed_ = true;
      co_return true;
    }
  }
  auto& packet = *pending_;
  if ((!decoder_ && !pcm_codec_) || !packet.pts || !packet.duration)
    throw std::runtime_error("Audio packet is missing its configuration or timing");
  const auto rate = format_.sample_rate;
  auto pts = Ticks(*packet.pts, rate);
  const auto duration = Ticks(*packet.duration, rate);
  if (duration < 0 || (!duration && !vorbis_priming_))
    throw std::runtime_error("Audio packets require positive duration after decoder priming");
  const auto container = input_->source.Format();
  if (next_pts_ && (container == ContainerFormat::Matroska || container == ContainerFormat::WebM)) {
    const auto distance = std::uint64_t(std::max(pts, *next_pts_)) - std::uint64_t(std::min(pts, *next_pts_));
    if (distance <= rate / 1000) pts = *next_pts_;
  }
  const auto end = AddSamples(pts, duration);
  const auto discard = packet.discard_padding ? Ticks(*packet.discard_padding, rate) : 0;
  if (discard < -duration || discard > duration) throw std::runtime_error("Packet trim exceeds its audio duration");
  auto start = AddSamples(pts, discard < 0 ? -discard : 0);
  auto stop = end - std::max<std::int64_t>(0, discard);
  if (first_audible_pts_) start = std::max(start, CeilTicks(*first_audible_pts_, rate));
  if (!edits_) {
    start = std::max(start, CeilTicks(target_, rate));
    if (input_->range) stop = std::min(stop, CeilTicks(input_->range->end, rate));
  }
  const MediaPresentationRange window{{start, rate}, {stop, rate}};
  if (decoder_) {
    if (Require(decoder_->TrySubmit({packet.data, {pts, rate}, {}, packet.duration, true})) == SubmitStatus::WouldBlock)
      co_return false;
    if (!std::exchange(vorbis_priming_, false)) windows_.push_back(window);
  } else {
    Publish(UnpackPcm(packet, *pcm_codec_, format_, pts, duration), window);
  }
  next_pts_ = end;
  pending_.reset();
  co_return true;
}

void AudioDecode::Configure(const AudioDecodeStart& start) {
  const auto& description = start.description;
  format_ = PlaybackFormat(description);
  format_.sample_format = sample_format_;
  if (media::detail::DescribePackedPcm(description.codec).bits) {
    decoder_.reset();
    pcm_codec_ = description.codec;
  } else {
    pcm_codec_.reset();
    decoder_.emplace(Require(AudioDecoder::Create({
        .initialization_data = description.initialization_data, .output = format_, .codec = description.codec})));
  }
  first_audible_pts_ = start.first_audible_pts;
  vorbis_priming_ = description.codec == CodecId::Vorbis;
  next_pts_.reset();
  windows_.clear();
  sealed_ = false;
}

void AudioDecode::Publish(AudioFrame pcm, MediaPresentationRange window) {
  if (!TrimAudio(pcm, window.start, window.end)) return;
  if (edits_) edits_->Push(pcm);
  else frame = std::move(pcm);
}

namespace {

std::string_view AudioCopySuffix(const MediaDemuxer& reader) {
  const auto tracks = reader.Tracks();
  if (tracks.size() != 1 || tracks.front().kind != TrackKind::Audio) return {};
  switch (reader.Format()) {
    case ContainerFormat::Aiff: return ".aiff";
    case ContainerFormat::Caf: return ".caf";
    case ContainerFormat::Wave64: return ".w64";
    case ContainerFormat::Ogg:
    case ContainerFormat::Matroska:
    case ContainerFormat::WebM:
      if (tracks.front().descriptions.size() == 1 && tracks.front().descriptions.front().codec == CodecId::Vorbis) {
        if (reader.Format() == ContainerFormat::Ogg) return ".ogg";
        return reader.Format() == ContainerFormat::Matroska ? ".mka" : ".webm";
      }
      return {};
    case ContainerFormat::Flac: return ".flac";
    case ContainerFormat::Mp3: return ".mp3";
    case ContainerFormat::Mp4:
      if (std::ranges::all_of(tracks.front().descriptions, [](const auto& description) {
        return description.codec == CodecId::Aac || description.codec == CodecId::Alac;
      })) return ".m4a";
      return {};
    default: return {};
  }
}

struct AudioCopy {
  AudioCopy(std::shared_ptr<const File> input, const File& temporary)
      : file(std::move(input)), reader(Open(*file)) {
    const auto format = reader.Format();
    const auto suffix = AudioCopySuffix(reader);
    if (suffix.empty()) throw std::runtime_error("The audio track cannot be copied to this container");
    output = TemporaryFile(temporary, suffix);
#ifdef __EMSCRIPTEN__
    const auto layout = ContainerLayout::Default;
#else
    const auto layout = format == ContainerFormat::Mp4 ? ContainerLayout::FastStart : ContainerLayout::Default;
#endif
    mux.emplace(Require(MediaMuxer::Open(
        Require(output->OpenRandomAccess(FileAccessMode::ReadWrite)), format,
        reader.Tracks(), {.layout = layout, .audio_metadata = reader.AudioMetadata()})));
  }

  bool Pump() {
    const auto deadline = std::chrono::steady_clock::now() + 8ms;
    for (unsigned i = 0; i < 32; ++i) {
      auto item = Require(reader.Read());
      if (item && std::holds_alternative<TrackDecodeStart>(*item)) continue;
      if (item && mux) {
        const auto& packet = std::get<ContainerPacket>(*item);
        if (Require(mux->TryWrite(packet)))
          throw std::runtime_error("Audio copy requested another track");
        ++remaining;
      } else if (item) {
        if (!remaining) throw std::runtime_error("Audio copy added packets");
        --remaining;
      } else if (mux) {
        const auto& track = reader.Tracks().front();
        range = reader.PresentationRange(track.id);
        if (reader.Format() == ContainerFormat::Mp4)
          Require(mux->EndTrack(track.id, track.priming_samples, track.padding_samples));
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
    return std::make_shared<AudioInput>(file);
  });
  if (controls.audio_info)
    controls.audio_info(!AudioCopySuffix(input->source).empty(), input->source.AudioMetadata());
  AudioDecode decode(input);
  std::optional<DecodedPlayback> playback;
  std::optional<AudioFormat> format;
  PlaybackEpoch epoch;
  bool playing = false, sealed = false;
  const auto origin = Seconds(input->origin);
  const bool incremental = input->source.Layout() == ContainerLayout::Fragmented ||
      input->source.Format() == ContainerFormat::WebM;
  float observed_duration = 0.0F;
  const auto report_position = [&](float position) {
    observed_duration = std::max(observed_duration, position);
    if (input->duration > 0.0F) controls.position(std::min(position, input->duration), input->duration);
    else controls.position(position, incremental ? observed_duration : 0.0F);
  };
  report_position(0);
  for (;;) {
    if (const auto seek = controls.take_seek(); seek && (incremental || input->duration > 0.0F)) {
      const auto end = input->duration > 0.0F ? input->duration : std::max(0.0F, *seek);
      const auto target = std::clamp(*seek, 0.0F, end);
      if (co_await decode.Seek(TimeAt(origin + target))) {
        playback.reset();
        report_position(target);
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
        report_position(current);
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

#ifdef MEDIA_PREVIEW_TEST_EXPORTS
Task<std::shared_ptr<const File>> ExportWave(std::shared_ptr<const File> file, File temporary) {
  auto input = co_await OnFileThread([file] { return std::make_shared<AudioInput>(file); });
  const auto codec = input->track.descriptions.front().codec;
  const bool floating = codec == CodecId::Alac || codec == CodecId::Vorbis || media::detail::DescribePackedPcm(codec).bits;
  AudioDecode decode(input, floating ? AudioSampleFormat::F32 : AudioSampleFormat::S16);
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
          const MediaTrack track{.id = 1, .kind = TrackKind::Audio, .descriptions = {{
              .codec = format.sample_format == AudioSampleFormat::F32 ? CodecId::PcmF32Le : CodecId::PcmS16Le,
              .sample_rate = format.sample_rate, .channel_count = format.ChannelCount()}}};
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
#endif

} // namespace media_preview
