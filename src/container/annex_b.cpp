#include "annex_b.h"

#include "codec/encoder/format.h"
#include "codec/hevc.h"

namespace huxerui::media::detail {
namespace {
constexpr auto Invalid = ContainerErrorCode::InvalidData;
constexpr auto Unsupported = ContainerErrorCode::Unsupported;
constexpr std::array StartCode{std::byte{}, std::byte{}, std::byte{}, std::byte{1}};

class AnnexBReader final : public IndexedImport {
public:
  AnnexBReader(ContainerInput source, CodecId codec) : IndexedImport(std::move(source), ContainerFormat::AnnexB) {
    auto data = ReadImportBytes(input);
    const auto units = SplitAnnexB(data);
    const auto nals = std::span(units);
    AnnexBTrack video(codec);
    std::size_t first = 0;
    bool picture = false;
    for (std::size_t i = 0; i < nals.size(); ++i) {
      const auto nal = nals[i].bytes;
      const bool begins = video.StartsPicture(nal);
      if (picture && (begins || video.Prefix(nal))) {
        if (auto packet = video.Convert(nals.subspan(first, i - first), 1)) Add(std::move(*packet), nals[first].offset);
        first = i;
        picture = false;
      }
      picture |= begins;
    }
    if (auto packet = video.Convert(nals.subspan(first), 1)) Add(std::move(*packet), nals[first].offset);
    tracks.push_back({.id = 1, .kind = TrackKind::Video, .descriptions = {video.Description()}});
    Complete();
  }
};
} // namespace

std::vector<AnnexBNal> SplitAnnexB(ByteView bytes) {
  std::vector<AnnexBNal> nals;
  std::size_t zeros = 0, start = 0, offset = 0;
  const auto add = [&](std::size_t end) {
    if (end <= start) Fail(Invalid, "Empty Annex B NAL unit");
    if (nals.size() == MaxContainerSamples) Fail(ContainerErrorCode::LimitExceeded, "Annex B NAL count exceeds its limit");
    nals.push_back({bytes.subspan(start, end - start), offset});
  };
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    if (bytes[i] == std::byte{}) { ++zeros; continue; }
    if (bytes[i] == std::byte{1} && zeros >= 2) {
      if (start) add(i - zeros);
      else if (i != zeros) Fail(Invalid, "Bytes precede the first Annex B start code");
      start = i + 1;
      offset = i - zeros;
    }
    zeros = 0;
  }
  if (!start) Fail(Invalid, "Missing Annex B start code");
  add(bytes.size() - zeros);
  return nals;
}

std::optional<CodecId> LooksLikeAnnexB(ByteView bytes) {
  std::size_t zeros = 0;
  bool began = false;
  for (std::size_t i = 0; i + 1 < bytes.size(); ++i) {
    if (bytes[i] == std::byte{}) { ++zeros; continue; }
    if (bytes[i] == std::byte{1} && zeros >= 2) {
      if (!began && i != zeros) return {};
      began = true;
      const auto header = std::to_integer<unsigned>(bytes[i + 1]);
      if ((header & 0x9f) == 7) return CodecId::H264;
      if (header == 0x42 && i + 2 < bytes.size() && (std::to_integer<unsigned>(bytes[i + 2]) & 7))
        return CodecId::Hevc;
    }
    zeros = 0;
  }
  return {};
}

unsigned AnnexBTrack::Type(ByteView nal) const {
  if (codec_ == CodecId::Hevc) {
    try { return HevcNalType(nal); }
    catch (const std::invalid_argument& error) { Fail(Invalid, error.what()); }
  }
  const auto header = Number<std::uint8_t>(nal);
  const auto type = header & 31;
  if ((header & 128) || !type || type > 12 || (type >= 2 && type <= 4))
    Fail(Unsupported, "Unsupported Annex B AVC NAL unit");
  return type;
}
bool AnnexBTrack::Picture(unsigned type) const {
  return codec_ == CodecId::Hevc ? type <= 31 : type == 1 || type == 5;
}
bool AnnexBTrack::Parameter(unsigned type) const {
  return codec_ == CodecId::Hevc ? type >= 32 && type <= 34 : type == 7 || type == 8;
}
bool AnnexBTrack::StartsPicture(ByteView nal) const {
  return Picture(Type(nal)) && (Number<std::uint8_t>(nal, codec_ == CodecId::Hevc ? 2 : 1) & 128);
}
bool AnnexBTrack::Prefix(ByteView nal) const {
  const auto type = Type(nal);
  return Parameter(type) || (codec_ == CodecId::Hevc ? type == 35 || type == 39 : type == 6 || type == 9);
}

