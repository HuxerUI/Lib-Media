#pragma once

#include "container_io.h"

namespace huxerui::media::detail {

CodecId SignedPcmCodec(unsigned bits, bool big_endian, bool floating_point = false);
MediaTrack PcmTrack(CodecId codec, std::uint32_t rate, unsigned channels);
void ValidatePcmTrack(const MediaTrack& track);
unsigned PcmPacketFrames(const ContainerPacket& packet, unsigned alignment, std::int64_t start, unsigned rate);

class PcmReader : public ContainerReader {
public:
  PcmReader(ContainerInput input, ContainerFormat format, MediaTrack track, std::uint64_t bytes, unsigned alignment);

  std::optional<ContainerPacket> Read() override;
  std::optional<SeekPoint> Find(std::uint32_t id, MediaTimestamp time, SeekDirection direction, bool seek) override;
  std::optional<MediaPresentationRange> PresentationRange(std::size_t) const override;

protected:
  PcmReader(ContainerInput input, ContainerFormat format) : ContainerReader(std::move(input), format) {}
  void Begin(MediaTrack track, std::uint64_t bytes, unsigned alignment, unsigned padding = 0);

private:
  unsigned alignment_ = 0;
  std::uint32_t rate_ = 0;
  std::uint64_t offset_ = 0;
  std::int64_t frames_ = 0;
  std::int64_t cursor_ = 0;
  unsigned padding_ = 0;
};

} // namespace huxerui::media::detail
