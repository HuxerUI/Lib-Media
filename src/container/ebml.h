#pragma once

#include "container_io.h"

namespace huxerui::media::detail::ebml {

inline constexpr std::uint32_t Header = 0x1a45dfa3;
inline constexpr std::uint32_t Segment = 0x18538067;
inline constexpr std::uint32_t SeekHead = 0x114d9b74;
inline constexpr std::uint32_t Info = 0x1549a966;
inline constexpr std::uint32_t Tracks = 0x1654ae6b;
inline constexpr std::uint32_t Cluster = 0x1f43b675;
inline constexpr std::uint32_t Cues = 0x1c53bb6b;
inline constexpr std::uint32_t Void = 0xec;
inline constexpr std::uint32_t Timestamp = 0xe7;
inline constexpr std::uint32_t SimpleBlock = 0xa3;
inline constexpr std::uint32_t BlockGroup = 0xa0;
inline constexpr std::uint32_t Block = 0xa1;
inline constexpr std::uint32_t BlockDuration = 0x9b;
inline constexpr std::uint32_t ReferenceBlock = 0xfb;
inline constexpr std::uint32_t DiscardPadding = 0x75a2;

inline constexpr std::uint32_t Nanoseconds = 1000000000;
inline constexpr std::uint32_t DefaultTimestampScale = 1000000;
inline constexpr std::int64_t OpusSeekPreroll = 80000000;
inline constexpr auto BadData = ContainerErrorCode::InvalidData;
inline constexpr auto BadArgument = ContainerErrorCode::InvalidArgument;
inline constexpr auto Unsupported = ContainerErrorCode::Unsupported;

struct Vint {
  std::uint64_t value;
  unsigned width;
  bool unknown;
};

struct Element {
  std::uint32_t id;
  std::uint64_t position;
  std::uint64_t data;
  std::optional<std::uint64_t> end;
};

struct Field {
  std::uint32_t id;
  ByteView data;
};

Vint ReadVint(ByteView& bytes, bool id = false);
std::optional<Element> ReadElement(ContainerInput& input, std::optional<std::uint64_t> end = {});
Field ReadField(ByteView& bytes);
void SkipElement(ContainerInput& input, const Element& element);
Bytes ReadBody(ContainerInput& input, const Element& element, std::size_t limit = MaxContainerMetadata);
bool SegmentChild(std::uint32_t id);
std::uint64_t UInt(ByteView data);
std::int64_t SInt(ByteView data);
double Float(ByteView data);
std::string_view Text(ByteView data);
std::int64_t SignedTime(std::uint64_t value);
std::int64_t ScaleTime(std::int64_t ticks, std::uint64_t scale);
std::int64_t ToNs(MediaTimestamp time);
std::optional<MediaTimestamp> CodecDuration(const MediaTrack& track, ByteView packet, ContainerErrorCode error);

void PutVint(Bytes& out, std::uint64_t value);
void PutId(Bytes& out, std::uint32_t id);
void PutElement(Bytes& out, std::uint32_t id, ByteView bytes);
void PutUInt(Bytes& out, std::uint32_t id, std::uint64_t value);
void PutInt(Bytes& out, std::uint32_t id, std::int64_t value);
void PutFloat(Bytes& out, std::uint32_t id, double value);
void PutText(Bytes& out, std::uint32_t id, std::string_view text);

template<class Body> void Master(Bytes& out, std::uint32_t id, Body body) {
  Bytes payload;
  body(payload);
  PutElement(out, id, payload);
}

} // namespace huxerui::media::detail::ebml
