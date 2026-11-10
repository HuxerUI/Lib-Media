#include <huxerui/file.h>
#include <huxerui/media_container.h>

#include "container/ebml.h"
#include "container/mp4.h"

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
  if (fragmented && !audio) {
    const auto count = fixture.packets.size();
    const auto span = fixture.packets.back().dts->ticks + fixture.packets.back().duration->ticks -
                      fixture.packets.front().dts->ticks;
    for (std::size_t i = 0; i < count; ++i) {
      auto packet = fixture.packets[i];
      packet.pts->ticks += span;
      packet.dts->ticks += span;
      fixture.packets.push_back(std::move(packet));
    }
  }
  return fixture;
}

void Compare(const ContainerPacket& actual, const ContainerPacket& expected) {
  Check(actual.track_id == expected.track_id && actual.pts == expected.pts && actual.dts == expected.dts &&
        actual.duration == expected.duration && actual.sync_sample == expected.sync_sample &&
        std::ranges::equal(actual.data.AsBytes(), expected.data.AsBytes()), "Container packet differs after round trip");
}

void Mp4(const fs::path& path, ContainerLayout layout) {
  const bool fragmented = layout == ContainerLayout::Fragmented;
  const bool fast_start = layout == ContainerLayout::FastStart;
  const auto video = Fixture(false, fragmented);
  const auto audio = Fixture(true, fragmented);
  const std::array tracks{video.track, audio.track};
  using Requirement = MediaMuxer::OutputRequirement;
  const auto requirement = Take(MediaMuxer::ValidateOutput(ContainerFormat::Mp4, tracks, layout));
  Check(requirement == (fragmented ? Requirement::SequentialWrite :
                       fast_start ? Requirement::RandomReadWrite : Requirement::RandomWrite),
        "Wrong MP4 output requirement");
  const auto file = Path(path);
  if (fast_start) {
    auto write_only = Output(path);
    auto rejected = MediaMuxer::Open(std::move(write_only), ContainerFormat::Mp4, tracks, layout);
    Check(!rejected.Succeeded() && rejected.Error().code == ContainerErrorCode::InvalidArgument &&
          fs::file_size(path) == 0, "Fast-start accepted a write-only destination");
  }
  auto mux = fragmented
      ? Take(MediaMuxer::Open(Take(file.OpenWrite()), ContainerFormat::Mp4, tracks, layout))
      : Take(MediaMuxer::Open(fast_start ? Take(file.OpenRandomAccess(FileAccessMode::ReadWrite)) : Output(path),
                             ContainerFormat::Mp4, tracks, layout));

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

  if (!fragmented && !fast_start) {
    auto rejected = MediaDemuxer::Open(Take(file.OpenRead()));
    Check(!rejected.Succeeded() && rejected.Error().code == ContainerErrorCode::NeedRandomAccess,
          "Tail metadata did not request random access");
  }
  auto demux = fragmented
      ? Take(MediaDemuxer::Open(Take(file.OpenRead())))
      : Take(MediaDemuxer::Open(Take(file.OpenRandomAccess())));
  Check(demux.Layout() == (fragmented ? layout : ContainerLayout::Default) && demux.Tracks().size() == 2,
        "Wrong MP4 layout or track count");
  if (fast_start) {
    auto sequential = Take(MediaDemuxer::Open(Take(file.OpenRead())));
    std::size_t count = 0;
    while (Take(sequential.Read())) ++count;
    Check(count == video.packets.size() + audio.packets.size(), "Fast-start MP4 cannot be read sequentially");
  }
  Check(demux.Tracks()[0].visible == video.track.visible, "MP4 clean aperture was not retained");
  if (!fragmented) {
    Check(demux.Tracks()[1].priming_samples == 1024 && demux.Tracks()[1].padding_samples == 288,
          "MP4 AAC edit list did not retain trim");
    const auto& first = audio.packets.front();
    const auto& last = audio.packets.back();
    const MediaPresentationRange expected{{first.pts->ticks + 1024, 48000},
        {last.pts->ticks + last.duration->ticks - 288, 48000}};
    Check(demux.Tracks()[1].presentation_edit == expected && demux.PresentationRange(audio.track.id) == expected,
          "MP4 AAC presentation range includes trimmed packets");
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
    auto indexed = Take(MediaDemuxer::Open(Take(file.OpenRandomAccess())));
    Check(!indexed.PresentationRange(video.track.id), "Unindexed fragments reported sample coverage");
    const auto requirement = Take(MediaMuxer::ValidateOutput(ContainerFormat::Mp4, indexed.Tracks(), layout));
    const std::array selected{video.track.id};
    Take(indexed.SelectTracks(selected));
    Compare(std::get<ContainerPacket>(*Take(indexed.Read())), video.packets[0]);
    const auto& second_idr = video.packets[video.packets.size() / 2];
    const auto future = Take(indexed.FindSeekPoint(video.track.id, *video.packets[1].pts, SeekDirection::AtOrAfter));
    Check(future && future->time == *second_idr.pts && future->kind == SeekPoint::Kind::Independent,
          "Fragment index did not find the next independent sample");
    Compare(std::get<ContainerPacket>(*Take(indexed.Read())), video.packets[1]);
    const auto video_range = indexed.PresentationRange(video.track.id);
    Check(indexed.Duration() && video_range && indexed.PresentationRange(audio.track.id) &&
          !indexed.PresentationRange(0),
          "Fragment index did not establish presentation coverage");
    Check(!indexed.Tracks()[0].presentation_edit && !indexed.Tracks()[1].presentation_edit &&
          Take(MediaMuxer::ValidateOutput(ContainerFormat::Mp4, indexed.Tracks(), layout)) == requirement,
          "Building an index made unedited tracks incompatible with fragmented output");
    auto edited = indexed.Tracks()[0];
    edited.presentation_edit = video_range;
    const auto unsupported = MediaMuxer::ValidateOutput(ContainerFormat::Mp4, std::span(&edited, 1), layout);
    Check(!unsupported.Succeeded() && unsupported.Error().code == ContainerErrorCode::Unsupported,
          "Fragmented output accepted an authored edit");
    for (const auto* expected : {&second_idr, &video.packets[0], &second_idr}) {
      const auto point = Take(indexed.Seek(video.track.id, *expected->pts));
      Check(point && point->time == *expected->pts && point->kind == SeekPoint::Kind::Independent,
            "Fragment seek changed its anchor");
      Check(std::get<MediaTrack>(*Take(indexed.Read())).id == video.track.id &&
            indexed.PresentationRange(video.track.id) == video_range, "Fragment seek changed the track or its coverage");
      Compare(std::get<ContainerPacket>(*Take(indexed.Read())), *expected);
    }

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
    auto damaged_index = Take(MediaDemuxer::Open(Take(Path(shortened).OpenRandomAccess())));
    const auto failed = damaged_index.FindSeekPoint(video.track.id, {1, 1});
    Check(!failed.Succeeded() && failed.Error().code == ContainerErrorCode::InvalidData,
          "Truncated fragments produced a seek index");
    Take(damaged_index.SelectTracks(selected));
    Compare(std::get<ContainerPacket>(*Take(damaged_index.Read())), video.packets.front());
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

void AudioFormats(const fs::path& directory) {
  const auto fixtures = fs::path(MEDIA_FIXTURES) / "container";
  struct Sample {
    const char* name;
    ContainerFormat format;
    CodecId codec;
    MediaTimestamp duration;
  };
  const std::array samples{
      Sample{"preview-opus.ogg", ContainerFormat::Ogg, CodecId::Opus, {107376, 48000}},
      Sample{"preview-flac.flac", ContainerFormat::Flac, CodecId::Flac, {50142, 44100}},
      Sample{"preview-mp3.mp3", ContainerFormat::Mp3, CodecId::Mp3, {50142, 44100}}};
  for (const auto& sample : samples) {
    const auto file = Path(fixtures / sample.name);
    auto reader = Take(MediaDemuxer::Open(Take(file.OpenRead())));
    Check(reader.Format() == sample.format && reader.Tracks().size() == 1 &&
          reader.Tracks().front().codec == sample.codec, "Wrong native audio format or codec");
    unsigned count = 0;
    std::optional<ContainerPacket> last;
    while (auto item = Take(reader.Read())) {
      last = std::get<ContainerPacket>(std::move(*item));
      Check(last->duration.has_value(), "Native audio frame lost its sample count");
      ++count;
    }
    if (count <= 2 || reader.Duration() != sample.duration) {
      const auto actual = reader.Duration();
      throw std::runtime_error(std::string(sample.name) + " duration differs from the reference (ticks " +
                               (actual ? std::to_string(actual->ticks) : "unknown") + ")");
    }
    auto random = Take(MediaDemuxer::Open(Take(file.OpenRandomAccess())));
    const auto anchor = Take(random.FindSeekPoint(1, {1, 1}));
    Check(anchor && anchor->time.Compare({1, 1}) <= 0, "Native audio seek has no covered anchor");
    Check(std::holds_alternative<ContainerPacket>(*Take(random.Read())), "Native audio query consumed the first packet");
    for (auto target : {MediaTimestamp{1, 2}, MediaTimestamp{1, 5}, MediaTimestamp{1, 1}}) {
      const auto found = Take(random.Seek(1, target));
      Check(found && found->time.Compare(target) <= 0, "Repeated native audio seek has no anchor");
      Check(std::holds_alternative<MediaTrack>(*Take(random.Read())), "Native audio seek lost its track description");
      const auto packet = std::get<ContainerPacket>(*Take(random.Read()));
      Check(packet.pts == found->decode_start.value_or(found->time), "Native audio seek used the wrong decode start");
    }
    auto truncated = Take(file.ReadBytes());
    truncated.pop_back();
    const auto damaged = Path(directory / "truncated-audio");
    Check(damaged.WriteBytes(truncated), "Could not create a truncated audio sample");
    auto broken = Take(MediaDemuxer::Open(Take(damaged.OpenRead())));
    auto result = broken.Read();
    while (result.Succeeded() && result.Value()) result = broken.Read();
    Check(!result.Succeeded() && result.Error().code == ContainerErrorCode::InvalidData,
          "Truncated audio frame was treated as clean EOF");
  }

  const auto opus = Path(fixtures / "webm-opus.webm");
  auto webm = Take(MediaDemuxer::Open(Take(opus.OpenRead())));
  const auto target = Path(directory / "remux.ogg");
  auto ogg = Take(MediaMuxer::Open(Take(target.OpenWrite()), ContainerFormat::Ogg, webm.Tracks()));
  std::vector<ContainerPacket> expected;
  while (auto item = Take(webm.Read())) {
    auto packet = std::get<ContainerPacket>(std::move(*item));
    Check(!Take(ogg.TryWrite(packet)), "Single-track Ogg requested more input tracks");
    expected.push_back(std::move(packet));
  }
  Take(ogg.Finish());
  auto reopened = Take(MediaDemuxer::Open(Take(target.OpenRead())));
  Check(reopened.Tracks().front().initialization_data == webm.Tracks().front().initialization_data,
        "OpusHead changed during remux");
  const auto reverse = Path(directory / "remux.webm");
  auto back = Take(MediaMuxer::Open(Take(reverse.OpenWrite()), ContainerFormat::WebM, reopened.Tracks()));
  std::size_t index = 0;
  while (auto item = Take(reopened.Read())) {
    const auto packet = std::get<ContainerPacket>(std::move(*item));
    Check(index < expected.size() && packet.duration == expected[index].duration &&
          packet.discard_padding == expected[index].discard_padding &&
          std::ranges::equal(packet.data.AsBytes(), expected[index].data.AsBytes()),
          "WebM to Ogg remux changed Opus packets or tail trim");
    Check(!Take(back.TryWrite(packet)), "Single-track WebM requested more input tracks");
    ++index;
  }
  Take(back.Finish());
  Check(index == expected.size(), "WebM to Ogg remux lost packets");
  auto roundtrip = Take(MediaDemuxer::Open(Take(reverse.OpenRead())));
  index = 0;
  while (auto item = Take(roundtrip.Read())) {
    const auto packet = std::get<ContainerPacket>(std::move(*item));
    Check(index < expected.size() && packet.duration == expected[index].duration &&
          packet.discard_padding == expected[index].discard_padding &&
          std::ranges::equal(packet.data.AsBytes(), expected[index].data.AsBytes()),
          "Ogg to WebM remux changed Opus packets or tail trim");
    ++index;
  }
  Check(index == expected.size(), "Ogg to WebM remux lost packets");

  auto links = Take(Path(fixtures / "preview-opus.ogg").ReadBytes());
  const auto next_link = Take(target.ReadBytes());
  Check(!std::ranges::equal(std::span(links).subspan(14, 4), std::span(next_link).subspan(14, 4)),
        "Chained Ogg fixture must use distinct stream serial numbers");
  links.insert(links.end(), next_link.begin(), next_link.end());
  const auto chained = Path(directory / "chained.ogg");
  Check(chained.WriteBytes(links), "Could not write chained Ogg input");
  const auto boundary = MediaTimestamp{107376, 48000};
  const auto second_duration = reopened.Duration()->Rescale(48000)->ticks;
  for (const bool random : {false, true}) {
    auto reader = random ? Take(MediaDemuxer::Open(Take(chained.OpenRandomAccess())))
                         : Take(MediaDemuxer::Open(Take(chained.OpenRead())));
    unsigned descriptions = 0;
    std::optional<MediaTimestamp> next_start;
    while (auto item = Take(reader.Read())) {
      if (const auto* track = std::get_if<MediaTrack>(&*item)) {
        ++descriptions;
        next_start = {boundary.ticks - static_cast<std::int64_t>(*track->priming_samples), 48000};
      } else if (next_start) {
        Check(std::get<ContainerPacket>(*item).pts == next_start, "Second Ogg link lost its leading trim or offset");
        next_start.reset();
      }
    }
    Check(descriptions == 1 && reader.Duration() == MediaTimestamp{boundary.ticks + second_duration, 48000},
          "Chained Ogg did not expose both links on one presentation timeline");
  }
  auto indexed = Take(MediaDemuxer::Open(Take(chained.OpenRandomAccess())));
  const auto first_config = indexed.Tracks().front().initialization_data;
  const auto second_config = reopened.Tracks().front().initialization_data;
  const auto next = Take(indexed.FindSeekPoint(1, boundary));
  Check(next && next->byte_offset >= links.size() - next_link.size() && indexed.Tracks().front().initialization_data == first_config,
        "Ogg link index query changed the active description");
  Check(std::holds_alternative<ContainerPacket>(*Take(indexed.Read())), "Ogg index query moved the read cursor");
  for (auto target : {boundary, MediaTimestamp{1, 1}, MediaTimestamp{3, 1}, boundary}) {
    const auto point = Take(indexed.Seek(1, target));
    const bool second = target.Compare(boundary) >= 0;
    Check(point && point->decode_start && (second == (*point->byte_offset >= links.size() - next_link.size())),
          "Ogg seek landed in the wrong link");
    const auto track = std::get<MediaTrack>(*Take(indexed.Read()));
    const auto packet = std::get<ContainerPacket>(*Take(indexed.Read()));
    Check(track.initialization_data == (second ? second_config : first_config) && packet.pts == point->decode_start,
          "Ogg seek replayed the wrong link or preroll");
  }

  for (const auto format : {ContainerFormat::Flac, ContainerFormat::Mp3}) {
    const bool flac = format == ContainerFormat::Flac;
    const auto input = Path(fixtures / (flac ? "tagged-flac.flac" : "tagged-mp3.mp3"));
    const auto copy = Path(directory / (flac ? "copy.flac" : "copy.mp3"));
    auto audio = Take(MediaDemuxer::Open(Take(input.OpenRandomAccess())));
    const auto track = audio.Tracks().front();
    const auto& tags = track.audio_metadata;
    Check(tags && tags->title == "试听 Café" && tags->artist == "Lib-Media" && tags->album == "Audio Fixtures" &&
          tags->track_number == "2/12" && tags->cover && tags->cover->mime_type == "image/png",
          "Audio tags or front cover were not exposed");
    Check(Take(MediaMuxer::ValidateOutput(format, audio.Tracks())) == MediaMuxer::OutputRequirement::RandomWrite,
          "Native audio copy must request random writes");
    auto mux = Take(MediaMuxer::Open(Output(directory / (flac ? "copy.flac" : "copy.mp3")), format, audio.Tracks()));
    std::vector<ContainerPacket> packets;
    while (auto item = Take(audio.Read())) {
      packets.push_back(std::get<ContainerPacket>(std::move(*item)));
      Check(!Take(mux.TryWrite(packets.back())), "Single-track audio output requested another packet");
    }
    Take(mux.Finish());
    auto readback = Take(MediaDemuxer::Open(Take(copy.OpenRandomAccess())));
    const auto descriptions = readback.Tracks();
    const auto& metadata = descriptions.front().audio_metadata;
    Check(readback.Duration() == audio.Duration() && metadata && metadata->title == tags->title &&
          metadata->artist == tags->artist && metadata->album == tags->album && metadata->cover &&
          std::ranges::equal(metadata->cover->data.AsBytes(), tags->cover->data.AsBytes()), "Audio copy lost metadata or trim");
    for (const auto& packet : packets) Compare(std::get<ContainerPacket>(*Take(readback.Read())), packet);
    Check(!Take(readback.Read()), "Audio copy added an access unit");
    for (const auto target : {MediaTimestamp{1, 1}, MediaTimestamp{1, 5}, MediaTimestamp{4, 5}}) {
      const auto point = Take(readback.Seek(1, target));
      Check(point && point->time.Compare(target) <= 0 && std::holds_alternative<MediaTrack>(*Take(readback.Read())),
            "Audio copy could not seek to a covered packet");
      Check(std::get<ContainerPacket>(*Take(readback.Read())).pts == point->decode_start.value_or(point->time),
            "Audio copy seek landed at the wrong packet");
    }
  }
}

void EmptyM4aMetadata(const fs::path& directory) {
  using huxerui::media::detail::Number, huxerui::media::detail::SetU32, huxerui::media::detail::Tag;
  const auto input = Path(fs::path(MEDIA_FIXTURES) / "container" / "tagged-aac.m4a");
  const auto original = Take(input.ReadBytes());
  for (const std::string_view tag : {"udta", "ilst"}) {
    auto bytes = original;
    const auto name = std::as_bytes(std::span(tag.data(), tag.size()));
    const auto found = std::ranges::search(bytes, name);
    Check(!found.empty() && found.begin() - bytes.begin() >= 4, "Fixture is missing a metadata box");
    const auto box = static_cast<std::size_t>(found.begin() - bytes.begin()) - 4;
    const auto size = Number<std::uint32_t>(bytes, box);
    Check(size >= 16 && size <= bytes.size() - box, "Fixture metadata box is too small");
    // Preserve the file offsets by replacing the payload with a sibling free box.
    SetU32(bytes, box, 8);
    SetU32(bytes, box + 8, size - 8);
    SetU32(bytes, box + 12, Tag("free"));
    const auto output = Path(directory / ("empty-" + std::string(tag) + ".m4a"));
    Check(output.WriteBytes(bytes), "Could not stage empty M4A metadata");
    auto reader = Take(MediaDemuxer::Open(Take(output.OpenRandomAccess())));
    const auto& track = reader.Tracks().front();
    Check(track.codec == CodecId::Aac && !track.audio_metadata &&
          reader.PresentationRange(track.id) == MediaPresentationRange{{0, 1}, {52800, 48000}},
          "Empty optional M4A metadata affected the audio track");
    unsigned count = 0;
    while (Take(reader.Read())) ++count;
    Check(count == 53, "Empty optional M4A metadata affected the audio packets");
  }
}

void M4a(const fs::path& directory) {
  const auto fixtures = fs::path(MEDIA_FIXTURES) / "container";
  for (const auto codec : {CodecId::Aac, CodecId::Alac}) {
    const bool aac = codec == CodecId::Aac;
    const auto input = Path(fixtures / (aac ? "tagged-aac.m4a" : "tagged-alac.m4a"));
    const auto output = Path(directory / (aac ? "copy-aac.m4a" : "copy-alac.m4a"));
    auto reader = Take(MediaDemuxer::Open(Take(input.OpenRandomAccess())));
    const auto track = reader.Tracks().front();
    const std::uint32_t rate = aac ? 48000 : 96000;
    const std::int64_t samples = aac ? 52800 : 10560;
    const MediaPresentationRange range{{0, 1}, {samples, rate}};
    Check(reader.Format() == ContainerFormat::Mp4 && reader.Tracks().size() == 1 &&
        track.codec == codec && track.sample_rate == rate && track.channel_count == 2 &&
        reader.PresentationRange(track.id) == range, "Wrong M4A coding or effective presentation range");
    Check(track.priming_samples.value_or(0) == (aac ? 1024 : 0), "Wrong M4A encoder priming");
    const auto& tags = track.audio_metadata;
    Check(tags && tags->title == "试听 Café" && tags->artist == "Lib-Media" && tags->album == "Audio Fixtures" &&
        tags->track_number == "2/12" && tags->genre == "Test" && tags->date == "2026" &&
        tags->cover && tags->cover->mime_type == "image/png", "Wrong M4A tags or cover");
    auto mux = Take(MediaMuxer::Open(Take(output.OpenRandomAccess(FileAccessMode::ReadWrite, FileOpenMode::CreateNew)),
        ContainerFormat::Mp4, reader.Tracks(), ContainerLayout::FastStart));
    std::vector<ContainerPacket> packets;
    while (auto item = Take(reader.Read())) {
      packets.push_back(std::get<ContainerPacket>(std::move(*item)));
      Check(!Take(mux.TryWrite(packets.back())), "M4A writer did not accept the packet");
    }
    Check(packets.size() == (aac ? 53 : 3) && packets.back().duration == MediaTimestamp{aac ? 576 : 2368, rate},
        "M4A final packet lost its actual duration");
    Take(mux.Finish());
    auto sequential = Take(MediaDemuxer::Open(Take(output.OpenRead()), ContainerFormat::Mp4));
    const auto copy = sequential.Tracks().front();
    const auto& metadata = copy.audio_metadata;
    Check(copy.codec == codec && copy.initialization_data == track.initialization_data &&
        copy.priming_samples == track.priming_samples && copy.padding_samples == track.padding_samples &&
        sequential.PresentationRange(copy.id) == range, "M4A copy lost format or gapless trim");
    Check(metadata && metadata->title == tags->title && metadata->artist == tags->artist &&
        metadata->album == tags->album && metadata->track_number == tags->track_number &&
        metadata->genre == tags->genre && metadata->date == tags->date && metadata->cover &&
        metadata->cover->mime_type == tags->cover->mime_type &&
        std::ranges::equal(metadata->cover->data.AsBytes(), tags->cover->data.AsBytes()), "M4A copy lost tags or cover bytes");
    for (const auto& packet : packets) Compare(std::get<ContainerPacket>(*Take(sequential.Read())), packet);
    Check(!Take(sequential.Read()), "M4A copy added packets");
    auto random = Take(MediaDemuxer::Open(Take(output.OpenRandomAccess())));
    for (const auto ticks : {samples / 2, std::int64_t(0), samples - 1}) {
      const auto point = Take(random.Seek(copy.id, {ticks, rate}));
      Check(point && std::holds_alternative<MediaTrack>(*Take(random.Read())), "M4A copy lost its seek index");
      const auto packet = std::get<ContainerPacket>(*Take(random.Read()));
      Check(packet.pts == point->decode_start.value_or(point->time), "M4A seek landed at the wrong packet");
    }
  }
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
  const std::array source_ranges{input.PresentationRange(tracks[0].id), input.PresentationRange(tracks[1].id)};
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
      track.presentation_edit.reset();
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
        result.Tracks()[0].pixel_aspect_ratio == tracks[0].pixel_aspect_ratio &&
        result.Tracks()[0].video_extensions == tracks[0].video_extensions &&
        result.Tracks()[0].initialization_data == tracks[0].initialization_data &&
        result.Tracks()[1].codec == tracks[1].codec && result.Tracks()[1].channel_count == tracks[1].channel_count,
        "Remuxed video or audio description differs");
  if (!fragmented) {
    for (std::size_t i = 0; i < tracks.size(); ++i) {
      Check(result.Tracks()[i].presentation_edit == tracks[i].presentation_edit &&
            result.PresentationRange(tracks[i].id) == source_ranges[i],
            "QuickTime remux changed the track edit or effective presentation range");
    }
  }
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
  if (audio_only) {
    for (const auto milliseconds : {500, 4500, 180, 3200}) {
      const auto point = Take(input.Seek(tracks.front().id, {milliseconds, 1000}));
      Check(point && point->decode_start, "Repeated WebM seek lost its preroll anchor");
      Check(std::holds_alternative<MediaTrack>(*Take(input.Read())), "WebM seek lost its description");
      Check(std::get<ContainerPacket>(*Take(input.Read())).pts == point->decode_start,
            "Repeated WebM seek started in the wrong Cluster");
    }
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
                         "avc-aac.mkv", "vp8-opus.webm", "fast-start.mp4"};
  try {
    Check(fs::create_directory(path), "Could not create a fresh container test directory");
    Mp4(path / names[0], ContainerLayout::Default);
    Mp4(path / names[1], ContainerLayout::Fragmented);
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
    Mp4(path / names[17], ContainerLayout::FastStart);
    AudioFormats(path);
    M4a(path);
    EmptyM4aMetadata(path);
    if (argc != 2) {
      for (auto name : names) fs::remove(path / name);
      fs::remove(path / "open-ended.mp4");
      fs::remove(path / "truncated.mp4");
      fs::remove(path / "remux.ogg");
      fs::remove(path / "remux.webm");
      fs::remove(path / "chained.ogg");
      fs::remove(path / "copy.flac");
      fs::remove(path / "copy.mp3");
      fs::remove(path / "copy-aac.m4a");
      fs::remove(path / "copy-alac.m4a");
      fs::remove(path / "empty-udta.m4a");
      fs::remove(path / "empty-ilst.m4a");
      fs::remove(path / "truncated-audio");
      fs::remove(path);
    }
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  return 0;
}
