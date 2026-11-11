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

std::optional<ContainerPacket> ReadPacket(MediaDemuxer& reader) {
  while (auto item = Take(reader.Read())) {
    if (auto* packet = std::get_if<ContainerPacket>(&*item)) return std::move(*packet);
  }
  return {};
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
  track.descriptions.resize(1);
  track.id = audio ? 2 : 1;
  track.kind = audio ? TrackKind::Audio : TrackKind::Video;
  track.descriptions.front().codec = audio ? CodecId::Aac : CodecId::H264;
  track.descriptions.front().framing = audio ? PacketFraming::CodecPacket : PacketFraming::LengthPrefixedNals;
  track.descriptions.front().initialization_data = Take(Path(base / (name + ".config")).ReadBytes());
  if (audio) {
    track.descriptions.front().sample_rate = 48000;
    track.descriptions.front().channel_count = 2;
  } else {
    track.descriptions.front().coded_width = 64;
    track.descriptions.front().coded_height = 48;
    track.descriptions.front().visible = VideoRectangle{2, 2, 60, 44};
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
        actual.sample_description_index == expected.sample_description_index &&
        std::ranges::equal(actual.data.AsBytes(), expected.data.AsBytes()), "Container packet differs after round trip");
}

void Mp4(const fs::path& path, ContainerLayout layout) {
  const bool fragmented = layout == ContainerLayout::Fragmented;
  const bool fast_start = layout == ContainerLayout::FastStart;
  const auto video = Fixture(false, fragmented);
  const auto audio = Fixture(true, fragmented);
  const std::array tracks{video.track, audio.track};
  using Requirement = MediaMuxer::OutputRequirement;
  const auto requirement = Take(MediaMuxer::ValidateOutput(ContainerFormat::Mp4, tracks, {.layout = layout}));
  Check(requirement == (fragmented ? Requirement::SequentialWrite :
                       fast_start ? Requirement::RandomReadWrite : Requirement::RandomWrite),
        "Wrong MP4 output requirement");
  const auto file = Path(path);
  if (fast_start) {
    auto write_only = Output(path);
    auto rejected = MediaMuxer::Open(std::move(write_only), ContainerFormat::Mp4, tracks, {.layout = layout});
    Check(!rejected.Succeeded() && rejected.Error().code == ContainerErrorCode::InvalidArgument &&
          fs::file_size(path) == 0, "Fast-start accepted a write-only destination");
  }
  auto mux = fragmented
      ? Take(MediaMuxer::Open(Take(file.OpenWrite()), ContainerFormat::Mp4, tracks, {.layout = layout}))
      : Take(MediaMuxer::Open(fast_start ? Take(file.OpenRandomAccess(FileAccessMode::ReadWrite)) : Output(path),
                             ContainerFormat::Mp4, tracks, {.layout = layout}));

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
    while (ReadPacket(sequential)) ++count;
    Check(count == video.packets.size() + audio.packets.size(), "Fast-start MP4 cannot be read sequentially");
  }
  Check(demux.Tracks()[0].descriptions.front().visible == video.track.descriptions.front().visible, "MP4 clean aperture was not retained");
  if (!fragmented) {
    const auto& first = audio.packets.front();
    const auto& last = audio.packets.back();
    const MediaPresentationRange expected{{first.pts->ticks + 1024, 48000},
        {last.pts->ticks + last.duration->ticks - 288, 48000}};
    Check(demux.Tracks()[1].edits == std::vector<MediaTrack::Edit>{
          {{expected.end.ticks - expected.start.ticks, 48000}, MediaTimestamp{1024, 48000}}} && demux.PresentationRange(audio.track.id) == expected,
          "MP4 AAC presentation range includes trimmed packets");
  }
  const auto media_packet = [fragmented](const TrackFixture& input, std::size_t index) {
    auto packet = input.packets[index];
    if (!fragmented) {
      const auto origin = input.packets.front().dts->ticks;
      packet.pts->ticks -= origin;
      packet.dts->ticks -= origin;
    }
    return packet;
  };
  cursor = {};
  std::optional<ContainerPacket> retained;
  std::size_t last_packet_size = 0;
  while (auto item = ReadPacket(demux)) {
    const auto& packet = *item;
    const auto index = packet.track_id - 1;
    Check(index < inputs.size() && cursor[index] < inputs[index]->packets.size(), "Unexpected MP4 packet");
    Compare(packet, media_packet(*inputs[index], cursor[index]++));
    last_packet_size = packet.data.AsBytes().size();
    if (!retained && index == 0) retained = packet;
  }
  Check(cursor[0] == video.packets.size() && cursor[1] == audio.packets.size(), "MP4 packets were lost");
  if (!fragmented) {
    Check(!Take(demux.Read()), "Find or EOF changed the read cursor");
    auto point = Take(demux.FindSeekPoint(1, {1, 24}));
    Check(point && point->presentation_time == *video.packets[0].pts && point->kind == SeekPoint::Kind::Independent,
          "MP4 seek point is not the expected IDR");
    Check(!Take(demux.Read()), "Index query consumed or repositioned input");
    Check(Take(demux.Seek(1, {0, 1}))->byte_offset == point->byte_offset, "MP4 seek chose a different anchor");
    const std::array selection{1u};
    Take(demux.SelectTracks(selection));
    Check(std::get<TrackDecodeStart>(*Take(demux.Read())).track_id == 1, "Seek did not replay the track configuration");
    Compare(*ReadPacket(demux), media_packet(video, 0));
  } else {
    auto index = demux.FindSeekPoint(1, {1, 1});
    Check(!index.Succeeded() && index.Error().code == ContainerErrorCode::IndexUnavailable,
          "Fragmented input reported an index it does not have");
  }
  Take(demux.Close());
  Compare(*retained, media_packet(video, 0));

  if (fragmented) {
    auto indexed = Take(MediaDemuxer::Open(Take(file.OpenRandomAccess())));
    Check(!indexed.PresentationRange(video.track.id), "Unindexed fragments reported sample coverage");
    const auto requirement = Take(MediaMuxer::ValidateOutput(ContainerFormat::Mp4, indexed.Tracks(), {.layout = layout}));
    const std::array selected{video.track.id};
    Take(indexed.SelectTracks(selected));
    Compare(*ReadPacket(indexed), video.packets[0]);
    const auto& second_idr = video.packets[video.packets.size() / 2];
    const auto future = Take(indexed.FindSeekPoint(video.track.id, *video.packets[1].pts, SeekDirection::AtOrAfter));
    Check(future && future->presentation_time == *second_idr.pts && future->kind == SeekPoint::Kind::Independent,
          "Fragment index did not find the next independent sample");
    Compare(*ReadPacket(indexed), video.packets[1]);
    Take(indexed.FindSeekPoint(video.track.id, {INT64_MAX, 1}));
    const auto video_range = indexed.PresentationRange(video.track.id);
    Check(indexed.Duration() && video_range && indexed.PresentationRange(audio.track.id) &&
          !indexed.PresentationRange(0),
          "Fragment index did not establish presentation coverage");
    Check(indexed.Tracks()[0].edits.empty() && indexed.Tracks()[1].edits.empty() &&
          Take(MediaMuxer::ValidateOutput(ContainerFormat::Mp4, indexed.Tracks(), {.layout = layout})) == requirement,
          "Building an index made unedited tracks incompatible with fragmented output");
    auto edited = indexed.Tracks()[0];
    const auto start = video_range->start.Rescale(*edited.timescale)->ticks;
    const auto end = video_range->end.Rescale(*edited.timescale)->ticks;
    edited.edits = {{{start, *edited.timescale}, {}}, {{end - start, *edited.timescale}, video_range->start}};
    Check(Take(MediaMuxer::ValidateOutput(ContainerFormat::Mp4, std::span(&edited, 1), {.layout = layout})) == requirement,
          "Fragmented output rejected a known presentation edit");
    for (const auto* expected : {&second_idr, &video.packets[0], &second_idr}) {
      const auto point = Take(indexed.Seek(video.track.id, *expected->pts));
      Check(point && point->presentation_time == *expected->pts && point->kind == SeekPoint::Kind::Independent,
            "Fragment seek changed its anchor");
      Check(std::get<TrackDecodeStart>(*Take(indexed.Read())).track_id == video.track.id &&
            indexed.PresentationRange(video.track.id) == video_range, "Fragment seek changed the track or its coverage");
      Compare(*ReadPacket(indexed), *expected);
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
    while (ReadPacket(stream)) ++packets;
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
    const auto failed = damaged_index.FindSeekPoint(video.track.id, {INT64_MAX, 1});
    Check(!failed.Succeeded() && failed.Error().code == ContainerErrorCode::InvalidData,
          "Truncated fragments produced a seek index");
    Take(damaged_index.SelectTracks(selected));
    Compare(*ReadPacket(damaged_index), video.packets.front());
  }
}

