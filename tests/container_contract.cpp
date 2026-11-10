#include <huxerui/file.h>
#include <huxerui/media_container.h>

#include "container/ebml.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace huxerui;
using namespace huxerui::media;
namespace fs = std::filesystem;

namespace {
void Check(bool ok, const char* message) {
  if (!ok) throw std::runtime_error(message);
}

template<class T, class E> T Take(Result<T, E> result) {
  if (!result.Succeeded()) throw std::runtime_error(result.Error().message);
  return std::move(result).Value();
}

File Path(const fs::path& path) { return File(path.u8string()); }

RandomAccessStream Output(const fs::path& path) {
  return Take(Path(path).OpenRandomAccess(FileAccessMode::WriteOnly, FileOpenMode::CreateNew));
}

BufferReference BytesRef(Bytes bytes) {
  auto owner = std::make_shared<Bytes>(std::move(bytes));
  return BufferReference(*owner, owner);
}

struct TrackFixture {
  MediaTrack track;
  std::vector<ContainerPacket> packets;
};

TrackFixture Fixture(bool audio, bool fragmented) {
  const auto base = fs::path(MEDIA_FIXTURES) / "codec";
  const std::string name = audio ? "aac" : "avc";
  TrackFixture fixture;
  auto& track = fixture.track;
  track.id = audio ? 2 : 1;
  track.kind = audio ? TrackKind::Audio : TrackKind::Video;
  track.codec = audio ? CodecId::Aac : CodecId::H264;
  track.framing = audio ? PacketFraming::CodecPacket : PacketFraming::LengthPrefixedNals;
  track.initialization_data = Take(Path(base / (name + ".config")).ReadBytes());
  if (audio) {
    track.sample_rate = 48000;
    track.channel_count = 2;
  } else {
    track.coded_width = 64;
    track.coded_height = 48;
    track.visible = VideoRectangle{2, 2, 60, 44};
  }
  std::ifstream rows(base / (name + ".txt"));
  std::string file;
  std::int64_t pts, dts, duration;
  std::uint32_t scale;
  int sync;
  while (rows >> file >> pts >> dts >> duration >> scale >> sync) {
    track.timescale = scale;
    const auto shift = fragmented ? scale : 0;
    fixture.packets.push_back({track.id, BytesRef(Take(Path(base / file).ReadBytes())),
                              MediaTimestamp{pts + shift, scale}, MediaTimestamp{dts + shift, scale},
                              MediaTimestamp{duration, scale}, sync != 0});
  }
  Check(!fixture.packets.empty(), "Missing codec fixtures");
  if (audio && !fragmented) fixture.packets.back().duration = MediaTimestamp{1024, 48000};
  return fixture;
}

void Compare(const ContainerPacket& actual, const ContainerPacket& expected) {
  Check(actual.track_id == expected.track_id && actual.pts == expected.pts && actual.dts == expected.dts &&
        actual.duration == expected.duration && actual.sync_sample == expected.sync_sample &&
        std::ranges::equal(actual.data.AsBytes(), expected.data.AsBytes()), "Container packet differs after round trip");
}

void Mp4(const fs::path& path, bool fragmented) {
  const auto video = Fixture(false, fragmented);
  const auto audio = Fixture(true, fragmented);
  const std::array tracks{video.track, audio.track};
  const auto layout = fragmented ? ContainerLayout::Fragmented : ContainerLayout::Default;
  const auto requirement = Take(MediaMuxer::ValidateOutput(ContainerFormat::Mp4, tracks, layout));
  Check(requirement == (fragmented ? MediaMuxer::OutputRequirement::SequentialWrite :
                                    MediaMuxer::OutputRequirement::RandomWrite), "Wrong MP4 output requirement");
  auto mux = fragmented
      ? Take(MediaMuxer::Open(Take(Path(path).OpenWrite()), ContainerFormat::Mp4, tracks, layout))
      : Take(MediaMuxer::Open(Output(path), ContainerFormat::Mp4, tracks, layout));

  auto untimed = video.packets[0];
  untimed.dts.reset();
  auto error = mux.TryWrite(untimed);
  Check(!error.Succeeded() && error.Error().code == ContainerErrorCode::MissingTiming, "Missing AVC DTS was accepted");
  Check(!Take(mux.TryWrite(video.packets[0])), "First video packet was not accepted");
  Check(Take(mux.TryWrite(video.packets[1])) == audio.track.id, "Interleaver did not request missing audio");
  std::array<std::size_t, 2> cursor{1, 0};
  const std::array inputs{&video, &audio};
  std::size_t track = 1;
  std::size_t attempts = 0;
  while (cursor[0] < video.packets.size() || cursor[1] < audio.packets.size()) {
    Check(++attempts < 300, "Mux interleaver made no progress");
    auto& input = *inputs[track];
    const auto need = Take(mux.TryWrite(input.packets[cursor[track]]));
    if (need) {
      Check(*need == inputs[1 - track]->track.id, "Mux returned an invalid pending track");
      track = 1 - track;
    } else if (++cursor[track] == input.packets.size()) {
      if (track == 1 && !fragmented) Take(mux.EndTrack(input.track.id, 1024, 288));
      else Take(mux.EndTrack(input.track.id));
      track = 1 - track;
    }
  }
  Take(mux.Finish());
  Check(!mux.TryWrite(video.packets[0]).Succeeded(), "Finished writer still accepts input");

  auto file = Path(path);
  if (!fragmented) {
    auto rejected = MediaDemuxer::Open(Take(file.OpenRead()));
    Check(!rejected.Succeeded() && rejected.Error().code == ContainerErrorCode::NeedRandomAccess,
          "Tail metadata did not request random access");
  }
  auto demux = fragmented
      ? Take(MediaDemuxer::Open(Take(file.OpenRead())))
      : Take(MediaDemuxer::Open(Take(file.OpenRandomAccess())));
  Check(demux.Layout() == layout && demux.Tracks().size() == 2, "Wrong MP4 layout or track count");
  Check(demux.Tracks()[0].visible == video.track.visible, "MP4 clean aperture was not retained");
  if (!fragmented) {
    Check(demux.Tracks()[1].priming_samples == 1024 && demux.Tracks()[1].padding_samples == 288,
          "MP4 AAC edit list did not retain trim");
  }
  cursor = {};
  std::optional<ContainerPacket> retained;
  std::size_t last_packet_size = 0;
  while (auto item = Take(demux.Read())) {
    const auto& packet = std::get<ContainerPacket>(*item);
    const auto index = packet.track_id - 1;
    Check(index < inputs.size() && cursor[index] < inputs[index]->packets.size(), "Unexpected MP4 packet");
    Compare(packet, inputs[index]->packets[cursor[index]++]);
    last_packet_size = packet.data.AsBytes().size();
    if (!retained && index == 0) retained = packet;
  }
  Check(cursor[0] == video.packets.size() && cursor[1] == audio.packets.size(), "MP4 packets were lost");
  if (!fragmented) {
    Check(!Take(demux.Read()), "Find or EOF changed the read cursor");
    auto point = Take(demux.FindSeekPoint(1, {1, 24}));
    Check(point && point->time == *video.packets[0].pts && point->kind == SeekPoint::Kind::Independent,
          "MP4 seek point is not the expected IDR");
    Check(!Take(demux.Read()), "Index query consumed or repositioned input");
    Check(Take(demux.Seek(1, {0, 1}))->byte_offset == point->byte_offset, "MP4 seek chose a different anchor");
    const std::array selection{1u};
    Take(demux.SelectTracks(selection));
    Check(std::get<MediaTrack>(*Take(demux.Read())).id == 1, "Seek did not replay the track configuration");
    Compare(std::get<ContainerPacket>(*Take(demux.Read())), video.packets[0]);
  } else {
    auto index = demux.FindSeekPoint(1, {1, 1});
    Check(!index.Succeeded() && index.Error().code == ContainerErrorCode::IndexUnavailable,
          "Fragmented input reported an index it does not have");
  }
  Take(demux.Close());
  Compare(*retained, video.packets[0]);

  if (fragmented) {
    auto bytes = Take(file.ReadBytes());
    Check(bytes.size() >= last_packet_size + 8, "Missing final fragment data box");
    auto header = std::span(bytes).subspan(bytes.size() - last_packet_size - 8, 8);
    constexpr std::array mdat{std::byte{'m'}, std::byte{'d'}, std::byte{'a'}, std::byte{'t'}};
    Check(std::ranges::equal(header.last(4), mdat), "Expected one sample in the last fragment data box");
    std::ranges::fill(header.first(4), std::byte{0});
    const auto open_ended = Path(path.parent_path() / "open-ended.mp4");
    Check(open_ended.WriteBytes(bytes), "Could not write open-ended MP4 sample");
    auto stream = Take(MediaDemuxer::Open(Take(open_ended.OpenRead())));
    std::size_t packets = 0;
    while (Take(stream.Read())) ++packets;
    Check(packets == video.packets.size() + audio.packets.size(), "Open-ended media data lost packets");
    Check(!Take(stream.Read()), "Open-ended media data did not remain at EOF");
    Take(stream.Close());

    // The last fragment has a declared sample that can no longer be read in full.
    const auto shortened = path.parent_path() / "truncated.mp4";
    fs::copy_file(path, shortened);
    fs::resize_file(shortened, fs::file_size(shortened) - 1);
    auto truncated = Take(MediaDemuxer::Open(Take(Path(shortened).OpenRead())));
    auto result = truncated.Read();
    while (result.Succeeded() && result.Value()) result = truncated.Read();
    Check(!result.Succeeded() && result.Error().code == ContainerErrorCode::InvalidData,
          "Truncated fragment was treated as EOF");
  }
}

void Wave(const fs::path& path, ContainerFormat format) {
  MediaTrack track;
  track.id = 1;
  track.kind = TrackKind::Audio;
  track.codec = format == ContainerFormat::Rf64 ? CodecId::PcmS24Le : CodecId::PcmU8;
  track.sample_rate = 48000;
  track.channel_count = format == ContainerFormat::Rf64 ? 6 : 1;
  const std::array tracks{track};
  const auto frame_bytes = format == ContainerFormat::Rf64 ? 18 : 1;
  Bytes bytes(3 * frame_bytes);
  for (std::size_t i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<std::byte>(i);
  auto mux = Take(MediaMuxer::Open(Output(path), format, tracks));
  auto retained = BytesRef(bytes);
  Check(!Take(mux.TryWrite({1, retained, {}, {}, {}, {}})), "WAVE packet was not accepted");
  Take(mux.Finish());

  auto sequential = Take(MediaDemuxer::Open(Take(Path(path).OpenRead())));
  Check(sequential.Format() == format && sequential.Tracks()[0].codec == track.codec &&
        sequential.Tracks()[0].channel_count == track.channel_count && sequential.Duration() == MediaTimestamp{3, 48000},
        "WAVE metadata differs after round trip");
  auto packet = std::get<ContainerPacket>(*Take(sequential.Read()));
  Check(std::ranges::equal(packet.data.AsBytes(), bytes), "WAVE data differs after round trip");
  Check(!Take(sequential.Read()) && !Take(sequential.Read()), "WAVE EOF or alignment pad is incorrect");

  auto demux = Take(MediaDemuxer::Open(Take(Path(path).OpenRandomAccess())));
  const auto point = Take(demux.Seek(1, {3, 96000}, SeekDirection::AtOrAfter));
  Check(point && point->time == MediaTimestamp{2, 48000} && point->kind == SeekPoint::Kind::Independent,
        "WAVE seek did not land on the expected sample");
  Check(std::get<MediaTrack>(*Take(demux.Read())).id == 1, "WAVE seek did not replay track description");
  packet = std::get<ContainerPacket>(*Take(demux.Read()));
  Check(packet.pts == MediaTimestamp{2, 48000} &&
        std::ranges::equal(packet.data.AsBytes(), std::span(bytes).last(frame_bytes)), "WAVE seek payload differs");
}

void Adts(const fs::path& path) {
  auto audio = Fixture(true, false);
  audio.track.id = 1;
  const std::array tracks{audio.track};
  auto mux = Take(MediaMuxer::Open(Take(Path(path).OpenWrite()), ContainerFormat::Adts, tracks));
  std::int64_t next = 0;
  for (auto& packet : audio.packets) {
    packet.track_id = 1;
    packet.pts = packet.dts = MediaTimestamp{next, 48000};
    packet.duration = MediaTimestamp{1024, 48000};
    Check(!Take(mux.TryWrite(packet)), "ADTS packet was not accepted");
    next += 1024;
  }
  auto trim = mux.EndTrack(1, 1024);
  Check(!trim.Succeeded() && trim.Error().code == ContainerErrorCode::Unsupported, "ADTS silently discarded trim");
  Take(mux.Finish());

  auto demux = Take(MediaDemuxer::Open(Take(Path(path).OpenRead())));
  Check(demux.Format() == ContainerFormat::Adts && demux.Tracks()[0].initialization_data == audio.track.initialization_data,
        "Wrong ADTS configuration");
  for (const auto& packet : audio.packets) Compare(std::get<ContainerPacket>(*Take(demux.Read())), packet);
  Check(!Take(demux.Read()) && demux.Duration() == MediaTimestamp{next, 48000}, "Wrong ADTS end or duration");
}

void Mov(const fs::path& destination, bool hevc, ContainerFormat format, bool in_band = false,
         ContainerLayout layout = ContainerLayout::Default) {
  const auto fixture = fs::path(MEDIA_FIXTURES) / "container" /
      (hevc ? "hevc-aac-portrait.mov" : "prores-pcm.mov");
  const auto file = Path(fixture);
  auto wrong_hint = MediaDemuxer::Open(Take(file.OpenRandomAccess()), ContainerFormat::Mp4);
  Check(!wrong_hint.Succeeded() && wrong_hint.Error().code == ContainerErrorCode::UnrecognizedFormat,
        "A QuickTime brand matched an MP4 hint");
  auto input = Take(MediaDemuxer::Open(Take(file.OpenRandomAccess()), ContainerFormat::Mov));
  Check(input.Format() == ContainerFormat::Mov && input.Tracks().size() == 2, "QuickTime brand or track count differs");

  std::vector<MediaTrack> tracks(input.Tracks().begin(), input.Tracks().end());
  Check(tracks[0].codec == (hevc ? CodecId::Hevc : CodecId::ProRes) &&
        tracks[0].sample_entry == (hevc ? "hvc1" : "apcn") &&
        tracks[0].rotation == (hevc ? VideoRotation::Clockwise90 : VideoRotation::None),
        "QuickTime video description differs");
  Check(tracks[1].codec == (hevc ? CodecId::Aac : CodecId::PcmS24Le), "QuickTime audio coding differs");
  const auto seek = Take(input.FindSeekPoint(tracks[0].id, {0, 1}));
  Check(seek && seek->kind == SeekPoint::Kind::Independent && seek->time == MediaTimestamp{0, 1},
        "QuickTime video did not expose an independent first frame");
  std::array<std::vector<ContainerPacket>, 2> packets;
  std::size_t pcm_bytes = 0;
  while (auto item = Take(input.Read())) {
    auto packet = std::get<ContainerPacket>(std::move(*item));
    const auto track = packet.track_id == tracks[0].id ? 0u : 1u;
    if (!hevc && track == 1) {
      const auto size = packet.data.AsBytes().size();
      Check(size % 6 == 0 && size > 6 && size <= 16384, "QuickTime PCM did not group aligned frames");
      pcm_bytes += size;
    }
    packets[track].push_back(std::move(packet));
  }
  Check(packets[0].size() == (hevc ? 6 : 4) && !packets[1].empty(), "Wrong QuickTime fixture packet count");
  if (!hevc) Check(pcm_bytes == 4000 * 6, "QuickTime PCM lost sample frames");
  Take(input.Close());

  if (in_band) {
    tracks[0].sample_entry = "hev1";
    // Repeat the configuration's VPS/SPS/PPS in the first access unit as permitted by hev1.
    const auto& config = tracks[0].initialization_data;
    const auto read16 = [&](std::size_t offset) {
      return (std::to_integer<unsigned>(config.at(offset)) << 8) | std::to_integer<unsigned>(config.at(offset + 1));
    };
    Check((std::to_integer<unsigned>(config.at(21)) & 3) == 3, "Expected four-byte HEVC NAL lengths");
    Bytes bytes;
    std::size_t offset = 23;
    while (offset < config.size()) {
      const auto type = std::to_integer<unsigned>(config.at(offset)) & 63;
      const auto count = read16(offset + 1);
      offset += 3;
      for (unsigned i = 0; i < count; ++i) {
        const auto length = read16(offset);
        Check(length <= config.size() - offset - 2, "Invalid HEVC fixture NAL length");
        if (type >= 32 && type <= 34) {
          bytes.insert(bytes.end(), 2, std::byte{0});
          bytes.insert(bytes.end(), config.begin() + offset, config.begin() + offset + 2 + length);
        }
        offset += 2 + length;
      }
    }
    Check(!bytes.empty(), "No HEVC parameter sets in the fixture");
    const auto original = packets[0].front().data.AsBytes();
    bytes.insert(bytes.end(), original.begin(), original.end());
    packets[0].front().data = BytesRef(std::move(bytes));
  }

  const bool fragmented = layout == ContainerLayout::Fragmented;
  if (fragmented) {
    for (auto& track : tracks) {
      track.priming_samples.reset();
      track.padding_samples.reset();
    }
    for (auto& group : packets) {
      for (auto& packet : group) {
        packet.pts->ticks += packet.pts->timescale;
        packet.dts->ticks += packet.dts->timescale;
      }
    }
  }
  auto output = fragmented
      ? Take(MediaMuxer::Open(Take(Path(destination).OpenWrite()), format, tracks, layout))
      : Take(MediaMuxer::Open(Output(destination), format, tracks));
  if (hevc && format == ContainerFormat::Mov) {
    auto packet = packets[0].front();
    const auto picture = packet.data.AsBytes();
    Bytes combined(picture.begin(), picture.end());
    combined.insert(combined.end(), picture.begin(), picture.end());
    packet.data = BytesRef(std::move(combined));
    const auto rejected = output.TryWrite(packet);
    Check(!rejected.Succeeded() && rejected.Error().code == ContainerErrorCode::InvalidArgument,
          "Mux accepted two HEVC pictures as one sample");
  }
  std::array<std::size_t, 2> cursor{};
  std::size_t track = 0, attempts = 0;
  while (cursor[0] < packets[0].size() || cursor[1] < packets[1].size()) {
    Check(++attempts <= 2 * (packets[0].size() + packets[1].size()), "QuickTime remux did not make progress");
    const auto need = Take(output.TryWrite(packets[track][cursor[track]]));
    if (need) {
      Check(*need == tracks[1 - track].id, "QuickTime remux requested the wrong track");
      track = 1 - track;
    } else if (++cursor[track] == packets[track].size()) {
      Take(output.EndTrack(tracks[track].id));
      track = 1 - track;
    }
  }
  Take(output.Finish());

  auto result = fragmented
      ? Take(MediaDemuxer::Open(Take(Path(destination).OpenRead()), format))
      : Take(MediaDemuxer::Open(Take(Path(destination).OpenRandomAccess()), format));
  Check(result.Format() == format && result.Layout() == layout && result.Tracks().size() == tracks.size() &&
        result.Tracks()[0].sample_entry == tracks[0].sample_entry &&
        result.Tracks()[0].rotation == tracks[0].rotation &&
        result.Tracks()[0].initialization_data == tracks[0].initialization_data &&
        result.Tracks()[1].codec == tracks[1].codec && result.Tracks()[1].channel_count == tracks[1].channel_count,
        "Remuxed video or audio description differs");
  cursor = {};
  while (auto item = Take(result.Read())) {
    const auto& packet = std::get<ContainerPacket>(*item);
    const auto index = packet.track_id == tracks[0].id ? 0u : 1u;
    Check(cursor[index] < packets[index].size(), "Unexpected remuxed packet");
    Compare(packet, packets[index][cursor[index]++]);
  }
  Check(cursor[0] == packets[0].size() && cursor[1] == packets[1].size(), "QuickTime remux lost packets");
  if (fragmented) return;
  const auto point = Take(result.Seek(tracks[0].id, {0, 1}));
  Check(point && point->kind == (in_band ? SeekPoint::Kind::SyncMarked : SeekPoint::Kind::Independent),
        "Remuxed video seek evidence differs");
  Check(std::get<MediaTrack>(*Take(result.Read())).rotation == tracks[0].rotation,
        "Seek did not deliver the remuxed rotation");
  Compare(std::get<ContainerPacket>(*Take(result.Read())), packets[0].front());
}

void EbmlRoundTrip(const fs::path& destination, std::string_view fixture, ContainerFormat format,
                   CodecId video_codec = CodecId::Unknown) {
  namespace ebml = huxerui::media::detail::ebml;
  const auto source = Path(fs::path(MEDIA_FIXTURES) / "container" / fixture);
  auto input = Take(MediaDemuxer::Open(Take(source.OpenRandomAccess()), format));
  std::vector<MediaTrack> tracks(input.Tracks().begin(), input.Tracks().end());
  const bool audio_only = video_codec == CodecId::Unknown;
  const bool webm = format == ContainerFormat::WebM;
  Check(input.Format() == format && tracks.size() == (audio_only ? 1u : 2u), "Wrong Matroska or WebM track count");
  Check(tracks.front().codec == (audio_only ? CodecId::Opus : video_codec) &&
        tracks.back().codec == (webm ? CodecId::Opus : CodecId::Aac), "Wrong Matroska or WebM codec mapping");
  if (video_codec == CodecId::Vp8) {
    Check(tracks.front().initialization_data.empty(), "WebM VP8 unexpectedly reported codec private data");
    auto invalid = tracks;
    invalid.front().initialization_data.push_back(std::byte{0});
    const auto result = MediaMuxer::ValidateOutput(format, invalid);
    Check(!result.Succeeded() && result.Error().code == ContainerErrorCode::InvalidArgument,
          "WebM VP8 accepted codec private data");
  }
  if (webm) {
    Check(tracks.back().seek_preroll == MediaTimestamp{80, 1000} &&
          tracks.back().priming_samples == 312, "Opus seek preroll or pre-skip differs");
  }
  const auto time = MediaTimestamp{audio_only ? 4500 : 450, 1000};
  const auto external_point = Take(input.FindSeekPoint(tracks.front().id, time));
  Check(external_point && external_point->byte_offset, "Independent fixture Cues did not resolve a Block");
  if (audio_only) {
    Check(external_point->kind == SeekPoint::Kind::NeedsPreroll && external_point->decode_start &&
          external_point->decode_start->Compare(external_point->time) < 0, "Opus Cues did not resolve earlier preroll");
  }

  std::vector<std::vector<ContainerPacket>> packets(tracks.size());
  bool positive_padding = false;
  while (auto item = Take(input.Read())) {
    auto packet = std::get<ContainerPacket>(std::move(*item));
    Check(packet.pts && !packet.dts, "Matroska Block did not preserve PTS or fabricated DTS");
    positive_padding |= packet.discard_padding && packet.discard_padding->ticks > 0;
    Check(packet.track_id > 0 && packet.track_id <= tracks.size(), "Unexpected Matroska track identity");
    packets[packet.track_id - 1].push_back(std::move(packet));
  }
  if (webm) Check(positive_padding, "Opus end trim was lost");
  if (!webm) {
    const auto& video = packets.front();
    Check(std::adjacent_find(video.begin(), video.end(), [](const auto& a, const auto& b) {
      return a.pts->Compare(*b.pts) > 0;
    }) != video.end(), "Matroska fixture did not preserve decode order with B pictures");
  }
  Take(input.Close());

  Check(Take(MediaMuxer::ValidateOutput(format, tracks)) == MediaMuxer::OutputRequirement::SequentialWrite,
        "Matroska or WebM unexpectedly requires backpatching");
  auto output = Take(MediaMuxer::Open(Take(Path(destination).OpenWrite()), format, tracks));
  if (audio_only) {
    auto short_packet = packets.front().front();
    short_packet.duration = MediaTimestamp{10, 1000};
    short_packet.discard_padding = MediaTimestamp{15, 1000};
    auto rejected = output.TryWrite(short_packet);
    Check(!rejected.Succeeded() && rejected.Error().code == ContainerErrorCode::InvalidArgument,
          "WebM accepted trim exceeding an explicit packet duration");
    short_packet.duration = MediaTimestamp{104, 10000};
    short_packet.discard_padding = MediaTimestamp{102, 10000};
    rejected = output.TryWrite(short_packet);
    Check(!rejected.Succeeded() && rejected.Error().code == ContainerErrorCode::Unsupported,
          "WebM accepted trim exceeding the serialized BlockDuration");
    packets.front().front().discard_padding = MediaTimestamp{0, 1};
  }
  std::vector<std::size_t> cursor(tracks.size());
  std::size_t track = 0;
  for (std::size_t attempts = 0; track < tracks.size();) {
    Check(++attempts < 1000, "Matroska or WebM mux did not make progress");
    const auto need = Take(output.TryWrite(packets[track][cursor[track]]));
    if (need) {
      Check(*need > 0 && *need <= tracks.size() && cursor[*need - 1] < packets[*need - 1].size(),
            "Matroska mux requested a completed track");
      track = *need - 1;
    } else if (++cursor[track] == packets[track].size()) {
      Take(output.EndTrack(tracks[track].id));
      track = 0;
      while (track < tracks.size() && cursor[track] == packets[track].size()) ++track;
    }
  }
  Take(output.Finish());

  auto readback = Take(MediaDemuxer::Open(Take(Path(destination).OpenRead()), format));
  Check(readback.Tracks().size() == tracks.size() &&
        readback.Tracks().front().codec == tracks.front().codec &&
        readback.Tracks().front().initialization_data == tracks.front().initialization_data,
        "Matroska or WebM output lost a track or changed its codec mapping");
  std::fill(cursor.begin(), cursor.end(), 0);
  while (auto item = Take(readback.Read())) {
    const auto& packet = std::get<ContainerPacket>(*item);
    const auto index = packet.track_id - 1;
    Check(index < tracks.size() && cursor[index] < packets[index].size(), "Unexpected remuxed Matroska packet");
    const auto& expected = packets[index][cursor[index]++];
    Check(packet.pts && std::abs(ebml::ToNs(*packet.pts) - ebml::ToNs(*expected.pts)) <= 500000 &&
          !packet.dts && packet.sync_sample == expected.sync_sample && packet.discard_padding == expected.discard_padding &&
          std::ranges::equal(packet.data.AsBytes(), expected.data.AsBytes()), "Matroska packet differs after remux");
    if (expected.duration) {
      Check(packet.duration && std::abs(ebml::ToNs(*packet.duration) - ebml::ToNs(*expected.duration)) <= 1000000,
            "Matroska Block duration exceeded the output grid error");
    }
  }
  for (std::size_t i = 0; i < cursor.size(); ++i) Check(cursor[i] == packets[i].size(), "Matroska remux lost packets");
  auto seekable = Take(MediaDemuxer::Open(Take(Path(destination).OpenRandomAccess()), format));
  const auto point = Take(seekable.Seek(tracks.front().id, time));
  Check(point && point->byte_offset, "Tail Cues did not resolve a Block");
  const auto expected = audio_only ? point->decode_start : std::optional{point->time};
  Check(expected && (!audio_only || (point->kind == SeekPoint::Kind::NeedsPreroll && expected->Compare(point->time) < 0)),
        "Matroska Seek did not preserve the decode start");
  Check(std::get<MediaTrack>(*Take(seekable.Read())).id == tracks.front().id, "Matroska Seek did not replay track description");
  Check(std::get<ContainerPacket>(*Take(seekable.Read())).pts == expected, "Matroska Seek landed at the wrong Block");
}

void MatroskaLacing(const fs::path& path, bool corrupt) {
  using namespace huxerui::media::detail;
  using namespace huxerui::media::detail::ebml;
  const auto audio = Fixture(true, false);
  Bytes bytes;
  Master(bytes, Header, [](Bytes& header) { PutText(header, 0x4282, "matroska"); });
  PutId(bytes, Segment);
  Put<std::uint8_t>(bytes, 0xff);
  Master(bytes, Info, [](Bytes&) {});
  Master(bytes, Tracks, [&](Bytes& tracks) {
    Master(tracks, 0xae, [&](Bytes& track) {
      PutUInt(track, 0xd7, 1);
      PutUInt(track, 0x83, 2);
      PutText(track, 0x86, "A_AAC");
      PutElement(track, 0x63a2, audio.track.initialization_data);
      Master(track, 0xe1, [](Bytes& parameters) {
        PutFloat(parameters, 0xb5, 48000);
        PutUInt(parameters, 0x9f, 2);
      });
    });
  });
  // Unknown-sized sibling Clusters exercise both boundary detection and all three lace encodings.
  const std::array<std::vector<std::size_t>, 3> sizes{{{3, 4}, {3, 3}, {3, 2, 4}}};
  for (unsigned i = 0; i < sizes.size(); ++i) {
    PutId(bytes, Cluster);
    Put<std::uint8_t>(bytes, 0xff);
    PutUInt(bytes, Timestamp, i * 100);
    Bytes block;
    PutVint(block, 1);
    Put<std::int16_t>(block, 0);
    Put<std::uint8_t>(block, static_cast<std::uint8_t>(128 | ((i + 1) << 1)));
    Put<std::uint8_t>(block, static_cast<std::uint8_t>(sizes[i].size() - 1));
    if (i == 0) Put<std::uint8_t>(block, 3);
    if (i == 2) {
      PutVint(block, corrupt ? 100 : 3);
      PutVint(block, 62);
    }
    for (std::size_t j = 0; j < sizes[i].size(); ++j)
      block.insert(block.end(), sizes[i][j], static_cast<std::byte>(j + 1));
    PutElement(bytes, SimpleBlock, block);
  }
  Master(bytes, BlockGroup, [](Bytes& group) {
    Bytes block;
    PutVint(block, 1);
    Put<std::int16_t>(block, 80);
    Put<std::uint8_t>(block, 0);
    Put<std::uint8_t>(block, 1);
    PutElement(group, Block, block);
    PutInt(group, DiscardPadding, -1000000);
  });
  if (corrupt) {
    Master(bytes, Cues, [](Bytes& entries) {
      Master(entries, 0xbb, [](Bytes& point) {
        PutUInt(point, 0xb3, 0);
        Master(point, 0xb7, [](Bytes& position) {
          PutUInt(position, 0xf7, 1);
          PutUInt(position, 0xf1, 1u << 30);
        });
      });
    });
  }
  Check(Path(path).WriteBytes(bytes), "Could not write synthetic Matroska lace");
  auto input = Take(MediaDemuxer::Open(Take(Path(path).OpenRead())));
  for (unsigned i = 0; i < sizes.size(); ++i) {
    for (std::size_t j = 0; j < sizes[i].size(); ++j) {
      auto item = input.Read();
      if (corrupt && i == 2) {
        Check(!item.Succeeded() && item.Error().code == ContainerErrorCode::InvalidData,
              "Matroska demux accepted a corrupt lace length");
        auto random = Take(MediaDemuxer::Open(Take(Path(path).OpenRandomAccess())));
        const auto point = random.FindSeekPoint(1, {0, 1});
        Check(!point.Succeeded() && point.Error().code == ContainerErrorCode::InvalidData,
              "Matroska demux accepted an out-of-range Cue");
        return;
      }
      const auto packet = std::get<ContainerPacket>(*Take(std::move(item)));
      Check(packet.data.AsBytes().size() == sizes[i][j] &&
            packet.data.AsBytes().front() == static_cast<std::byte>(j + 1) &&
            packet.pts == MediaTimestamp{static_cast<std::int64_t>(i * 100000000) +
                         ToNs({static_cast<std::int64_t>(j * 1024), 48000}), Nanoseconds},
            "Matroska lace lost a packet boundary or timestamp");
    }
  }
  const auto trimmed = std::get<ContainerPacket>(*Take(input.Read()));
  Check(trimmed.pts == MediaTimestamp{280, 1000} && trimmed.discard_padding == MediaTimestamp{-1, 1000},
        "Matroska did not preserve leading packet trim");
  Check(!Take(input.Read()), "Matroska did not end after its last unknown-sized Cluster");
  auto random = Take(MediaDemuxer::Open(Take(Path(path).OpenRandomAccess())));
  const auto point = random.FindSeekPoint(1, {0, 1});
  Check(!point.Succeeded() && point.Error().code == ContainerErrorCode::IndexUnavailable,
        "Matroska reported an index for a file with no Cues");
}
} // namespace

int main(int argc, char** argv) {
  const auto path = argc == 2 ? fs::path(argv[1]) : fs::temp_directory_path() /
      ("huxerui-container-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  const std::array names{"regular.mp4", "fragmented.mp4", "audio.wav", "audio.rf64.wav", "audio.aac",
                         "hevc.mov", "hevc.mp4", "hev1.mp4", "prores-pcm.mov", "hevc-fragmented.mp4",
                         "hevc-aac.mkv", "vp9-opus.webm", "opus.webm", "lace.mka", "lace-invalid.mka",
                         "avc-aac.mkv", "vp8-opus.webm"};
  try {
    Check(fs::create_directory(path), "Could not create a fresh container test directory");
    Mp4(path / names[0], false);
    Mp4(path / names[1], true);
    Wave(path / names[2], ContainerFormat::Wave);
    Wave(path / names[3], ContainerFormat::Rf64);
    Adts(path / names[4]);
    Mov(path / names[5], true, ContainerFormat::Mov);
    Mov(path / names[6], true, ContainerFormat::Mp4);
    Mov(path / names[7], true, ContainerFormat::Mp4, true);
    Mov(path / names[8], false, ContainerFormat::Mov);
    Mov(path / names[9], true, ContainerFormat::Mp4, false, ContainerLayout::Fragmented);
    EbmlRoundTrip(path / names[10], "mkv-hevc-aac.mkv", ContainerFormat::Matroska, CodecId::Hevc);
    EbmlRoundTrip(path / names[11], "webm-vp9-opus.webm", ContainerFormat::WebM, CodecId::Vp9);
    EbmlRoundTrip(path / names[12], "webm-opus.webm", ContainerFormat::WebM);
    MatroskaLacing(path / names[13], false);
    MatroskaLacing(path / names[14], true);
    EbmlRoundTrip(path / names[15], "mkv-avc-aac.mkv", ContainerFormat::Matroska, CodecId::H264);
    EbmlRoundTrip(path / names[16], "webm-vp8-opus.webm", ContainerFormat::WebM, CodecId::Vp8);
    if (argc != 2) {
      for (auto name : names) fs::remove(path / name);
      fs::remove(path / "open-ended.mp4");
      fs::remove(path / "truncated.mp4");
      fs::remove(path);
    }
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  return 0;
}
