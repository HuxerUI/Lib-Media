#include <huxerui/media_codec.h>
#ifndef __EMSCRIPTEN__
#include <huxerui/file.h>
#include <huxerui/media_container.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

using namespace huxerui;
using namespace huxerui::media;
namespace {
void Check(bool value, const std::string& message) { if (!value) throw std::runtime_error(message); }
template<class T> void Require(const CodecResult<T>& result) {
  if (!result.Succeeded()) throw std::runtime_error(result.Error().message + " (" + result.Error().platform_code + ")");
}
template<class F> void Eventually(F work, const char* message) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  do {
    if (work()) return;
#ifdef __EMSCRIPTEN__
    emscripten_sleep(1);
#else
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
#endif
  } while (std::chrono::steady_clock::now() < deadline);
  throw std::runtime_error(message);
}
struct Encoded {
  AudioEncodeFormat format;
  AudioEncodeSummary summary;
  std::vector<EncodedPacket> packets;
  std::vector<std::int16_t> source;
  std::vector<float> source_f32;
};
Encoded Encode(std::uint32_t rate, AudioChannelLayout layout, std::uint32_t total, std::uint32_t bitrate = 128000,
    CodecId codec = CodecId::Aac, AudioSampleFormat sample_format = AudioSampleFormat::S16) {
  AudioEncoderConfig config;
  config.input = {rate, sample_format, AudioPacking::Interleaved, layout};
  config.codec = codec;
  config.bitrate = bitrate;
  auto cleaned = std::make_shared<std::atomic<bool>>(false);
  auto cleanup_ok = std::make_shared<std::atomic<bool>>(false);
  config.cleanup_completed = [cleaned, cleanup_ok](auto result) { *cleanup_ok = result.Succeeded(); *cleaned = true; };
  auto created = AudioEncoder::Create(config);
  Require(created);
  auto encoder = std::move(created).Value();
  Encoded result;
  result.source.resize(std::size_t(total) * config.input.ChannelCount());
  for (std::uint32_t i = 0; i < total; ++i) for (unsigned c = 0; c < config.input.ChannelCount(); ++c) {
    const auto t = double(i) / rate;
    result.source[i * config.input.ChannelCount() + c] = static_cast<std::int16_t>(
        11000 * std::sin(t * 2 * 3.141592653589793 * (379 + c * 173) + t * t * 631) +
        3500 * std::sin(t * 2 * 3.141592653589793 * (1723 + c * 311)));
  }
  if (sample_format == AudioSampleFormat::F32) {
    for (std::size_t i = 0; i < result.source.size(); ++i)
      result.source_f32.push_back((int(result.source[i]) * 256 + int(i % 255)) / 8388608.0f);
  }
  const auto input = sample_format == AudioSampleFormat::F32 ?
      std::as_bytes(std::span(result.source_f32)) : std::as_bytes(std::span(result.source));
  const auto stride = config.input.ChannelCount() * config.input.BytesPerSample();
  std::uint64_t samples = 0;
  bool described = false, summarized = false, ended = false;
  auto drain = [&] {
    for (;;) {
      auto read = encoder.TryRead(); Require(read);
      if (!read.Value()) return;
      auto& value = *read.Value();
      Check(!ended, "Output after EOS");
      if (auto* format = std::get_if<AudioEncodeFormat>(&value)) {
        Check(!described && !summarized && format->codec == codec, "Wrong, duplicate, or late audio format");
        result.format = *format; described = true;
      } else if (auto* packet = std::get_if<EncodedPacket>(&value)) {
        Check(described && !summarized, "Audio packet event order");
        Check(!packet->data.AsBytes().empty() && packet->data.AsBytes().size() <= 65536 &&
              (codec == CodecId::Mp3 || packet->key_frame), "Invalid audio packet storage");
        Check(packet->pts.timescale == rate && packet->duration && packet->duration->timescale == rate &&
              packet->duration->ticks > 0, "Audio packet timing");
        const auto length = packet->duration->ticks;
        Check(codec == CodecId::Flac ? length <= 1024 : length == (codec == CodecId::Mp3 ? 1152 : 1024),
              "Unexpected audio packet duration");
        if (!result.packets.empty())
          Check(packet->pts.ticks == result.packets.back().pts.ticks + result.packets.back().duration->ticks, "Noncontiguous audio PTS");
        else Check(packet->key_frame, "First audio packet is not independently decodable");
        samples += length;
        if (!result.packets.empty()) {
          auto copy = std::make_shared<Bytes>(packet->data.AsBytes().begin(), packet->data.AsBytes().end());
          packet->data = BufferReference(*copy, copy);
        }
        result.packets.push_back(std::move(*packet));
      } else if (auto* summary = std::get_if<AudioEncodeSummary>(&value)) {
        Check(!summarized, "Duplicate audio summary"); result.summary = *summary; summarized = true;
      } else { Check(summarized, "EOS before summary"); ended = true; }
    }
  };
  std::uint32_t position = 0;
  const std::uint32_t chunks[] = {1, 1, 1023, 7, 8193, 511, 1024, 1537};
  unsigned chunk = 0;
  while (position < total) {
    const auto count = std::min({total - position, chunks[chunk++ % std::size(chunks)],
        static_cast<std::uint32_t>(config.limits.input_bytes / stride)});
    // Reuse mutable storage immediately after acceptance to verify the input snapshot.
    const auto bytes = input.subspan(position * stride, count * stride);
    auto pcm = std::make_shared<Bytes>(bytes.begin(), bytes.end());
    AudioFrame frame{config.input, {std::int64_t(rate) + position, rate}, count, {BufferReference(*pcm, pcm)}};
    Eventually([&] {
      drain();
      auto submitted = encoder.TrySubmit(frame); Require(submitted);
      return submitted.Value() == SubmitStatus::Accepted;
    }, "PCM submission stalled");
    std::fill(pcm->begin(), pcm->end(), std::byte{});
    position += count;
  }
  Eventually([&] {
    drain();
    auto finish = encoder.Finish();
    if (!finish.Succeeded() && finish.Error().code == CodecErrorCode::InvalidState) return false;
    Require(finish); return true;
  }, "Encoder initialization stalled");
  Eventually([&] { drain(); return ended; }, "audio drain stalled");
  Require(encoder.Finish());
  auto late = encoder.TryRead(); Require(late); Check(!late.Value(), "Repeated EOS");
  Check(result.summary.input_samples == total && result.summary.encoded_samples == samples, "Audio sample accounting");
  Check(total ? result.summary.input_start == MediaTimestamp{rate, rate} : !result.summary.input_start, "Audio input anchor");
  Check(total ? !result.packets.empty() : result.packets.empty(), "Empty/nonempty audio packet count");
  if (result.summary.priming_samples && result.summary.padding_samples)
    Check(result.summary.encoded_samples - *result.summary.priming_samples - *result.summary.padding_samples == total, "Audio trim accounting");
  Bytes retained;
  BufferReference slice;
  if (!result.packets.empty()) {
    slice = result.packets.front().data.Slice(0, result.packets.front().data.AsBytes().size());
    retained.assign(slice.AsBytes().begin(), slice.AsBytes().end());
  }
  Require(encoder.Reset());
  encoder.Close(); encoder.Close();
  Eventually([&] { return cleaned->load(); }, "Encoder cleanup stalled with held packets");
  Check(cleanup_ok->load(), "Encoder cleanup failed");
  Check(std::equal(retained.begin(), retained.end(), slice.AsBytes().begin(), slice.AsBytes().end()), "Audio storage changed across Reset/Close");
  return result;
}
template<class Sample = std::int16_t>
std::vector<Sample> Decode(const Encoded& encoded) {
  AudioDecoderConfig config;
  config.initialization_data = encoded.format.initialization_data;
  config.codec = encoded.format.codec;
  constexpr auto samples = std::is_same_v<Sample, float> ? AudioSampleFormat::F32 : AudioSampleFormat::S16;
  config.output = {encoded.format.sample_rate, samples, AudioPacking::Interleaved, encoded.format.channel_layout};
  auto created = AudioDecoder::Create(config); Require(created);
  auto decoder = std::move(created).Value();
  std::vector<Sample> pcm;
  bool ended = false;
  auto drain = [&] {
    for (;;) {
      auto read = decoder.TryRead(); Require(read);
      if (!read.Value()) return;
      if (auto* frame = std::get_if<AudioFrame>(&*read.Value())) {
        Check(frame->Validate(), "Invalid roundtrip PCM");
        const auto bytes = frame->planes.front().AsBytes();
        const auto offset = pcm.size(); pcm.resize(offset + bytes.size() / sizeof(Sample));
        std::memcpy(pcm.data() + offset, bytes.data(), bytes.size());
      } else if (std::holds_alternative<EndOfStream>(*read.Value())) ended = true;
    }
  };
  for (const auto& packet : encoded.packets) Eventually([&] {
    drain(); auto submitted = decoder.TrySubmit(packet); Require(submitted);
    return submitted.Value() == SubmitStatus::Accepted;
  }, "Roundtrip decoder submission stalled");
  Require(decoder.Finish());
  Eventually([&] { drain(); return ended; }, "Roundtrip decoder drain stalled");
  decoder.Close();
  Check(pcm.size() / config.output.ChannelCount() == encoded.summary.encoded_samples, "Roundtrip PCM count differs from coded audio packets");
  return pcm;
}
void CheckSignal(const Encoded& encoded, const std::vector<std::int16_t>& decoded) {
  const auto channels = encoded.format.channel_layout == AudioChannelLayout::Mono ? 1u : 2u;
  const auto source_samples = encoded.source.size() / channels;
  Check(source_samples > 8192, "Signal fixture is too short");
  double best = -1;
  std::size_t best_delay = 0;
  const auto maximum = std::min<std::size_t>(8192, decoded.size() / channels - source_samples);
  const auto first = encoded.summary.priming_samples.value_or(0);
  const auto last = encoded.summary.priming_samples.value_or(maximum);
  for (std::size_t delay = first; delay <= last; ++delay) {
    double dot = 0, source_energy = 0, decoded_energy = 0;
    for (std::size_t i = 4096; i < source_samples - 4096; i += 8) for (unsigned c = 0; c < channels; ++c) {
      const auto a = double(encoded.source[i * channels + c]);
      const auto b = double(decoded[(i + delay) * channels + c]);
      dot += a * b; source_energy += a * a; decoded_energy += b * b;
    }
    const auto correlation = dot / std::sqrt(source_energy * decoded_energy);
    if (correlation > best) { best = correlation; best_delay = delay; }
  }
  Check(best > 0.96, "AAC roundtrip signal correlation is too low: " + std::to_string(best));
  std::cout << encoded.format.sample_rate << " Hz " << channels << " channels: packets=" << encoded.packets.size()
      << " correlation=" << best << " alignment(test only)=" << best_delay << '\n';
}
#ifndef __EMSCRIPTEN__
template<class T, class E> T Take(Result<T, E> result) {
  if (!result.Succeeded()) throw std::runtime_error(result.Error().message);
  if constexpr (!std::is_void_v<T>) return std::move(result).Value();
}

