#pragma once

#include "import.h"

namespace huxerui::media::detail {

struct AnnexBNal {
  ByteView bytes;
  std::size_t offset;
};
std::vector<AnnexBNal> SplitAnnexB(ByteView bytes);

class AnnexBTrack {
public:
  explicit AnnexBTrack(CodecId codec) : codec_(codec) {}
  bool StartsPicture(ByteView nal) const;
  bool Prefix(ByteView nal) const;
  std::optional<ContainerPacket> Convert(std::span<const AnnexBNal> nals, std::uint32_t track);
  const MediaSampleDescription& Description() const { return description_; }
  bool IsReordered(const ContainerPacket& packet) const;

private:
  unsigned Type(ByteView nal) const;
  bool Picture(unsigned type) const;
  bool Parameter(unsigned type) const;
  CodecId codec_;
  std::vector<Bytes> parameters_;
  MediaSampleDescription description_;
};

} // namespace huxerui::media::detail
