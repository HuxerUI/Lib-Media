#include "framed_audio.h"

#include <array>

namespace huxerui::media::detail {
namespace {

constexpr auto Bad = ContainerErrorCode::InvalidData;

struct FrameHeader {
  unsigned version, rate, channels, samples, length, side_offset, side_size;
};

std::optional<FrameHeader> ParseHeader(ByteView bytes) {
  if (bytes.size() < 4) return {};
  const auto bits = Number<std::uint32_t>(bytes);
  const auto version = (bits >> 19) & 3u;
  const auto bitrate = (bits >> 12) & 15u;
  const auto rate = (bits >> 10) & 3u;
  if ((bits & 0xffe00000u) != 0xffe00000u || version == 1 || ((bits >> 17) & 3u) != 1 ||
      !bitrate || bitrate == 15 || rate == 3 || (bits & 3u) == 2)
    return {};
  constexpr std::array rates{44100u, 48000u, 32000u};
  constexpr std::array high{0u, 32u, 40u, 48u, 56u, 64u, 80u, 96u, 112u, 128u, 160u, 192u, 224u, 256u, 320u};
  constexpr std::array low{0u, 8u, 16u, 24u, 32u, 40u, 48u, 56u, 64u, 80u, 96u, 112u, 128u, 144u, 160u};
  const bool mpeg1 = version == 3;
  const auto sample_rate = rates[rate] / (mpeg1 ? 1 : version == 2 ? 2 : 4);
  const auto channels = ((bits >> 6) & 3u) == 3 ? 1u : 2u;
  const auto side = mpeg1 ? (channels == 1 ? 17u : 32u) : (channels == 1 ? 9u : 17u);
  const auto offset = (bits & 0x10000u) ? 4u : 6u;
  const auto length = (mpeg1 ? 144000u * high[bitrate] : 72000u * low[bitrate]) / sample_rate + ((bits >> 9) & 1u);
  if (length < side + offset) return {};
  return FrameHeader{version, sample_rate, channels, mpeg1 ? 1152u : 576u, length, offset, side};
}

bool TagPrefix(ByteView bytes) {
  return bytes.size() >= 3 && bytes[0] == std::byte{'I'} && bytes[1] == std::byte{'D'} &&
         bytes[2] == std::byte{'3'};
}

void SkipId3(ContainerInput& input) {
  const auto prefix = input.Peek(10);
  if (!TagPrefix(prefix)) return;
  if (prefix.size() != 10) Fail(Bad, "Truncated ID3 header");
  const auto version = Number<std::uint8_t>(prefix, 3);
  if (version < 2 || version > 4) Fail(ContainerErrorCode::Unsupported, "Unsupported ID3 version");
  const auto flags = Number<std::uint8_t>(prefix, 5);
  const auto size = Number<std::uint32_t>(prefix, 6);
  if (size & 0x80808080u) Fail(Bad, "Invalid ID3 synchsafe size");
  const auto count = ((size >> 24) << 21) | (((size >> 16) & 127u) << 14) |
                     (((size >> 8) & 127u) << 7) | (size & 127u);
  if (count > MaxContainerMetadata) Fail(ContainerErrorCode::LimitExceeded, "ID3 metadata exceeds its size limit");
  input.Skip(10 + count + (version == 4 && (flags & 16) ? 10 : 0));
}

struct Mp3Timing {
  std::optional<std::uint64_t> samples;
  std::optional<std::uint64_t> delay;
  std::optional<std::uint64_t> padding;
};

std::optional<Mp3Timing> ReadXing(ByteView frame, const FrameHeader& header) {
  const auto start = header.side_offset + header.side_size;
  if (frame.size() < start + 8) return {};
  auto info = frame.subspan(start);
  const auto marker = Number<std::uint32_t>(info);
  if (marker != Tag("Xing") && marker != Tag("Info")) return {};
  Mp3Timing timing;
  const auto flags = Number<std::uint32_t>(info, 4);
  std::size_t cursor = 8;
  if (flags & 1) {
    const auto frames = Number<std::uint32_t>(info, cursor);
    if (!frames) Fail(Bad, "Xing frame count is zero");
    timing.samples = std::uint64_t(frames) * header.samples;
    cursor += 4;
  }
  if (flags & 2) cursor += 4;
  if (flags & 4) cursor += 100;
  if (flags & 8) cursor += 4;
  if (cursor > info.size()) Fail(Bad, "Truncated Xing information");
  const auto extension = info.subspan(cursor);
  if (extension.size() >= 24) {
    const auto encoder = Number<std::uint32_t>(extension);
    if (encoder == Tag("LAME") || encoder == Tag("Lavc") || encoder == Tag("Lavf")) {
      const auto trim = Number<std::uint32_t>(extension, 20) & 0xffffffu;
      timing.delay = trim >> 12;
      timing.padding = trim & 0xfffu;
    }
  }
  return timing;
}

class Mp3Reader final : public FramedAudioReader {
public:
  explicit Mp3Reader(ContainerInput input) : FramedAudioReader(std::move(input), ContainerFormat::Mp3) {
    SkipId3(this->input);
    const auto first = ParseHeader(this->input.Peek(4));
    if (!first) Fail(Bad, "Missing MPEG Layer III frame");
    info_ = *first;
    const auto bytes = this->input.Peek(info_.length);
    if (bytes.size() != info_.length) Fail(Bad, "Truncated MPEG Layer III frame");
    const auto timing = ReadXing(bytes, info_);
    if (timing) this->input.Skip(info_.length);
    MediaTrack track{.id = 1, .kind = TrackKind::Audio, .codec = CodecId::Mp3, .timescale = info_.rate,
                     .sample_rate = info_.rate, .channel_count = info_.channels,
                     .priming_samples = timing ? timing->delay : std::nullopt,
                     .padding_samples = timing ? timing->padding : std::nullopt};
    Begin(std::move(track), timing ? timing->samples : std::nullopt);
  }

private:
  std::optional<AudioPacket> ReadFrame(std::int64_t) override {
    const auto prefix = input.Peek(4);
    if (prefix.empty()) return {};
    if (prefix.size() == 4 && (Number<std::uint32_t>(prefix) >> 8) == 0x544147u) {
      input.Skip(128);
      if (!input.Peek(1).empty()) Fail(Bad, "Trailing bytes after ID3v1");
      return {};
    }
    const auto header = ParseHeader(prefix);
    if (!header || header->version != info_.version || header->rate != info_.rate ||
        header->channels != info_.channels) Fail(Bad, "Invalid or changing MPEG Layer III frame");
    auto bytes = input.ReadBytes(header->length);
    const auto reservoir = Number<std::uint8_t>(bytes, header->side_offset);
    const auto restartable = reservoir == 0 &&
        (header->version != 3 || !(Number<std::uint8_t>(bytes, header->side_offset + 1) & 128u));
    return AudioPacket{std::move(bytes), header->samples, restartable};
  }
  FrameHeader info_{};
};
} // namespace

bool LooksLikeMp3(ByteView bytes) {
  return TagPrefix(bytes) || ParseHeader(bytes).has_value();
}

std::unique_ptr<ContainerReader> OpenMp3Reader(ContainerInput input) {
  return std::make_unique<Mp3Reader>(std::move(input));
}
} // namespace huxerui::media::detail