void WriteMp4(const File& file, std::span<const MediaTrack> descriptions, std::span<const ContainerPacket> packets,
              ContainerLayout layout) {
  auto mux = layout == ContainerLayout::Fragmented
      ? Take(MediaMuxer::Open(Take(file.OpenWrite()), ContainerFormat::Mp4, descriptions, {.layout = layout}))
      : Take(MediaMuxer::Open(Take(file.OpenRandomAccess(FileAccessMode::ReadWrite, FileOpenMode::CreateNew)),
                              ContainerFormat::Mp4, descriptions, {.layout = layout}));
  for (const auto& packet : packets) Check(!Take(mux.TryWrite(packet)), "Single-track MP4 write blocked");
  Take(mux.Finish());
}

void DescriptionsAndEdits(const fs::path& path, ContainerLayout layout) {
  using media::detail::Number;
  using media::detail::Put;
  auto video = Fixture(false, true);
  const auto group = video.packets.size() / 2;
  const auto scale = *video.track.timescale;
  const auto origin = video.packets.front().dts->ticks;
  for (auto& packet : video.packets) {
    packet.dts->ticks -= origin;
    packet.pts->ticks -= origin;
  }
  const auto first = *video.packets.front().pts;
  const auto second = *video.packets[group].pts;
  const auto length = video.packets.front().duration->ticks;
  video.track.edits = {{{length, scale}, {}}, {{length, scale}, first},
                       {{length, scale}, {}}, {{2 * length, scale}, second}, {{2 * length, scale}, {}}};
  auto alternate = video.track.descriptions.front();
  alternate.visible = VideoRectangle{4, 2, 56, 44};
  alternate.initialization_data[4] = (alternate.initialization_data[4] & std::byte{0xfc}) | std::byte{1};
  for (auto& packet : std::span(video.packets).subspan(group)) {
    packet.sample_description_index = 2;
    auto nals = packet.data.AsBytes();
    Bytes bytes;
    while (!nals.empty()) {
      const auto size = Number<std::uint32_t>(nals);
      Check(size && size <= UINT16_MAX && size <= nals.size() - 4, "Invalid AVC test packet");
      Put(bytes, static_cast<std::uint16_t>(size));
      media::detail::Append(bytes, nals.subspan(4, size));
      nals = nals.subspan(4 + size);
    }
    packet.data = BytesRef(std::move(bytes));
  }
  video.track.descriptions.push_back(alternate);
  const auto file = Path(path);
  WriteMp4(file, std::span(&video.track, 1), video.packets, layout);
  auto reader = Take(MediaDemuxer::Open(Take(file.OpenRandomAccess())));
  const auto known = reader.Tracks().front().descriptions;
  Check(known.size() == 2 && reader.Tracks().front().edits == video.track.edits && known[1].visible == alternate.visible &&
        known[1].initialization_data == alternate.initialization_data, "MP4 changed its sample descriptions or edits");
  std::vector<MediaPresentationSlice> slices;
  Take(MapToPresentation(video.track.edits, first, {second.ticks + 2 * length, scale}, slices));
  Check(slices.size() == 2 && slices[0].media_start == first && slices[1].media_start == second &&
        slices[0].presentation == MediaPresentationRange{{length, scale}, {2 * length, scale}} &&
        slices[1].presentation == MediaPresentationRange{{3 * length, scale}, {5 * length, scale}},
        "MP4 edit mapping lost a gap or retained media interval");
  Take(MapToPresentation(video.track.edits, {first.ticks + length, scale}, second, slices));
  Check(slices.empty(), "MP4 edit mapping retained excluded media");
  const auto later = Take(reader.Seek(video.track.id, {3 * length, scale}));
  Check(later && later->presentation_time == MediaTimestamp{3 * length, scale} && later->decode_start_pts == second && later->kind == SeekPoint::Kind::Independent,
        "Seek did not map the second edit to its independent sample");
  Check(std::get<TrackDecodeStart>(*Take(reader.Read())).sample_description_index == 2,
        "Seek did not replay the new description");
  Compare(*ReadPacket(reader), video.packets[group]);
  const auto previous = Take(reader.Seek(video.track.id, {length, scale}));
  Check(previous && previous->presentation_time == MediaTimestamp{length, scale} && previous->decode_start_pts == first, "Seek did not map the first edit");
  Check(std::get<TrackDecodeStart>(*Take(reader.Read())).sample_description_index == 1,
        "Backward seek did not restore the original description");
  for (std::size_t i = 0; i < video.packets.size(); ++i) {
    if (i == group) {
      const auto updated = std::get<TrackDecodeStart>(*Take(reader.Read()));
      Check(updated.sample_description_index == 2 && known[updated.sample_description_index - 1].initialization_data == alternate.initialization_data,
            "MP4 omitted the description event before the new packets");
    }
    Compare(*ReadPacket(reader), video.packets[i]);
  }
  Check(!Take(reader.Read()), "MP4 returned extra packets across the edits");
  Check(reader.Duration() == MediaTimestamp{7 * length, scale} &&
        reader.PresentationRange(video.track.id) == MediaPresentationRange{{length, scale}, {5 * length, scale}},
        "The trailing empty edit changed the sample bounds or disappeared from the movie duration");
  if (layout == ContainerLayout::Fragmented) {
    auto streamed = Take(MediaDemuxer::Open(Take(file.OpenRandomAccess())));
    while (Take(streamed.Read())) {}
    Check(streamed.Duration() == reader.Duration() &&
          streamed.PresentationRange(video.track.id) == reader.PresentationRange(video.track.id),
          "Streaming and indexed reads disagree about the trailing edit");
  }
}

void TimelineMapping() {
  const std::array<MediaTrack::Edit, 1> edits{{{{9000000003, 3000000001u}, MediaTimestamp{0, 4000000000u}}}};
  std::vector<MediaPresentationSlice> slices;
  Take(MapToPresentation(edits, {2000000000, 2000000000u}, {6000000002, 3000000001u}, slices));
  Check(slices.size() == 1 && slices[0].media_start == MediaTimestamp{1, 1} &&
        slices[0].presentation == MediaPresentationRange{{1, 1}, {2, 1}},
        "Equivalent rational clocks changed the presentation mapping");
}

void AacEditSeek(const fs::path& path, ContainerLayout layout) {
  auto audio = Fixture(true, false);
  constexpr std::uint32_t scale = 48000;
  constexpr std::int64_t second = 3000 * 1024;
  audio.track.edits = {{{1, 1}, MediaTimestamp{1024, scale}}, {{1, 1}, {}},
                       {{1, 1}, MediaTimestamp{second, scale}}};
  std::vector<ContainerPacket> packets(3048, audio.packets.front());
  for (std::size_t i = 0; i < packets.size(); ++i)
    packets[i].pts = packets[i].dts = MediaTimestamp{static_cast<std::int64_t>(i) * 1024, scale};
  const auto file = Path(path);
  WriteMp4(file, std::span(&audio.track, 1), packets, layout);
  auto reader = Take(MediaDemuxer::Open(Take(file.OpenRandomAccess())));
  const auto id = audio.track.id;
  for (const auto [time, direction] : {std::pair{MediaTimestamp{2, 1}, SeekDirection::AtOrBefore},
                                       {MediaTimestamp{3, 2}, SeekDirection::AtOrAfter}}) {
    const auto anchor = Take(reader.FindSeekPoint(id, time, direction));
    Check(anchor && anchor->presentation_time == MediaTimestamp{2, 1} &&
          anchor->decode_start_pts == MediaTimestamp{second - 1024, scale} &&
          anchor->kind == SeekPoint::Kind::NeedsPreroll, "AAC seek did not choose preroll in the second media interval");
    const auto point = Take(reader.Seek(id, time, direction));
    Check(point && point->decode_start_pts == anchor->decode_start_pts &&
          std::get<TrackDecodeStart>(*Take(reader.Read())).track_id == id, "AAC seek did not replay its decode start");
    Check(ReadPacket(reader)->pts == anchor->decode_start_pts &&
          ReadPacket(reader)->pts == MediaTimestamp{second, scale}, "AAC seek read packets from the wrong edit");
  }
  const auto first = Take(reader.Seek(id, {0, 1}));
  Check(first && first->presentation_time == MediaTimestamp{0, 1} &&
        first->decode_start_pts == MediaTimestamp{0, scale}, "AAC backward seek did not restore the initial preroll");
}

