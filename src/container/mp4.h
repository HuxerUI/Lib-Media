#pragma once

#include "container_io.h"

namespace huxerui::media::detail {

struct Mp4Sample {
  std::uint64_t offset;
  std::uint32_t size;
  std::int64_t pts;
  std::int64_t dts;
  std::uint32_t duration;
  bool sync;
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
bool AvcIdr(ByteView bytes, unsigned length_size, ContainerErrorCode error);
unsigned HevcNalLength(ByteView config, bool hvc1, ContainerErrorCode error);
bool HevcIdr(ByteView bytes, unsigned length_size, bool hvc1, ContainerErrorCode error);
bool ProResEntry(std::uint32_t tag);
unsigned MovPcmBits(CodecId codec);
bool MovPcmBigEndian(CodecId codec);

} // namespace huxerui::media::detail
