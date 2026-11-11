#pragma once

#include <cstddef>
#include <optional>
#include <span>

namespace huxerui::media::detail {

using Av1Bytes = std::span<const std::byte>;

struct Av1Sequence {
  unsigned width;
  unsigned height;
  bool reduced_header;
  bool sdr_420;

  bool operator==(const Av1Sequence&) const = default;
};

struct Av1Configuration {
  unsigned profile;
  unsigned level;
  bool high_tier;
  bool eight_bit_420;
  std::optional<Av1Sequence> sequence;
};

struct Av1PacketInfo {
  std::optional<Av1Sequence> sequence;
  std::optional<std::size_t> unsized_obu;
  unsigned pictures = 0;
  bool spatial_layers = false;
  bool sync = false;
};

Av1Configuration ParseAv1Configuration(Av1Bytes bytes);
Av1PacketInfo InspectAv1Packet(Av1Bytes bytes);

} // namespace huxerui::media::detail
