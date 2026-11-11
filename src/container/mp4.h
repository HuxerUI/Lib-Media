#pragma once

#include "container_io.h"

namespace huxerui::media::detail {

inline constexpr std::uint32_t MaxMp4Descriptions = 32;
inline constexpr std::size_t MaxMp4Edits = 256;

struct Mp4Sample {
  std::uint64_t offset;
  std::uint32_t size;
  std::int64_t pts;
  std::int64_t dts;
  std::uint32_t duration;
  bool sync;
  std::uint32_t description = 1;
};

inline void SetU32(Bytes& bytes, std::size_t position, std::uint32_t value) {
  for (std::size_t i = 0; i < 4; ++i) bytes[position + i] = static_cast<std::byte>((value >> (24 - 8 * i)) & 255);
}

template<class Body> void Box(Bytes& bytes, std::string_view tag, Body body) {
  const auto start = bytes.size();
  Put(bytes, 0u);
  Put(bytes, Tag(tag));
  body();
  if (bytes.size() > MaxContainerMetadata) Fail(ContainerErrorCode::LimitExceeded, "MP4 box exceeds metadata budget");
  SetU32(bytes, start, static_cast<std::uint32_t>(bytes.size() - start));
}

template<class Body> void FullBox(Bytes& bytes, std::string_view tag, std::uint32_t flags, Body body) {
  Box(bytes, tag, [&] { Put(bytes, flags); body(); });
}

std::uint32_t Mp4Scale(const MediaTrack& track);

struct Mp4EditSegment {
  std::int64_t begin, end, media;
};

struct Mp4EditTimeline {
  std::uint32_t scale;
  std::vector<Mp4EditSegment> segments;
  std::int64_t duration = 0;
};

Mp4EditTimeline Mp4Edits(std::span<const MediaTrack::Edit> edits, std::uint32_t movie_scale,
                         std::uint32_t media_scale, ContainerErrorCode error);

struct AlacConfig {
  std::uint32_t frame_length, sample_rate;
  unsigned channels, bits;
};
AlacConfig ParseAlac(ByteView config, ContainerErrorCode error);

struct Mp4AudioTag {
  std::string_view name;
  std::string MediaAudioMetadata::*field;
};
inline constexpr Mp4AudioTag Mp4AudioTags[]{
    {"\xa9" "nam", &MediaAudioMetadata::title},
    {"\xa9" "ART", &MediaAudioMetadata::artist},
    {"\xa9" "alb", &MediaAudioMetadata::album},
    {"\xa9" "gen", &MediaAudioMetadata::genre},
    {"\xa9" "day", &MediaAudioMetadata::date}};

bool Mp4VideoExtensionTag(std::uint32_t tag);
void ValidateMp4VideoExtension(std::uint32_t tag, ByteView payload, CodecId codec, ContainerErrorCode error);
bool AvcIdr(ByteView bytes, unsigned length_size, ContainerErrorCode error);
unsigned HevcNalLength(ByteView config, bool hvc1, ContainerErrorCode error);
bool HevcIdr(ByteView bytes, unsigned length_size, bool hvc1, ContainerErrorCode error);
void ValidateAv1Configuration(ByteView config, ContainerErrorCode error);
bool Av1Sync(ByteView bytes, ContainerErrorCode error);
bool ProResEntry(std::uint32_t tag);
void ValidateMovDataDescription(std::string_view entry, ByteView data, ContainerErrorCode error);
unsigned MovPcmBits(CodecId codec);
bool MovPcmBigEndian(CodecId codec);

} // namespace huxerui::media::detail