void FragmentedAacTrim(const fs::path& path) {
  auto audio = Fixture(true, true);
  const auto scale = *audio.track.descriptions.front().sample_rate;
  audio.track.priming_samples = 128;
  audio.track.padding_samples = 173;
  const auto start = audio.packets.front().pts->ticks + 128;
  const auto end = audio.packets.back().pts->ticks + audio.packets.back().duration->ticks - 173;
  const MediaPresentationRange range{{start, scale}, {end, scale}};
  audio.track.edits = {{{start, scale}, {}}, {{end - start, scale}, MediaTimestamp{start, scale}}};
  const auto file = Path(path);
  WriteMp4(file, std::span(&audio.track, 1), audio.packets, ContainerLayout::Fragmented);
  auto reader = Take(MediaDemuxer::Open(Take(file.OpenRandomAccess())));
  const auto id = audio.track.id;
  Take(reader.FindSeekPoint(id, {INT64_MAX, 1}));
  Check(reader.PresentationRange(id) == range && reader.Tracks().front().edits == audio.track.edits,
        "Fragmented AAC lost its exact priming or tail trim");
  for (const auto& expected : audio.packets) Compare(*ReadPacket(reader), expected);
  const auto copy = Path(path.parent_path() / "trim-readback.m4a");
  WriteMp4(copy, reader.Tracks(), audio.packets, ContainerLayout::Default);
  auto readback = Take(MediaDemuxer::Open(Take(copy.OpenRandomAccess())));
  Check(readback.PresentationRange(id) == range && readback.Tracks().front().edits.size() == 2 &&
        readback.Tracks().front().edits.back().media_start == MediaTimestamp{128, scale},
        "M4A copy lost the fragmented edit");
}

void AppendFile(const fs::path& path, std::span<const std::byte> data) {
  std::ofstream file(path, std::ios::binary | std::ios::app);
  file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
  Check(bool(file), "Could not append a fragment test file");
}

void FragmentSeekAndSharedMedia(const fs::path& path) {
  using namespace media::detail;
  auto fixture = Fixture(false, false);
  fixture.track.timescale = 1;
  const auto id = fixture.track.id;
  constexpr std::array order{2, 0, 1};
  std::array<ContainerPacket, order.size()> packets;
  for (std::size_t i = 0; i < order.size(); ++i) {
    packets[i] = fixture.packets.front();
    packets[i].pts = MediaTimestamp{order[i], 1};
    packets[i].dts = MediaTimestamp{static_cast<std::int64_t>(i), 1};
    packets[i].duration = MediaTimestamp{1, 1};
  }
  const auto file = Path(path / "reordered-fragments.mp4");
  WriteMp4(file, std::span(&fixture.track, 1), packets, ContainerLayout::Fragmented);
  for (const auto direction : {SeekDirection::AtOrBefore, SeekDirection::AtOrAfter}) {
    auto reader = Take(MediaDemuxer::Open(Take(file.OpenRandomAccess())));
    const auto target = *packets.back().pts;
    const auto point = Take(reader.FindSeekPoint(id, target, direction));
    Check(point && point->presentation_time == target, "A later fragment's reordered sync picture was skipped");
    const auto anchor = Take(reader.Seek(id, target, direction));
    Check(anchor && anchor->byte_offset == point->byte_offset &&
          std::holds_alternative<TrackDecodeStart>(*Take(reader.Read())), "The seek lost its reordered sync picture");
    Compare(*ReadPacket(reader), packets.back());
  }

  // Place two consecutive moofs before a single mdat and adjust their relative run offsets.
  const auto bytes = Take(file.ReadBytes());
  const ByteView data(bytes);
  const auto skip = [&](std::size_t at) { return at + Number<std::uint32_t>(data, at); };
  const auto first = skip(skip(0)), first_media = skip(first), second = skip(first_media);
  const auto second_media = skip(second), third = skip(second_media);
  const auto patch_run = [](Bytes& moof, std::uint32_t offset) {
    const auto tag = std::as_bytes(std::span("trun", 4));
    const auto run = std::ranges::search(moof, tag);
    Check(!run.empty(), "Missing shared-media test run");
    SetU32(moof, static_cast<std::size_t>(run.begin() - moof.begin()) + 12, offset);
  };
  Bytes first_moof(data.begin() + first, data.begin() + first_media);
  Bytes second_moof(data.begin() + second, data.begin() + second_media);
  const auto first_bytes = data.subspan(first_media + 8, second - first_media - 8);
  const auto second_bytes = data.subspan(second_media + 8, third - second_media - 8);
  patch_run(first_moof, static_cast<std::uint32_t>(first_moof.size() + second_moof.size() + 8));
  patch_run(second_moof, static_cast<std::uint32_t>(second_moof.size() + first_bytes.size() + 8));
  Bytes shared(data.begin(), data.begin() + first);
  Append(shared, first_moof);
  Append(shared, second_moof);
  Box(shared, "mdat", [&] {
    Append(shared, first_bytes);
    Append(shared, second_bytes);
  });
  const auto complete = shared.size();
  Append(shared, data.subspan(third));

  const auto combined = Path(path / "shared-mdat.mp4");
  Check(combined.WriteBytes(shared), "Could not stage shared media data");
  auto reader = Take(MediaDemuxer::Open(Take(combined.OpenRandomAccess())));
  for (const auto& packet : packets) Compare(*ReadPacket(reader), packet);
  Check(!Take(reader.Read()), "Reading shared media produced extra packets");
  const auto point = Take(reader.Seek(id, *packets[1].pts));
  Check(point && point->presentation_time == *packets[1].pts && std::holds_alternative<TrackDecodeStart>(*Take(reader.Read())),
        "The fragment index did not include the second run in shared media");
  Compare(*ReadPacket(reader), packets[1]);

  const auto growing = path / "shared-mdat-growing.mp4";
  AppendFile(growing, ByteView(shared).first(complete - 1));
  auto live = Take(MediaDemuxer::Open(Take(Path(growing).OpenRandomAccess()), {}, ContainerInputMode::Growing));
  Check(!Take(live.Read()), "A partial shared mdat exposed packets");
  AppendFile(growing, ByteView(shared).subspan(complete - 1));
  for (const auto& packet : packets) Compare(*ReadPacket(live), packet);
}

