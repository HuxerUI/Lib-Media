#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace huxerui::media::detail {

using Av1Bytes = std::span<const std::byte>;

struct Av1Sequence {
  unsigned width;
  unsigned height;
  bool reduced_header;
  bool sdr_420;
  unsigned bit_depth, level;
  bool high_tier;

  bool operator==(const Av1Sequence&) const = default;
};

struct Av1Configuration {
  unsigned profile;
  unsigned level;
  bool high_tier;
  bool main_420;
  unsigned bit_depth;
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
std::vector<std::byte> SizedAv1Packet(Av1Bytes bytes, std::size_t offset);
std::vector<std::byte> MakeAv1Configuration(Av1Bytes sequence_header);

} // namespace huxerui::media::detail