void M4aReadback(Encoded& encoded) {
  const auto& summary = encoded.summary;
  if (!summary.priming_samples || !summary.padding_samples) return;
  const auto rate = encoded.format.sample_rate;
  const MediaTrack track{.id = 1, .kind = TrackKind::Audio,
      .descriptions = {{.codec = CodecId::Aac, .initialization_data = encoded.format.initialization_data,
          .sample_rate = rate, .channel_count = encoded.format.channel_layout == AudioChannelLayout::Mono ? 1u : 2u}}};
  const auto path = std::filesystem::temp_directory_path() /
      ("lib-media-aac-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".m4a");
  const File file(path.u8string());
  auto mux = Take(MediaMuxer::Open(Take(file.OpenRandomAccess(FileAccessMode::ReadWrite, FileOpenMode::CreateNew)),
      ContainerFormat::Mp4, std::span(&track, 1), {.layout = ContainerLayout::FastStart}));
  for (const auto& packet : encoded.packets)
    Check(!Take(mux.TryWrite({1, packet.data, packet.pts, packet.dts, packet.duration, true})), "M4A AAC muxing stalled");
  Take(mux.EndTrack(1, summary.priming_samples, summary.padding_samples));
  Take(mux.Finish());
  auto demux = Take(MediaDemuxer::Open(Take(file.OpenRandomAccess())));
  const auto tracks = demux.Tracks();
  const auto& description = tracks.front().descriptions.front();
  const auto origin = encoded.packets.front().pts.Rescale(rate)->ticks;
  Check(description.codec == CodecId::Aac && description.initialization_data == track.descriptions.front().initialization_data &&
      !tracks.front().edits.empty() &&
      tracks.front().edits.back().media_start == MediaTimestamp{static_cast<std::int64_t>(*summary.priming_samples), rate},
      "M4A changed AAC initialization or trim");
  const auto start = *summary.input_start->Rescale(rate);
  Check(demux.PresentationRange(track.id) == MediaPresentationRange{start,
      {start.ticks + static_cast<std::int64_t>(summary.input_samples), rate}}, "M4A changed the audible sample interval");
  Check(std::holds_alternative<TrackDecodeStart>(*Take(demux.Read())), "M4A lost its decode start");
  for (auto& packet : encoded.packets) {
    auto item = Take(demux.Read());
    Check(bool(item), "M4A lost an encoded AAC packet");
    auto& actual = std::get<ContainerPacket>(*item);
    Check(actual.pts == MediaTimestamp{packet.pts.Rescale(rate)->ticks - origin, rate} && actual.duration == packet.duration &&
        std::ranges::equal(actual.data.AsBytes(), packet.data.AsBytes()), "M4A changed the AAC packet");
    packet.data = std::move(actual.data);
  }
  Check(!Take(demux.Read()), "M4A added AAC packets");
  Take(demux.Close());
  std::filesystem::remove(path);
}

void ReadFramedAudio(Encoded& encoded, const std::filesystem::path& directory) {
  const bool flac = encoded.format.codec == CodecId::Flac;
  const auto layout = encoded.format.channel_layout;
  const auto rate = encoded.format.sample_rate;
  const AudioFormat pcm{rate, AudioSampleFormat::S16, AudioPacking::Interleaved, layout};
  const MediaTrack track{.id = 1, .kind = TrackKind::Audio, .timescale = rate,
      .descriptions = {{.codec = encoded.format.codec, .initialization_data = encoded.format.initialization_data,
          .sample_rate = rate, .channel_count = pcm.ChannelCount(), .speaker_mask = pcm.SpeakerMask()}}};
  const auto path = directory / (std::to_string(rate) + "-" + std::to_string(pcm.ChannelCount()) + "-" +
      std::to_string(encoded.summary.input_samples) + (flac ? ".flac" : ".mp3"));
  const File file(path.u8string());
  MediaMuxerOptions options;
  options.audio_metadata.emplace().title = "Encoded audio round trip";
  auto mux = Take(MediaMuxer::Open(Take(file.OpenRandomAccess(FileAccessMode::ReadWrite, FileOpenMode::CreateNew)),
      flac ? ContainerFormat::Flac : ContainerFormat::Mp3, std::span(&track, 1), options));
  if (!flac) {
    const auto dependent = std::ranges::find(encoded.packets, false, &EncodedPacket::key_frame);
    if (dependent != encoded.packets.end())
      Check(!mux.TryWrite({1, dependent->data, MediaTimestamp{0, rate}, {}, dependent->duration, false}).Succeeded(),
            "MP3 writer accepted a dependent initial frame");
  }
  for (const auto& packet : encoded.packets)
    Check(!Take(mux.TryWrite({1, packet.data, {}, {}, packet.duration, packet.key_frame})), "Audio file muxing stalled");
  Take(mux.EndTrack(1, encoded.summary.priming_samples, encoded.summary.padding_samples));
  Take(mux.Finish());

  auto demux = Take(MediaDemuxer::Open(Take(file.OpenRandomAccess())));
  const auto expected_end = MediaTimestamp{static_cast<std::int64_t>(encoded.summary.input_samples), rate};
  const auto& output = demux.Tracks().front();
  const auto& description = output.descriptions.front();
  const auto metadata = demux.AudioMetadata();
  Check(demux.Duration() == expected_end && demux.PresentationRange(1) == MediaPresentationRange{{0, rate}, expected_end} &&
        description.codec == encoded.format.codec && metadata && metadata->title == options.audio_metadata->title &&
        (flac ? description.speaker_mask == pcm.SpeakerMask() :
          output.priming_samples == encoded.summary.priming_samples && output.padding_samples == encoded.summary.padding_samples),
        "Encoded audio file changed its duration, description, or trim");
  const auto first = Take(demux.Read());
  Check(first && std::holds_alternative<TrackDecodeStart>(*first), "Encoded audio file lost its decode start");
  encoded.format.initialization_data = description.initialization_data;
  for (auto& packet : encoded.packets) {
    auto item = Take(demux.Read());
    Check(item && std::holds_alternative<ContainerPacket>(*item), "Encoded audio file lost a packet");
    auto& actual = std::get<ContainerPacket>(*item);
    Check(actual.pts == MediaTimestamp{packet.pts.ticks - rate, rate} && actual.duration == packet.duration &&
        std::ranges::equal(actual.data.AsBytes(), packet.data.AsBytes()), "Encoded audio file changed a packet");
    packet.data = std::move(actual.data);
  }
  Check(!Take(demux.Read()), "Encoded audio file added packets");
  Check(Take(demux.Seek(1, {expected_end.ticks - 1, rate})).has_value(), "Encoded audio file cannot seek to its final interval");
  Take(demux.Close());
}
#endif

void Export(const Encoded& encoded, const std::filesystem::path& directory) {
  std::filesystem::create_directories(directory);
  const auto name = std::to_string(encoded.format.sample_rate) + (encoded.format.channel_layout == AudioChannelLayout::Mono ? "-mono" : "-stereo");
  std::ofstream aac(directory / (name + ".aac"), std::ios::binary);
  for (const auto& packet : encoded.packets) {
    const auto size = packet.data.AsBytes().size() + 7;
    const auto index = encoded.format.sample_rate == 48000 ? 3u : 4u;
    const auto channels = encoded.format.channel_layout == AudioChannelLayout::Mono ? 1u : 2u;
    const unsigned char header[] = {0xff, 0xf1, static_cast<unsigned char>((1u << 6) | (index << 2)),
        static_cast<unsigned char>((channels << 6) | (size >> 11)), static_cast<unsigned char>(size >> 3),
        static_cast<unsigned char>((size << 5) | 0x1f), 0xfc};
    aac.write(reinterpret_cast<const char*>(header), sizeof(header));
    aac.write(reinterpret_cast<const char*>(packet.data.AsBytes().data()), packet.data.AsBytes().size());
  }
  std::ofstream source(directory / (name + ".s16"), std::ios::binary);
  source.write(reinterpret_cast<const char*>(encoded.source.data()), encoded.source.size() * 2);
}

void CheckCompressedAudio(std::optional<std::filesystem::path> output) {
  for (const auto codec : {CodecId::Flac, CodecId::Mp3}) {
    std::atomic<bool> done{false};
    std::optional<CodecError> error;
    AudioEncoderConfig config;
    config.input = {48000, AudioSampleFormat::S16, AudioPacking::Interleaved, AudioChannelLayout::Stereo};
    config.codec = codec;
    AudioEncoder::Probe(config, [&](auto result) { if (!result.Succeeded()) error = result.Error(); done = true; });
    Eventually([&] { return done.load(); }, "Compressed audio encoder probe stalled");
    if (error && error->code == CodecErrorCode::Unsupported && error->message.starts_with(codec == CodecId::Flac ?
        "FLAC encoding requires libFLAC" : "MP3 encoding requires libmp3lame")) {
      std::cout << error->message << '\n';
      continue;
    }
    if (error) throw std::runtime_error(error->message + " (" + error->platform_code + ")");
    for (auto count : {0u, 1u, 1023u, 1024u, 1025u, 1151u, 1152u, 1153u})
      Encode(48000, AudioChannelLayout::Stereo, count, 128000, codec);

    const auto exercise = [&](unsigned rate, AudioChannelLayout layout, unsigned count, AudioSampleFormat format) {
      auto encoded = Encode(rate, layout, count, 128000, codec, format);
      const bool flac = codec == CodecId::Flac;
      const auto& summary = encoded.summary;
      Check(summary.priming_samples && summary.padding_samples && encoded.packets.front().pts ==
          MediaTimestamp{rate - static_cast<std::int64_t>(*summary.priming_samples), rate}, "Audio encoder lost its priming position");
      if (flac) Check(summary.priming_samples == 0 && summary.padding_samples == 0, "FLAC padded its short tail");
#ifndef __EMSCRIPTEN__
      const auto directory = output.value_or(std::filesystem::temp_directory_path() /
          ("lib-media-compressed-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())));
      std::filesystem::create_directories(directory);
      ReadFramedAudio(encoded, directory);
      if (!output) std::filesystem::remove_all(directory);
#endif
      if (flac && format == AudioSampleFormat::F32)
        Check(Decode<float>(encoded) == encoded.source_f32, "24-bit FLAC changed PCM samples or channel order");
      else if (flac)
        Check(std::ranges::equal(Decode<float>(encoded), encoded.source,
            [](float actual, std::int16_t sample) { return actual == sample / 32768.0f; }), "16-bit FLAC changed PCM samples");
#ifndef __EMSCRIPTEN__
      else if (output) {
        const AudioFormat pcm{rate, format, AudioPacking::Interleaved, layout};
        std::ofstream source(*output / (std::to_string(rate) + "-" + std::to_string(pcm.ChannelCount()) + "-" +
            std::to_string(count) + ".s16"), std::ios::binary);
        source.write(reinterpret_cast<const char*>(encoded.source.data()), encoded.source.size() * sizeof(std::int16_t));
      }
#endif
    };
    if (codec == CodecId::Flac) {
      exercise(44100, AudioChannelLayout::Mono, 16385, AudioSampleFormat::S16);
      exercise(48000, AudioChannelLayout::Surround51Side, 32769, AudioSampleFormat::F32);
      exercise(96000, AudioChannelLayout::Surround71, 16385, AudioSampleFormat::F32);
    } else {
      exercise(32000, AudioChannelLayout::Mono, 5731, AudioSampleFormat::S16);
      exercise(44100, AudioChannelLayout::Stereo, 125009, AudioSampleFormat::S16);
      exercise(48000, AudioChannelLayout::Mono, 120017, AudioSampleFormat::S16);
    }
    std::cout << (codec == CodecId::Flac ? "FLAC" : "MP3") << " encoding and sample accounting passed\n";
  }
}
} // namespace
int Run(int argc, char** argv) {
  try {
#ifdef __EMSCRIPTEN__
    const bool compressed_only = EM_ASM_INT({ return new URLSearchParams(location.search).has('compressed-audio'); });
#else
    const bool compressed_only = argc == 2 && std::string_view(argv[1]) == "--compressed-audio";
#endif
    if (!compressed_only) {
      for (auto count : {0u, 1u, 1023u, 1024u, 1025u, 1537u}) Encode(48000, AudioChannelLayout::Stereo, count);
      for (auto rate : {44100u, 48000u}) for (auto layout : {AudioChannelLayout::Mono, AudioChannelLayout::Stereo}) {
        auto encoded = Encode(rate, layout, rate + 1537);
#ifndef __EMSCRIPTEN__
        M4aReadback(encoded);
#endif
        CheckSignal(encoded, Decode(encoded));
        if (argc == 2) Export(encoded, argv[1]);
      }
      for (auto bitrate : {96000u, 160000u, 192000u}) Encode(48000, AudioChannelLayout::Stereo, 8193, bitrate);
      std::atomic<bool> probed{false}, supported{false};
      AudioEncoderConfig config; config.input = {48000, AudioSampleFormat::S16, AudioPacking::Interleaved, AudioChannelLayout::Stereo};
      AudioEncoder::Probe(config, [&](auto result) { supported = result.Succeeded(); probed = true; });
      Eventually([&] { return probed.load(); }, "AAC probe stalled"); Check(supported, "AAC probe failed");
      std::cout << "AAC encoder native tests passed\n";
    }
    CheckCompressedAudio(!compressed_only && argc == 2 ? std::optional{std::filesystem::path(argv[1])} : std::nullopt);
#ifdef __EMSCRIPTEN__
    EM_ASM({ document.body.textContent = 'PASS: C++ WASM audio encoding, roundtrip, sample accounting and cleanup'; });
#endif
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
#ifdef __EMSCRIPTEN__
    EM_ASM({ document.body.textContent = 'FAIL: ' + UTF8ToString($0); }, error.what());
#endif
    return 1;
  }
}
#ifdef __EMSCRIPTEN__
int main() { return Run(0, nullptr); }
#else
int main(int argc, char** argv) { return Run(argc, argv); }
#endif