void GrowingAndIndexedMp4(const fs::path& path) {
  using namespace media::detail;
  auto fixture = Fixture(false, false);
  const auto scale = *fixture.track.timescale;
  const auto id = fixture.track.id;
  auto first = fixture.packets.front();
  first.pts = first.dts = MediaTimestamp{0, scale};
  auto second = first;
  second.pts = second.dts = *first.duration;
  const std::array packets{first, second};
  fixture.track.edits = {{{2 * first.duration->ticks, scale}, MediaTimestamp{0, scale}}};
  const auto file = Path(path / "index-source.mp4");
  WriteMp4(file, std::span(&fixture.track, 1), packets, ContainerLayout::Fragmented);
  const auto bytes = Take(file.ReadBytes());
  const ByteView data(bytes);
  const auto skip = [&](std::size_t at) { return at + Number<std::uint32_t>(data, at); };
  const auto init = skip(skip(0)), boundary = skip(skip(init));
  const auto growing = path / "growing.mp4";
  AppendFile(growing, data.first(boundary + 3));
  auto reader = Take(MediaDemuxer::Open(Take(Path(growing).OpenRandomAccess()), {}, ContainerInputMode::Growing));
  Compare(*ReadPacket(reader), first);
  Check(!Take(reader.Read()) && !Take(reader.FindSeekPoint(id, *second.pts, SeekDirection::AtOrAfter)),
        "A partial fragment became visible");
  AppendFile(growing, data.subspan(boundary + 3, data.size() - boundary - 4));
  Check(!Take(reader.Read()), "A fragment with a partial mdat became visible");
  AppendFile(growing, data.last(1));
  Check(Take(reader.FindSeekPoint(id, *second.pts, SeekDirection::AtOrAfter))->presentation_time == *second.pts,
        "The index did not extend after append");
  Compare(*ReadPacket(reader), second);
  Check(Take(reader.Seek(id, *first.pts))->presentation_time == *first.pts, "Growing input lost its earlier index");
  Check(std::holds_alternative<TrackDecodeStart>(*Take(reader.Read())), "Growing seek did not replay the description");
  Compare(*ReadPacket(reader), first);

  Bytes index;
  FullBox(index, "sidx", 0, [&] {
    Put(index, id);
    Put(index, scale);
    Put(index, 0u);
    Put(index, 0u);
    Put<std::uint16_t>(index, 0);
    Put<std::uint16_t>(index, 2);
    for (auto size : {boundary - init, data.size() - boundary}) {
      Put(index, static_cast<std::uint32_t>(size));
      Put(index, static_cast<std::uint32_t>(first.duration->ticks));
      Put(index, 0x90000000u);
    }
  });
  const auto embedded = path / "embedded-index.mp4";
  AppendFile(embedded, data.first(init));
  AppendFile(embedded, index);
  auto broken = Bytes(data.begin() + init, data.begin() + boundary);
  const auto tfhd = std::ranges::search(broken, std::array{std::byte{'t'}, std::byte{'f'}, std::byte{'h'}, std::byte{'d'}});
  Check(!tfhd.empty(), "Missing test fragment header");
  SetU32(broken, static_cast<std::size_t>(tfhd.begin() - broken.begin()) + 8, 999);
  AppendFile(embedded, broken);
  AppendFile(embedded, data.subspan(boundary));
  auto direct = Take(MediaDemuxer::Open(Take(Path(embedded).OpenRandomAccess())));
  Check(Take(direct.Seek(id, *second.pts))->presentation_time == *second.pts, "Valid embedded index did not jump to its fragment");
  Check(std::holds_alternative<TrackDecodeStart>(*Take(direct.Read())), "Embedded seek did not replay the description");
  Compare(*ReadPacket(direct), second);

#if !defined(_WIN32)
  const auto large = path / "sparse.mp4";
  AppendFile(large, data.first(init));
  constexpr std::uint64_t free_size = 0x100000000ull;
  Bytes free_header;
  Put(free_header, 1u);
  Put(free_header, Tag("free"));
  Put(free_header, free_size);
  AppendFile(large, free_header);
  fs::resize_file(large, init + free_size);
  AppendFile(large, data.subspan(init));
  auto sparse = Take(MediaDemuxer::Open(Take(Path(large).OpenRandomAccess())));
  const auto point = Take(sparse.Seek(id, *second.pts));
  Check(point && point->byte_offset && *point->byte_offset > UINT32_MAX, "Sparse MP4 lost its 64-bit seek offset");
  Check(std::holds_alternative<TrackDecodeStart>(*Take(sparse.Read())), "Sparse seek did not replay the description");
  Compare(*ReadPacket(sparse), second);
#endif
}