std::optional<ContainerPacket> AnnexBTrack::Convert(std::span<const AnnexBNal> nals, std::uint32_t track) {
  Bytes packet;
  bool picture = false, key = false;
  for (const auto& entry : nals) {
    const auto nal = entry.bytes;
    const auto type = Type(nal);
    if (Parameter(type)) {
      const bool known = std::ranges::any_of(parameters_, [nal](const Bytes& bytes) { return std::ranges::equal(bytes, nal); });
      if (!known) {
        if (description_.codec != CodecId::Unknown)
          Fail(Unsupported, "Annex B parameter sets changed during import");
        if (parameters_.size() >= 64 || nal.size() > 65535)
          Fail(ContainerErrorCode::LimitExceeded, "Annex B parameter sets exceed their limits");
        parameters_.emplace_back(nal.begin(), nal.end());
      }
      continue;
    }
    if (type == (codec_ == CodecId::Hevc ? 35 : 9)) continue;
    if (Picture(type)) {
      if (StartsPicture(nal)) {
        if (picture) Fail(Unsupported, "Expected one picture per imported video access unit");
        picture = true;
        key = codec_ == CodecId::Hevc ? type == 19 || type == 20 : type == 5;
      } else if (!picture) Fail(Invalid, "Annex B access unit starts inside a picture");
    }
    if (packet.size() > MaxContainerPacket - 4 || nal.size() > MaxContainerPacket - 4 - packet.size())
      Fail(ContainerErrorCode::LimitExceeded, "Annex B access unit exceeds its byte limit");
    Put(packet, static_cast<std::uint32_t>(nal.size()));
    Append(packet, nal);
  }
  if (!picture) return {};
  if (description_.codec == CodecId::Unknown) {
    Bytes parameters;
    for (const auto& set : parameters_) {
      if (parameters.size() + set.size() + StartCode.size() > 65536)
        Fail(ContainerErrorCode::LimitExceeded, "Annex B configuration exceeds 64 KiB");
      Append(parameters, StartCode);
      Append(parameters, set);
    }
    auto format = ReadAnnexBVideoFormat(parameters, codec_);
    if (!format.Succeeded()) Fail(Unsupported, format.Error().message);
    auto value = std::move(format).Value();
    description_ = {.codec = codec_, .framing = PacketFraming::LengthPrefixedNals,
        .initialization_data = std::move(value.initialization_data), .coded_width = value.width,
        .coded_height = value.height, .visible = value.visible, .sample_entry = codec_ == CodecId::Hevc ? "hvc1" : "avc1"};
  }
  return ContainerPacket{.track_id = track, .data = Own(std::move(packet)), .sync_sample = key};
}

bool AnnexBTrack::IsReordered(const ContainerPacket& packet) const {
  const VideoEncodeFormat format{*description_.coded_width, *description_.coded_height, *description_.visible,
      description_.initialization_data, codec_};
  const auto info = InspectVideoEncoderPacket({packet.data, packet.pts.value_or(MediaTimestamp{}), packet.dts, packet.duration,
      packet.sync_sample.value_or(false)}, format);
  if (!info.Succeeded()) Fail(Invalid, info.Error().message);
  return info.Value().b_frame;
}

std::unique_ptr<ContainerReader> OpenAnnexBReader(ContainerInput input, CodecId codec) {
  return std::make_unique<AnnexBReader>(std::move(input), codec);
}

} // namespace huxerui::media::detail
