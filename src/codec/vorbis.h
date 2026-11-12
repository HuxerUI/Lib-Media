#pragma once

#include "container/container_io.h"

#include <array>
#include <vorbis/codec.h>

namespace huxerui::media::detail {

std::array<ByteView, 3> VorbisHeaderPackets(ByteView configuration, ContainerErrorCode error);
Bytes PackVorbisHeaders(const std::array<Bytes, 3>& headers);
ogg_packet VorbisPacket(ByteView data, bool beginning = false);

class VorbisHeaders {
public:
  VorbisHeaders(ByteView configuration, ContainerErrorCode error);
  ~VorbisHeaders();
  VorbisHeaders(const VorbisHeaders&) = delete;
  VorbisHeaders& operator=(const VorbisHeaders&) = delete;

  unsigned Rate() const { return static_cast<unsigned>(info_.rate); }
  unsigned Channels() const { return static_cast<unsigned>(info_.channels); }
  unsigned MaxSamples() const;
  unsigned BlockSize(ByteView data, ContainerErrorCode error) const;
  vorbis_info* Info() { return &info_; }

private:
  mutable vorbis_info info_{};
};

constexpr unsigned VorbisOverlap(unsigned previous, unsigned current) {
  return previous ? (previous + current) / 4 : 0;
}

} // namespace huxerui::media::detail
