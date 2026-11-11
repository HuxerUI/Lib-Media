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
};
Encoded Encode(std::uint32_t rate, AudioChannelLayout layout, std::uint32_t total, std::uint32_t bitrate = 128000) {
  AudioEncoderConfig config;
  config.input = {rate, AudioSampleFormat::S16, AudioPacking::Interleaved, layout};
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
  bool described = false, summarized = false, ended = false;
  auto drain = [&] {
    for (;;) {
      auto read = encoder.TryRead(); Require(read);
      if (!read.Value()) return;
      auto& value = *read.Value();
      Check(!ended, "Output after EOS");
      if (auto* format = std::get_if<AudioEncodeFormat>(&value)) {
        Check(!described && !summarized, "Duplicate or late AAC format");
        result.format = *format; described = true;
      } else if (auto* packet = std::get_if<EncodedPacket>(&value)) {
        Check(described && !summarized, "AAC packet event order");
        Check(!packet->data.AsBytes().empty() && packet->data.AsBytes().size() <= 65536 && packet->key_frame, "Invalid AAC packet storage");
        Check(packet->pts.timescale == rate && packet->duration == MediaTimestamp{1024, rate}, "AAC packet timing");
        if (!result.packets.empty()) Check(packet->pts.ticks == result.packets.back().pts.ticks + 1024, "Noncontiguous AAC PTS");
        if (!result.packets.empty()) {
          auto copy = std::make_shared<Bytes>(packet->data.AsBytes().begin(), packet->data.AsBytes().end());
          packet->data = BufferReference(*copy, copy);
        }
        result.packets.push_back(std::move(*packet));
      } else if (auto* summary = std::get_if<AudioEncodeSummary>(&value)) {
        Check(!summarized, "Duplicate AAC summary"); result.summary = *summary; summarized = true;
      } else { Check(summarized, "EOS before summary"); ended = true; }
    }
  };
  std::uint32_t position = 0;
  const std::uint32_t chunks[] = {1, 1, 1023, 7, 8193, 511, 1024, 1537};
  unsigned chunk = 0;
  while (position < total) {
    const auto count = std::min(total - position, chunks[chunk++ % std::size(chunks)]);
    // Reuse mutable storage immediately after acceptance to verify the input snapshot.
    auto pcm = std::make_shared<std::vector<std::int16_t>>(result.source.begin() + position * config.input.ChannelCount(),
        result.source.begin() + (position + count) * config.input.ChannelCount());
    AudioFrame frame{config.input, {std::int64_t(rate) + position, rate}, count,
        {BufferReference(std::as_bytes(std::span(*pcm)), pcm)}};
    Eventually([&] {
      drain();
      auto submitted = encoder.TrySubmit(frame); Require(submitted);
      return submitted.Value() == SubmitStatus::Accepted;
    }, "PCM submission stalled");
    std::fill(pcm->begin(), pcm->end(), 0);
    position += count;
  }
  Eventually([&] {
    drain();
    auto finish = encoder.Finish();
    if (!finish.Succeeded() && finish.Error().code == CodecErrorCode::InvalidState) return false;
    Require(finish); return true;
  }, "Encoder initialization stalled");
  Eventually([&] { drain(); return ended; }, "AAC drain stalled");
  Require(encoder.Finish());
  auto late = encoder.TryRead(); Require(late); Check(!late.Value(), "Repeated EOS");
  Check(result.summary.input_samples == total && result.summary.encoded_samples == result.packets.size() * 1024, "AAC sample accounting");
  Check(total ? result.summary.input_start == MediaTimestamp{rate, rate} : !result.summary.input_start, "AAC input anchor");
  Check(total ? !result.packets.empty() : result.packets.empty(), "Empty/nonempty AAC packet count");
  if (result.summary.priming_samples && result.summary.padding_samples)
    Check(result.summary.encoded_samples - *result.summary.priming_samples - *result.summary.padding_samples == total, "AAC trim accounting");
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
  Check(std::equal(retained.begin(), retained.end(), slice.AsBytes().begin(), slice.AsBytes().end()), "AAC storage changed across Reset/Close");
  return result;
}
std::vector<std::int16_t> Decode(const Encoded& encoded) {
  AudioDecoderConfig config;
  config.initialization_data = encoded.format.initialization_data;
  config.output = {encoded.format.sample_rate, AudioSampleFormat::S16, AudioPacking::Interleaved, encoded.format.channel_layout};
  auto created = AudioDecoder::Create(config); Require(created);
  auto decoder = std::move(created).Value();
  std::vector<std::int16_t> pcm;
  bool ended = false;
  auto drain = [&] {
    for (;;) {
      auto read = decoder.TryRead(); Require(read);
      if (!read.Value()) return;
      if (auto* frame = std::get_if<AudioFrame>(&*read.Value())) {
        Check(frame->Validate(), "Invalid roundtrip PCM");
        const auto bytes = frame->planes.front().AsBytes();
        const auto offset = pcm.size(); pcm.resize(offset + bytes.size() / 2);
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
  Check(pcm.size() / config.output.ChannelCount() == encoded.summary.encoded_samples, "Roundtrip PCM count differs from AAC access units");
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
} // namespace
int Run(int argc, char** argv) {
  try {
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
#ifdef __EMSCRIPTEN__
    EM_ASM({ document.body.textContent = 'PASS: C++ WASM AAC encoding, roundtrip, sample accounting and cleanup'; });
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