void Wave(const fs::path& path, ContainerFormat format) {
  MediaTrack track;
  track.descriptions.resize(1);
  track.id = 1;
  track.kind = TrackKind::Audio;
  track.descriptions.front().codec = format == ContainerFormat::Rf64 ? CodecId::PcmS24Le : CodecId::PcmU8;
  track.descriptions.front().sample_rate = 48000;
  track.descriptions.front().channel_count = format == ContainerFormat::Rf64 ? 6 : 1;
  const std::array tracks{track};
  const auto frame_bytes = format == ContainerFormat::Rf64 ? 18 : 1;
  Bytes bytes(3 * frame_bytes);
  for (std::size_t i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<std::byte>(i);
  auto mux = Take(MediaMuxer::Open(Output(path), format, tracks));
  auto retained = BytesRef(bytes);
  Check(!Take(mux.TryWrite({1, retained, {}, {}, {}, {}})), "WAVE packet was not accepted");
  Take(mux.Finish());

  auto sequential = Take(MediaDemuxer::Open(Take(Path(path).OpenRead())));
  Check(sequential.Format() == format && sequential.Tracks()[0].descriptions.front().codec == track.descriptions.front().codec &&
        sequential.Tracks()[0].descriptions.front().channel_count == track.descriptions.front().channel_count && sequential.Duration() == MediaTimestamp{3, 48000},
        "WAVE metadata differs after round trip");
  auto packet = *ReadPacket(sequential);
  Check(std::ranges::equal(packet.data.AsBytes(), bytes), "WAVE data differs after round trip");
  Check(!Take(sequential.Read()) && !Take(sequential.Read()), "WAVE EOF or alignment pad is incorrect");

  auto demux = Take(MediaDemuxer::Open(Take(Path(path).OpenRandomAccess())));
  const auto point = Take(demux.Seek(1, {3, 96000}, SeekDirection::AtOrAfter));
  Check(point && point->presentation_time == MediaTimestamp{2, 48000} && point->kind == SeekPoint::Kind::Independent,
        "WAVE seek did not land on the expected sample");
  Check(std::get<TrackDecodeStart>(*Take(demux.Read())).track_id == 1, "WAVE seek did not replay track description");
  packet = *ReadPacket(demux);
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
  Check(demux.Format() == ContainerFormat::Adts && demux.Tracks()[0].descriptions.front().initialization_data == audio.track.descriptions.front().initialization_data,
        "Wrong ADTS configuration");
  for (const auto& packet : audio.packets) Compare(*ReadPacket(demux), packet);
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
          reader.Tracks().front().descriptions.front().codec == sample.codec, "Wrong native audio format or codec");
    unsigned count = 0;
    std::optional<ContainerPacket> last;
    while (auto item = ReadPacket(reader)) {
      last = std::move(item);
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
    Check(anchor && anchor->presentation_time.Compare({1, 1}) <= 0, "Native audio seek has no covered anchor");
    Check(ReadPacket(random).has_value(), "Native audio query consumed the first packet");
    for (auto target : {MediaTimestamp{1, 2}, MediaTimestamp{1, 5}, MediaTimestamp{1, 1}}) {
      const auto found = Take(random.Seek(1, target));
      Check(found && found->presentation_time.Compare(target) <= 0, "Repeated native audio seek has no anchor");
      Check(std::holds_alternative<TrackDecodeStart>(*Take(random.Read())), "Native audio seek lost its track description");
      const auto packet = *ReadPacket(random);
      Check(packet.pts == found->decode_start_pts, "Native audio seek used the wrong decode start");
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
  while (auto item = ReadPacket(webm)) {
    auto packet = *item;
    Check(!Take(ogg.TryWrite(packet)), "Single-track Ogg requested more input tracks");
    expected.push_back(std::move(packet));
  }
  Take(ogg.Finish());
  auto reopened = Take(MediaDemuxer::Open(Take(target.OpenRead())));
  Check(reopened.Tracks().front().descriptions.front().initialization_data == webm.Tracks().front().descriptions.front().initialization_data,
        "OpusHead changed during remux");
  const auto reverse = Path(directory / "remux.webm");
  auto back = Take(MediaMuxer::Open(Take(reverse.OpenWrite()), ContainerFormat::WebM, reopened.Tracks()));
  std::size_t index = 0;
  while (auto item = ReadPacket(reopened)) {
    const auto packet = *item;
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
  while (auto item = ReadPacket(roundtrip)) {
    const auto packet = *item;
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
  const auto second_offset = links.size();
  links.insert(links.end(), next_link.begin(), next_link.end());
  const auto third_offset = links.size();
  const auto third = Path(fixtures / "opus-link.ogg");
  const auto last_link = Take(third.ReadBytes());
  links.insert(links.end(), last_link.begin(), last_link.end());
  const auto chained = Path(directory / "chained.ogg");
  Check(chained.WriteBytes(links), "Could not write chained Ogg input");
  const auto boundary = MediaTimestamp{107376, 48000};
  const auto second_duration = reopened.Duration()->Rescale(48000)->ticks;
  const MediaTimestamp last_boundary{boundary.ticks + second_duration, 48000};
  auto last = Take(MediaDemuxer::Open(Take(third.OpenRandomAccess())));
  Take(last.FindSeekPoint(1, {0, 1}));
  const auto last_duration = last.Duration()->Rescale(48000)->ticks;
  const auto first_config = Take(MediaDemuxer::Open(Take(chained.OpenRead()))).Tracks().front().descriptions.front().initialization_data;
  const auto second_config = reopened.Tracks().front().descriptions.front().initialization_data;
  for (const bool random : {false, true}) {
    auto reader = random ? Take(MediaDemuxer::Open(Take(chained.OpenRandomAccess())))
                         : Take(MediaDemuxer::Open(Take(chained.OpenRead())));
    const std::array starts{MediaTimestamp{0, 48000}, boundary, last_boundary};
    std::size_t link = 0;
    while (auto item = Take(reader.Read())) {
      const auto* event = std::get_if<TrackDecodeStart>(&*item);
      if (!event) continue;
      Check(link < starts.size(), "Chained Ogg emitted an extra decode start");
      const auto start = starts[link];
      const auto index = link == 1 ? 2u : 1u;
      const auto description = reader.Tracks().front().descriptions.at(index - 1);
      const auto pre_skip = (std::to_integer<unsigned>(description.initialization_data[10]) |
                             std::to_integer<unsigned>(description.initialization_data[11]) << 8);
      Check(event->track_id == 1 && event->sample_description_index == index && event->first_audible_pts == start &&
            description.initialization_data == (link == 1 ? second_config : first_config),
            "Chained Ogg lost the absolute start or stable configuration of a link");
      const auto first = std::get<ContainerPacket>(*Take(reader.Read()));
      Check(first.pts == MediaTimestamp{start.ticks - pre_skip, 48000}, "Ogg link lost its raw first packet time");
      ++link;
    }
    Check(link == starts.size() && reader.Tracks().front().descriptions.size() == 2 &&
          reader.Duration() == MediaTimestamp{last_boundary.ticks + last_duration, 48000},
          "Chained Ogg did not expose all links on one presentation timeline");
  }
  auto indexed = Take(MediaDemuxer::Open(Take(chained.OpenRandomAccess())));
  const auto next = Take(indexed.FindSeekPoint(1, boundary));
  Check(next && next->byte_offset >= second_offset && next->byte_offset < third_offset &&
        indexed.Tracks().front().descriptions.front().initialization_data == first_config,
        "Ogg link index query changed the original description");
  Check(ReadPacket(indexed).has_value(), "Ogg index query moved the read cursor");
  for (const auto start : {MediaTimestamp{0, 48000}, boundary, last_boundary}) {
    for (const auto direction : {SeekDirection::AtOrBefore, SeekDirection::AtOrAfter}) {
      const auto point = Take(indexed.FindSeekPoint(1, start, direction));
      Check(point && point->presentation_time == start && point->decode_start_pts.Compare(start) < 0,
            "Ogg seek confused a link's audible start with its pre-skip packets");
    }
  }
  for (auto target : {boundary, MediaTimestamp{1, 1}, last_boundary, MediaTimestamp{3, 1}, boundary}) {
    const auto point = Take(indexed.Seek(1, target));
    const bool final_link = target.Compare(last_boundary) >= 0;
    const bool second = !final_link && target.Compare(boundary) >= 0;
    const auto offset = final_link ? third_offset : second ? second_offset : 0;
    const auto limit = final_link ? links.size() : second ? third_offset : second_offset;
    Check(point && point->byte_offset >= offset && point->byte_offset < limit, "Ogg seek landed in the wrong link");
    const auto start = std::get<TrackDecodeStart>(*Take(indexed.Read()));
    const auto description = indexed.Tracks().front().descriptions.at(start.sample_description_index - 1);
    const auto packet = std::get<ContainerPacket>(*Take(indexed.Read()));
    Check(start.first_audible_pts == (final_link ? last_boundary : second ? boundary : MediaTimestamp{0, 48000}) &&
          description.initialization_data == (second ? second_config : first_config) &&
          packet.pts == point->decode_start_pts, "Ogg seek replayed the wrong link or preroll");
  }

  for (const auto format : {ContainerFormat::Flac, ContainerFormat::Mp3}) {
    const bool flac = format == ContainerFormat::Flac;
    const auto input = Path(fixtures / (flac ? "tagged-flac.flac" : "tagged-mp3.mp3"));
    const auto copy = Path(directory / (flac ? "copy.flac" : "copy.mp3"));
    auto audio = Take(MediaDemuxer::Open(Take(input.OpenRandomAccess())));
    const auto track = audio.Tracks().front();
    const auto& tags = audio.AudioMetadata();
    Check(tags && tags->title == "试听 Café" && tags->artist == "Lib-Media" && tags->album == "Audio Fixtures" &&
          tags->track_number == "2/12" && tags->cover && tags->cover->mime_type == "image/png",
          "Audio tags or front cover were not exposed");
    Check(Take(MediaMuxer::ValidateOutput(format, audio.Tracks())) == MediaMuxer::OutputRequirement::RandomWrite,
          "Native audio copy must request random writes");
    auto mux = Take(MediaMuxer::Open(Output(directory / (flac ? "copy.flac" : "copy.mp3")), format, audio.Tracks(),
        {.audio_metadata = tags}));
    std::vector<ContainerPacket> packets;
    while (auto item = ReadPacket(audio)) {
      packets.push_back(std::move(*item));
      Check(!Take(mux.TryWrite(packets.back())), "Single-track audio output requested another packet");
    }
    Take(mux.Finish());
    auto readback = Take(MediaDemuxer::Open(Take(copy.OpenRandomAccess())));
    const auto& metadata = readback.AudioMetadata();
    Check(readback.Duration() == audio.Duration() && metadata && metadata->title == tags->title &&
          metadata->artist == tags->artist && metadata->album == tags->album && metadata->cover &&
          std::ranges::equal(metadata->cover->data.AsBytes(), tags->cover->data.AsBytes()), "Audio copy lost metadata or trim");
    for (const auto& packet : packets) Compare(*ReadPacket(readback), packet);
    Check(!Take(readback.Read()), "Audio copy added an access unit");
    for (const auto target : {MediaTimestamp{1, 1}, MediaTimestamp{1, 5}, MediaTimestamp{4, 5}}) {
      const auto point = Take(readback.Seek(1, target));
      Check(point && point->presentation_time.Compare(target) <= 0 && std::holds_alternative<TrackDecodeStart>(*Take(readback.Read())),
            "Audio copy could not seek to a covered packet");
      Check(ReadPacket(readback)->pts == point->decode_start_pts,
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
    Check(track.descriptions.front().codec == CodecId::Aac && !reader.AudioMetadata() &&
          reader.PresentationRange(track.id) == MediaPresentationRange{{0, 1}, {52800, 48000}},
          "Empty optional M4A metadata affected the audio track");
    unsigned count = 0;
    while (ReadPacket(reader)) ++count;
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
        track.descriptions.front().codec == codec && track.descriptions.front().sample_rate == rate && track.descriptions.front().channel_count == 2 &&
        reader.PresentationRange(track.id) == range, "Wrong M4A coding or effective presentation range");
    Check(!aac || (!track.edits.empty() && track.edits.back().media_start == MediaTimestamp{1024, rate}),
          "Wrong M4A encoder priming");
    const auto& tags = reader.AudioMetadata();
    Check(tags && tags->title == "试听 Café" && tags->artist == "Lib-Media" && tags->album == "Audio Fixtures" &&
        tags->track_number == "2/12" && tags->genre == "Test" && tags->date == "2026" &&
        tags->cover && tags->cover->mime_type == "image/png", "Wrong M4A tags or cover");
    auto mux = Take(MediaMuxer::Open(Take(output.OpenRandomAccess(FileAccessMode::ReadWrite, FileOpenMode::CreateNew)),
        ContainerFormat::Mp4, reader.Tracks(), {.layout = ContainerLayout::FastStart, .audio_metadata = tags}));
    std::vector<ContainerPacket> packets;
    while (auto item = ReadPacket(reader)) {
      packets.push_back(std::move(*item));
      Check(!Take(mux.TryWrite(packets.back())), "M4A writer did not accept the packet");
    }
    Check(packets.size() == (aac ? 53 : 3) && packets.back().duration == MediaTimestamp{aac ? 576 : 2368, rate},
        "M4A final packet lost its actual duration");
    Take(mux.Finish());
    auto sequential = Take(MediaDemuxer::Open(Take(output.OpenRead()), ContainerFormat::Mp4));
    const auto copy = sequential.Tracks().front();
    const auto& metadata = sequential.AudioMetadata();
    Check(copy.descriptions.front().codec == codec && copy.descriptions.front().initialization_data == track.descriptions.front().initialization_data &&
        copy.edits == track.edits &&
        sequential.PresentationRange(copy.id) == range, "M4A copy lost format or gapless trim");
    Check(metadata && metadata->title == tags->title && metadata->artist == tags->artist &&
        metadata->album == tags->album && metadata->track_number == tags->track_number &&
        metadata->genre == tags->genre && metadata->date == tags->date && metadata->cover &&
        metadata->cover->mime_type == tags->cover->mime_type &&
        std::ranges::equal(metadata->cover->data.AsBytes(), tags->cover->data.AsBytes()), "M4A copy lost tags or cover bytes");
    for (const auto& packet : packets) Compare(*ReadPacket(sequential), packet);
    Check(!Take(sequential.Read()), "M4A copy added packets");
    auto random = Take(MediaDemuxer::Open(Take(output.OpenRandomAccess())));
    for (const auto ticks : {samples / 2, std::int64_t(0), samples - 1}) {
      const auto point = Take(random.Seek(copy.id, {ticks, rate}));
      Check(point && std::holds_alternative<TrackDecodeStart>(*Take(random.Read())), "M4A copy lost its seek index");
      const auto packet = *ReadPacket(random);
      Check(packet.pts == point->decode_start_pts, "M4A seek landed at the wrong packet");
    }
    if (aac) {
      auto multi = track;
      multi.descriptions.push_back(track.descriptions.front());
      for (auto& packet : std::span(packets).subspan(1)) packet.sample_description_index = 2;
      const auto switched = Path(directory / "tagged-descriptions.m4a");
      auto writer = Take(MediaMuxer::Open(Output(directory / "tagged-descriptions.m4a"), ContainerFormat::Mp4,
          std::span(&multi, 1), {.audio_metadata = tags}));
      for (const auto& packet : packets) Check(!Take(writer.TryWrite(packet)), "M4A configuration write blocked");
      Take(writer.Finish());
      auto tagged = Take(MediaDemuxer::Open(Take(switched.OpenRandomAccess())));
      Check(tagged.Tracks().front().descriptions.size() == 2 && tagged.AudioMetadata() &&
            tagged.AudioMetadata()->title == tags->title, "M4A configuration table lost its file tags");
      Compare(*ReadPacket(tagged), packets.front());
      const auto event = std::get<TrackDecodeStart>(*Take(tagged.Read()));
      Check(event.track_id == track.id && event.sample_description_index == 2 && tagged.AudioMetadata()->cover &&
            std::ranges::equal(tagged.AudioMetadata()->cover->data.AsBytes(), tags->cover->data.AsBytes()),
            "M4A description event changed the file tags");
      for (const auto& packet : std::span(packets).subspan(1))
        Compare(*ReadPacket(tagged), packet);
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
  Check(input.Format() == ContainerFormat::Mov && input.Tracks().size() == (hevc ? 2 : 3), "QuickTime brand or track count differs");

  std::vector<MediaTrack> tracks(input.Tracks().begin(), input.Tracks().end());
  Check(tracks[0].descriptions.front().codec == (hevc ? CodecId::Hevc : CodecId::ProRes) &&
        tracks[0].descriptions.front().sample_entry == (hevc ? "hvc1" : "apcn") &&
        tracks[0].rotation == (hevc ? VideoRotation::Clockwise90 : VideoRotation::None),
        "QuickTime video description differs");
  Check(tracks[1].descriptions.front().codec == (hevc ? CodecId::Aac : CodecId::PcmS24Le), "QuickTime audio coding differs");
  if (!hevc) {
    Check(tracks[2].kind == TrackKind::Data && tracks[2].descriptions.front().sample_entry == "tmcd" &&
          tracks[0].references == std::vector<MediaTrack::Reference>{{"tmcd", tracks[2].id}},
          "QuickTime timecode or its association is missing");
  }
  const auto seek = Take(input.FindSeekPoint(tracks[0].id, {0, 1}));
  Check(seek && seek->kind == SeekPoint::Kind::Independent && seek->presentation_time == MediaTimestamp{0, 1},
        "QuickTime video did not expose an independent first frame");
  std::vector<std::vector<ContainerPacket>> packets(tracks.size());
  std::size_t pcm_bytes = 0;
  while (auto item = ReadPacket(input)) {
    auto packet = *item;
    const auto track = media::detail::PacketTrack(tracks, packet.track_id);
    if (!hevc && track == 1) {
      const auto size = packet.data.AsBytes().size();
      Check(size % 6 == 0 && size > 6 && size <= 16384, "QuickTime PCM did not group aligned frames");
      pcm_bytes += size;
    }
    packets[track].push_back(std::move(packet));
  }
  Check(packets[0].size() == (hevc ? 6 : 4) && !packets[1].empty(), "Wrong QuickTime fixture packet count");
  if (!hevc) {
    Check(pcm_bytes == 4000 * 6, "QuickTime PCM lost sample frames");
    Check(packets[2].size() == 1 && packets[2][0].data.AsBytes().size() == 4, "Wrong QuickTime timecode sample");
  }
  std::vector<std::optional<MediaPresentationRange>> source_ranges;
  for (const auto& track : tracks) source_ranges.push_back(input.PresentationRange(track.id));
  Take(input.Close());

  if (in_band) {
    tracks[0].descriptions.front().sample_entry = "hev1";
    // Repeat the configuration's VPS/SPS/PPS in the first access unit as permitted by hev1.
    const auto& config = tracks[0].descriptions.front().initialization_data;
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
      track.edits.clear();
    }
    for (auto& group : packets) {
      for (auto& packet : group) {
        packet.pts->ticks += packet.pts->timescale;
        packet.dts->ticks += packet.dts->timescale;
      }
    }
  }
  auto output = fragmented
      ? Take(MediaMuxer::Open(Take(Path(destination).OpenWrite()), format, tracks, {.layout = layout}))
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
  std::vector<std::size_t> cursor(tracks.size());
  std::size_t total = 0;
  for (const auto& group : packets) total += group.size();
  std::size_t track = 0;
  for (std::size_t remaining = total, attempts = 0; remaining;) {
    Check(++attempts <= 2 * total, "QuickTime remux did not make progress");
    const auto need = Take(output.TryWrite(packets[track][cursor[track]]));
    if (need) {
      track = media::detail::PacketTrack(tracks, *need);
      Check(cursor[track] < packets[track].size(), "QuickTime remux requested an ended track");
    } else {
      --remaining;
      if (++cursor[track] == packets[track].size()) {
        Take(output.EndTrack(tracks[track].id));
        if (remaining) {
          do { track = (track + 1) % tracks.size(); } while (cursor[track] == packets[track].size());
        }
      }
    }
  }
  Take(output.Finish());

  auto result = fragmented
      ? Take(MediaDemuxer::Open(Take(Path(destination).OpenRead()), format))
      : Take(MediaDemuxer::Open(Take(Path(destination).OpenRandomAccess()), format));
  Check(result.Format() == format && result.Layout() == layout && result.Tracks().size() == tracks.size() &&
        result.Tracks()[0].descriptions.front().sample_entry == tracks[0].descriptions.front().sample_entry &&
        result.Tracks()[0].rotation == tracks[0].rotation &&
        result.Tracks()[0].descriptions.front().pixel_aspect_ratio == tracks[0].descriptions.front().pixel_aspect_ratio &&
        result.Tracks()[0].descriptions.front().video_extensions == tracks[0].descriptions.front().video_extensions &&
        result.Tracks()[0].descriptions.front().initialization_data == tracks[0].descriptions.front().initialization_data &&
        result.Tracks()[1].descriptions.front().codec == tracks[1].descriptions.front().codec && result.Tracks()[1].descriptions.front().channel_count == tracks[1].descriptions.front().channel_count,
        "Remuxed video or audio description differs");
  if (!fragmented) {
    for (std::size_t i = 0; i < tracks.size(); ++i) {
      Check(result.Tracks()[i].edits == tracks[i].edits &&
            result.Tracks()[i].references == tracks[i].references &&
            result.Tracks()[i].descriptions.front().initialization_data == tracks[i].descriptions.front().initialization_data &&
            result.PresentationRange(tracks[i].id) == source_ranges[i],
            "QuickTime remux changed the track edit or effective presentation range");
    }
  }
  std::ranges::fill(cursor, 0);
  while (auto item = ReadPacket(result)) {
    const auto& packet = *item;
    const auto index = media::detail::PacketTrack(tracks, packet.track_id);
    Check(cursor[index] < packets[index].size(), "Unexpected remuxed packet");
    Compare(packet, packets[index][cursor[index]++]);
  }
  for (std::size_t i = 0; i < tracks.size(); ++i)
    Check(cursor[i] == packets[i].size(), "QuickTime remux lost packets");
  if (fragmented) return;
  const auto point = Take(result.Seek(tracks[0].id, {0, 1}));
  Check(point && point->kind == (in_band ? SeekPoint::Kind::SyncMarked : SeekPoint::Kind::Independent),
        "Remuxed video seek evidence differs");
  Check(std::get<TrackDecodeStart>(*Take(result.Read())).track_id == tracks[0].id &&
        result.Tracks().front().rotation == tracks[0].rotation,
        "Seek did not deliver the remuxed rotation");
  Compare(*ReadPacket(result), packets[0].front());
}

void QuickTimeMetadata(const fs::path& path) {
  const auto fixture = Path(fs::path(MEDIA_FIXTURES) / "container" / "preview-hevc-metadata.mov");
  auto source = Take(MediaDemuxer::Open(Take(fixture.OpenRandomAccess()), ContainerFormat::Mov));
  const std::vector tracks(source.Tracks().begin(), source.Tracks().end());
  Check(tracks.size() == 2 && tracks[0].descriptions.front().codec == CodecId::Hevc && tracks[0].rotation == VideoRotation::Clockwise90 &&
        tracks[1].kind == TrackKind::Data && tracks[1].descriptions.front().sample_entry == "mebx" &&
        tracks[1].references == std::vector<MediaTrack::Reference>{{"cdsc", tracks[0].id}},
        "AVAssetWriter timed metadata description differs");
  const auto unsupported = MediaMuxer::ValidateOutput(ContainerFormat::Mp4, tracks);
  Check(!unsupported.Succeeded() && unsupported.Error().code == ContainerErrorCode::Unsupported,
        "MP4 output silently dropped a QuickTime track association");
  Take(source.SelectTracks(std::span(&tracks[1].id, 1)));
  const auto metadata = *ReadPacket(source);
  Check(!Take(source.Read()) && metadata.data.AsBytes().size() == 10, "Unexpected timed metadata packet");

  auto mux = Take(MediaMuxer::Open(Output(path), ContainerFormat::Mov, tracks));
  Check(!Take(mux.TryWrite(metadata)), "Timed metadata write did not accept the sample");
  Take(mux.EndTrack(tracks[1].id));
  auto video = Take(MediaDemuxer::Open(Take(fixture.OpenRandomAccess())));
  Take(video.SelectTracks(std::span(&tracks[0].id, 1)));
  std::vector<ContainerPacket> expected;
  while (auto item = Take(video.Read())) {
    if (const auto* packet = std::get_if<ContainerPacket>(&*item)) {
      Check(!Take(mux.TryWrite(*packet)), "QuickTime video write blocked");
      expected.push_back(*packet);
    }
  }
  Take(mux.Finish());
  auto copied = Take(MediaDemuxer::Open(Take(Path(path).OpenRandomAccess())));
  Check(copied.Tracks().size() == 2 && copied.Tracks()[0].rotation == tracks[0].rotation &&
        copied.Tracks()[0].descriptions.front().video_extensions == tracks[0].descriptions.front().video_extensions &&
        copied.Tracks()[1].references == tracks[1].references &&
        copied.Tracks()[1].descriptions.front().initialization_data == tracks[1].descriptions.front().initialization_data,
        "MOV copy changed timed metadata or the video description");
  std::size_t pictures = 0, metadata_samples = 0;
  while (auto item = ReadPacket(copied)) {
    const auto& packet = *item;
    if (packet.track_id == tracks[1].id) {
      Compare(packet, metadata);
      ++metadata_samples;
    } else {
      Compare(packet, expected.at(pictures++));
    }
  }
  Check(pictures == expected.size() && metadata_samples == 1, "MOV copy lost media or metadata samples");
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
  Check(tracks.front().descriptions.front().codec == (audio_only ? CodecId::Opus : video_codec) &&
        tracks.back().descriptions.front().codec == (webm ? CodecId::Opus : CodecId::Aac), "Wrong Matroska or WebM codec mapping");
  if (video_codec == CodecId::Vp8) {
    Check(tracks.front().descriptions.front().initialization_data.empty(), "WebM VP8 unexpectedly reported codec private data");
    auto invalid = tracks;
    invalid.front().descriptions.front().initialization_data.push_back(std::byte{0});
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
    Check(external_point->kind == SeekPoint::Kind::NeedsPreroll &&
          external_point->decode_start_pts.Compare(external_point->presentation_time) < 0, "Opus Cues did not resolve earlier preroll");
  }

  std::vector<std::vector<ContainerPacket>> packets(tracks.size());
  bool positive_padding = false;
  while (auto item = ReadPacket(input)) {
    auto packet = *item;
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
      Check(point.has_value(), "Repeated WebM seek lost its preroll anchor");
      Check(std::holds_alternative<TrackDecodeStart>(*Take(input.Read())), "WebM seek lost its description");
      Check(ReadPacket(input)->pts == point->decode_start_pts,
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
        readback.Tracks().front().descriptions.front().codec == tracks.front().descriptions.front().codec &&
        readback.Tracks().front().descriptions.front().initialization_data == tracks.front().descriptions.front().initialization_data,
        "Matroska or WebM output lost a track or changed its codec mapping");
  std::fill(cursor.begin(), cursor.end(), 0);
  while (auto item = ReadPacket(readback)) {
    const auto& packet = *item;
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
  Take(seekable.SelectTracks(std::span(&tracks.front().id, 1)));
  const auto point = Take(seekable.Seek(tracks.front().id, time));
  Check(point && point->byte_offset, "Tail Cues did not resolve a Block");
  const auto expected = point->decode_start_pts;
  Check(!audio_only || (point->kind == SeekPoint::Kind::NeedsPreroll && expected.Compare(point->presentation_time) < 0),
        "Matroska Seek did not preserve the decode start");
  Check(std::get<TrackDecodeStart>(*Take(seekable.Read())).track_id == tracks.front().id, "Matroska Seek did not replay track description");
  Check(ReadPacket(seekable)->pts == expected, "Matroska Seek landed at the wrong Block");
}

void WebmWithoutCues() {
  const auto file = Path(fs::path(MEDIA_FIXTURES) / "container" / "preview-vp9-opus-no-cues.webm");
  const auto bytes = Take(file.ReadBytes());
  constexpr std::array cues{std::byte{0x1c}, std::byte{0x53}, std::byte{0xbb}, std::byte{0x6b}};
  Check(std::ranges::search(bytes, cues).empty(), "The WebM scan fixture contains Cues");
  auto sequential = Take(MediaDemuxer::Open(Take(file.OpenRead())));
  const auto tracks = sequential.Tracks();
  const auto video = tracks[0].id, audio = tracks[1].id;
  Check(tracks[0].descriptions.front().codec == CodecId::Vp9 && tracks[1].descriptions.front().codec == CodecId::Opus,
        "The WebM scan fixture lost VP9 or Opus");
  const auto unavailable = sequential.FindSeekPoint(video, {1, 1});
  Check(!unavailable.Succeeded() && unavailable.Error().code == ContainerErrorCode::IndexUnavailable,
        "Sequential WebM unexpectedly has a random-access index");
  const auto first_start = std::get<TrackDecodeStart>(*Take(sequential.Read()));
  const auto first_packet = std::get<ContainerPacket>(*Take(sequential.Read()));
  Check(first_start.track_id == audio && first_start.first_audible_pts == MediaTimestamp{0, 1},
        "WebM did not report the original audible start");
  std::vector<ContainerPacket> packets{first_packet};
  while (auto packet = ReadPacket(sequential)) packets.push_back(std::move(*packet));
  const auto tail = std::find_if(packets.rbegin(), packets.rend(), [&](const auto& packet) { return packet.track_id == audio; });
  Check(tail != packets.rend() && tail->duration == MediaTimestamp{20, 1000} &&
        tail->discard_padding == MediaTimestamp{648, 48000}, "WebM lost coded duration or applied the Opus tail trim twice");

  auto random = Take(MediaDemuxer::Open(Take(file.OpenRandomAccess())));
  Check(std::get<TrackDecodeStart>(*Take(random.Read())).track_id == audio, "WebM lost its initial Opus decode start");
  const auto forward = Take(random.FindSeekPoint(video, {4, 5}, SeekDirection::AtOrAfter));
  Check(forward && forward->presentation_time.Compare({4, 5}) >= 0, "WebM scan did not find a later sync point");
  Compare(std::get<ContainerPacket>(*Take(random.Read())), first_packet);
  for (const auto time : {1100, 180, 780, 1400}) {
    const MediaTimestamp target{time, 1000};
    const auto point = Take(random.Seek(video, target));
    Check(point && point->kind == SeekPoint::Kind::SyncMarked && point->presentation_time.Compare(target) <= 0 &&
          point->presentation_time == point->decode_start_pts, "WebM scan missed the requested video anchor");
    std::optional<MediaTimestamp> first_video, first_audio;
    while (!(first_video && first_audio)) {
      const auto item = Take(random.Read());
      Check(item.has_value(), "WebM seek did not start both selected tracks");
      const auto* start = std::get_if<TrackDecodeStart>(&*item);
      if (!start) continue;
      const auto packet = std::get<ContainerPacket>(*Take(random.Read()));
      Check(packet.track_id == start->track_id && packet.sample_description_index == start->sample_description_index,
            "WebM decode start did not precede its packet");
      const auto original = std::ranges::find_if(packets, [&](const auto& expected) {
        return expected.track_id == packet.track_id && expected.pts == packet.pts;
      });
      Check(original != packets.end(), "WebM seek did not return an original packet");
      Compare(packet, *original);
      if (start->track_id == video) {
        first_video = packet.pts;
        Check(packet.sync_sample == true, "WebM seek started video on a dependent frame");
      } else {
        Check(!first_video && start->first_audible_pts == first_start.first_audible_pts,
              "WebM seek lost the earlier audio or changed its absolute audible start");
        first_audio = packet.pts;
      }
    }
    const auto anchor = point->decode_start_pts.Rescale(1000000000)->ticks;
    Check(first_video == point->decode_start_pts &&
          (*first_audio == *first_packet.pts || first_audio->Compare({anchor - 80000000, 1000000000}) <= 0),
          "WebM seek did not supply the video anchor and Opus preroll");
  }
  Take(random.SelectTracks(std::span(&audio, 1)));
  for (const auto direction : {SeekDirection::AtOrBefore, SeekDirection::AtOrAfter}) {
    const auto anchor = Take(random.Seek(audio, {0, 1}, direction));
    Check(anchor && anchor->presentation_time == MediaTimestamp{0, 1} && anchor->decode_start_pts == first_packet.pts,
          "WebM Opus seek confused the audible start with its codec delay");
    const auto start = std::get<TrackDecodeStart>(*Take(random.Read()));
    Check(start.first_audible_pts == MediaTimestamp{0, 1} &&
          std::get<ContainerPacket>(*Take(random.Read())).pts == first_packet.pts,
          "WebM Opus seek lost the first packet or its audible start");
  }
}

void WebmCuePreroll(const fs::path& path) {
  using namespace media::detail;
  using namespace media::detail::ebml;
  const auto source = Path(fs::path(MEDIA_FIXTURES) / "container" / "preview-vp9-opus-no-cues.webm");
  auto reader = Take(MediaDemuxer::Open(Take(source.OpenRead())));
  const auto opus = reader.Tracks()[1].descriptions.front().initialization_data;
  const auto audio = *ReadPacket(reader), video = *ReadPacket(reader);
  Check(audio.track_id == 2 && video.track_id == 1, "Missing WebM test packets");
  auto block = [](Bytes& cluster, const ContainerPacket& packet, std::int16_t relative) {
    Bytes body;
    PutVint(body, packet.track_id);
    Put(body, relative);
    Put<std::uint8_t>(body, 0x80);
    Append(body, packet.data.AsBytes());
    PutElement(cluster, SimpleBlock, body);
  };
  Bytes segment;
  Master(segment, Info, [](Bytes&) {});
  Master(segment, Tracks, [&](Bytes& entries) {
    Master(entries, 0xae, [](Bytes& track) {
      PutUInt(track, 0xd7, 1);
      PutUInt(track, 0x83, 1);
      PutText(track, 0x86, "V_VP9");
      Master(track, 0xe0, [](Bytes& video) {
        PutUInt(video, 0xb0, 320);
        PutUInt(video, 0xba, 240);
      });
    });
    Master(entries, 0xae, [&](Bytes& track) {
      PutUInt(track, 0xd7, 2);
      PutUInt(track, 0x83, 2);
      PutText(track, 0x86, "A_OPUS");
      PutElement(track, 0x63a2, opus);
      PutUInt(track, 0x56aa, 6500000);
      PutUInt(track, 0x56bb, OpusSeekPreroll);
      Master(track, 0xe1, [](Bytes& audio) {
        PutFloat(audio, 0xb5, 48000);
        PutUInt(audio, 0x9f, 1);
      });
    });
  });
  Master(segment, Cluster, [&](Bytes& cluster) {
    PutUInt(cluster, Timestamp, 0);
    block(cluster, audio, 0);
    block(cluster, video, 0);
  });
  Master(segment, Cluster, [](Bytes& cluster) {
    PutUInt(cluster, Timestamp, 100);
    // A prefix scan would hit the Cluster element budget before reaching the indexed media.
    for (int i = 0; i < 65536; ++i) PutElement(cluster, Void, {});
  });
  const auto previous = segment.size();
  Master(segment, Cluster, [&](Bytes& cluster) {
    PutUInt(cluster, Timestamp, 800);
    block(cluster, video, 0);
    for (std::int16_t offset = 20; offset < 200; offset += 20) block(cluster, audio, offset);
  });
  const auto target = segment.size();
  Master(segment, Cluster, [&](Bytes& cluster) {
    PutUInt(cluster, Timestamp, 1000);
    block(cluster, video, 0);
    block(cluster, audio, 20);
  });
  const auto seek_head = [](std::uint64_t offset) {
    Bytes head;
    Master(head, SeekHead, [&](Bytes& payload) {
      Master(payload, 0x4dbb, [&](Bytes& entry) {
        PutUInt(entry, 0x53ab, Cues);
        Bytes position;
        Put(position, offset);
        PutElement(entry, 0x53ac, position);
      });
    });
    return head;
  };
  const auto head_size = seek_head(0).size();
  const auto head = seek_head(head_size + segment.size());
  Master(segment, Cues, [&](Bytes& entries) {
    for (const auto [time, offset] : {std::pair{800, previous}, {1000, target}}) {
      Master(entries, 0xbb, [&](Bytes& cue) {
        PutUInt(cue, 0xb3, time);
        Master(cue, 0xb7, [&](Bytes& position) {
          PutUInt(position, 0xf7, 1);
          PutUInt(position, 0xf1, head_size + offset);
        });
      });
    }
  });
  Bytes bytes;
  Master(bytes, Header, [](Bytes& header) { PutText(header, 0x4282, "webm"); });
  PutId(bytes, Segment);
  Put<std::uint8_t>(bytes, 0xff);
  Append(bytes, head);
  Append(bytes, segment);
  const auto file = Path(path);
  Check(file.WriteBytes(bytes), "Could not write the WebM Cue fixture");
  auto indexed = Take(MediaDemuxer::Open(Take(file.OpenRandomAccess())));
  const auto point = Take(indexed.Seek(1, {1, 1}));
  Check(point && point->presentation_time == MediaTimestamp{1, 1}, "WebM seek missed the video Cue");
  const auto audio_start = std::get<TrackDecodeStart>(*Take(indexed.Read()));
  Check(audio_start.track_id == 2 && audio_start.first_audible_pts == MediaTimestamp{0, 1} &&
        ReadPacket(indexed)->pts == MediaTimestamp{913500000, Nanoseconds}, "WebM seek missed Opus preroll near the Cue");
  const auto picture = Take(indexed.FindSeekPoint(2, {970, 1000}));
  Check(picture && picture->presentation_time == MediaTimestamp{953500000, Nanoseconds} &&
        picture->decode_start_pts == MediaTimestamp{873500000, Nanoseconds}, "WebM audio reference seek missed the nearby Cue");
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
      PutElement(track, 0x63a2, audio.track.descriptions.front().initialization_data);
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
  Check(std::get<TrackDecodeStart>(*Take(input.Read())).track_id == 1, "Matroska lost its first decode start");
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
  const auto trimmed = *ReadPacket(input);
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
    WebmWithoutCues();
    WebmCuePreroll(path / "webm-cue-preroll.webm");
    AudioFormats(path);
    M4a(path);
    EmptyM4aMetadata(path);
    DescriptionsAndEdits(path / "descriptions.mp4", ContainerLayout::Default);
    DescriptionsAndEdits(path / "descriptions-fragmented.mp4", ContainerLayout::Fragmented);
    TimelineMapping();
    AacEditSeek(path / "aac-edit-seek.m4a", ContainerLayout::Default);
    AacEditSeek(path / "aac-edit-seek-fragmented.m4a", ContainerLayout::Fragmented);
    FragmentedAacTrim(path / "trim-fragmented.mp4");
    FragmentSeekAndSharedMedia(path);
    GrowingAndIndexedMp4(path);
    QuickTimeMetadata(path / "timed-metadata-copy.mov");
    if (argc != 2) fs::remove_all(path);
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  return 0;
}
