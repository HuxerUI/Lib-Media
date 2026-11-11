#include "container_io.h"

#include "codec/common.h"

#include <array>

namespace huxerui::media::detail {
namespace {

MediaSampleDescription DescribeAdts(std::span<const std::byte> header) {
  const auto first = Number<std::uint16_t>(header);
  if ((first & 0xfff6) != 0xfff0) Fail(ContainerErrorCode::InvalidData, "Invalid ADTS synchronization or layer");
  if ((first & 8) || (Number<std::uint8_t>(header, 6) & 3))
    Fail(ContainerErrorCode::Unsupported, "ADTS requires MPEG-4 with one raw block per frame");
  const auto coding = Number<std::uint16_t>(header, 2);
  const auto object = ((coding >> 14) & 3) + 1;
  const auto rate_index = (coding >> 10) & 15;
  const auto channels = (coding >> 6) & 7;
  MediaSampleDescription description;
  Put<std::uint16_t>(description.initialization_data,
      static_cast<std::uint16_t>((object << 11) | (rate_index << 7) | (channels << 3)));
  auto config = ParseAac(description.initialization_data);
  if (!config.Succeeded()) Fail(ContainerErrorCode::Unsupported, config.Error().message);
  description.codec = CodecId::Aac;
  description.sample_rate = config.Value().sample_rate;
  description.channel_count = config.Value().channels;
  return description;
}

class AdtsReader final : public ContainerReader {
public:
  explicit AdtsReader(ContainerInput input) : ContainerReader(std::move(input), ContainerFormat::Adts) {
    auto description = DescribeAdts(this->input.Peek(7));
    start = next_ = MediaTimestamp{0, *description.sample_rate};
    tracks.push_back({.id = 1, .kind = TrackKind::Audio, .timescale = next_.timescale,
                      .descriptions = {std::move(description)}});
  }

  std::optional<ContainerPacket> Read() override {
    std::array<std::byte, 7> header;
    if (!input.ReadExact(header)) {
      duration = next_;
      return {};
    }
    auto description = DescribeAdts(header);
    const auto size = (Number<std::uint32_t>(header, 2) >> 5) & 0x1fff;
    const auto crc = Number<std::uint8_t>(header, 1) & 1 ? 0u : 2u;
    if (size <= header.size() + crc) Fail(ContainerErrorCode::InvalidData, "ADTS frame has no payload");
    input.Skip(crc);
    auto bytes = Own(input.ReadBytes(size - header.size() - crc));

    // Maintain one exact timeline even when a later frame changes the sample clock.
    const auto rate = *description.sample_rate;
    const auto scale = CommonScale(next_.timescale, rate);
    const auto length = MediaTimestamp{1024, rate};
    const auto index = RegisterAudioDescription(tracks.front(), std::move(description));
    ContainerPacket packet{1, std::move(bytes), next_, next_, length, true, {}, index};
    next_ = {AddTime(ExactTicks(next_, scale), ExactTicks(length, scale)), scale};
    tracks.front().timescale = scale;
    return packet;
  }

private:
  MediaTimestamp next_;
};

class AdtsWriter final : public ContainerWriter {
public:
  using ContainerWriter::ContainerWriter;

  std::optional<std::uint32_t> Write(std::uint32_t, const ContainerPacket& packet) override {
    const auto data = packet.data.AsBytes();
    const auto length = data.size() + 7;
    if (length > 8191) Fail(ContainerErrorCode::LimitExceeded, "AAC packet does not fit an ADTS frame");
    const auto& description = tracks.front().descriptions.front();
    const auto rate = *description.sample_rate;
    const MediaTimestamp time{next_, rate}, duration{1024, rate};
    if ((packet.pts && *packet.pts != time) || (packet.dts && *packet.dts != time) ||
        (packet.duration && *packet.duration != duration))
      Fail(ContainerErrorCode::Unsupported, "ADTS cannot retain timing outside its continuous AAC frame grid");
    const auto next = AddTime(next_, 1024);
    const auto config = Number<std::uint16_t>(description.initialization_data);
    const auto frequency = (config >> 7) & 15;
    const auto channels = (config >> 3) & 15;
    const std::array header{
        std::byte{0xff}, std::byte{0xf1}, static_cast<std::byte>(0x40 | (frequency << 2) | (channels >> 2)),
        static_cast<std::byte>(((channels & 3) << 6) | (length >> 11)), static_cast<std::byte>((length >> 3) & 255),
        static_cast<std::byte>(((length & 7) << 5) | 31), std::byte{0xfc}};
    output.Write(header);
    output.Write(data);
    next_ = next;
    return {};
  }

  void Finish() override {
    if (!next_) Fail(ContainerErrorCode::InvalidState, "ADTS requires at least one packet");
  }

private:
  std::int64_t next_ = 0;
};
} // namespace

void ValidateAdts(std::span<const MediaTrack> tracks) {
  if (tracks.size() != 1 || tracks[0].kind != TrackKind::Audio || tracks[0].descriptions.front().codec != CodecId::Aac ||
      tracks[0].descriptions.front().framing != PacketFraming::CodecPacket)
    Fail(ContainerErrorCode::Unsupported, "ADTS requires one raw AAC-LC track");
  const auto& track = tracks[0];
  const auto& description = track.descriptions.front();
  const auto config = ParseAac(description.initialization_data);
  if (!config.Succeeded()) Fail(ContainerErrorCode::Unsupported, config.Error().message);
  if (description.sample_rate != config.Value().sample_rate || description.channel_count != config.Value().channels ||
      (track.timescale && track.timescale != description.sample_rate))
    Fail(ContainerErrorCode::InvalidArgument, "ADTS track metadata does not match AudioSpecificConfig");
  if (track.priming_samples.value_or(0) || track.padding_samples.value_or(0))
    Fail(ContainerErrorCode::Unsupported, "ADTS cannot preserve audio trim");
}

std::unique_ptr<ContainerReader> OpenAdtsReader(ContainerInput input) {
  return std::make_unique<AdtsReader>(std::move(input));
}

std::unique_ptr<ContainerWriter> OpenAdtsWriter(ContainerOutput output, std::span<const MediaTrack> tracks) {
  return std::make_unique<AdtsWriter>(std::move(output), tracks);
}

} // namespace huxerui::media::detail
