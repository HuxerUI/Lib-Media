#include "framed_audio.h"

#include <array>

namespace huxerui::media::detail {

std::optional<Mp3FrameHeader> ParseMp3FrameHeader(ByteView bytes) {
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
  return Mp3FrameHeader{version, sample_rate, channels, mpeg1 ? 1152u : 576u, length, offset, side};
}

namespace {

constexpr auto Bad = ContainerErrorCode::InvalidData;

bool TagPrefix(ByteView bytes) {
  return bytes.size() >= 3 && bytes[0] == std::byte{'I'} && bytes[1] == std::byte{'D'} &&
         bytes[2] == std::byte{'3'};
}

std::uint32_t Syncsafe(ByteView bytes) {
  const auto value = Number<std::uint32_t>(bytes);
  if (value & 0x80808080u) Fail(Bad, "Invalid ID3 synchsafe size");
  return ((value >> 24) << 21) | (((value >> 16) & 127u) << 14) |
         (((value >> 8) & 127u) << 7) | (value & 127u);
}

Bytes Unsynchronize(ByteView bytes) {
  Bytes result;
  result.reserve(bytes.size());
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    result.push_back(bytes[i]);
    if (bytes[i] == std::byte{255} && i + 1 < bytes.size() && bytes[i + 1] == std::byte{}) ++i;
  }
  return result;
}

std::size_t TextEnd(ByteView bytes, unsigned encoding) {
  const auto width = encoding == 1 || encoding == 2 ? 2u : 1u;
  for (std::size_t i = 0; i + width <= bytes.size(); i += width) {
    if (bytes[i] == std::byte{} && (width == 1 || bytes[i + 1] == std::byte{})) return i;
  }
  return bytes.size();
}

void AppendUtf8(std::string& text, std::uint32_t value) {
  if (value < 128) { text.push_back(static_cast<char>(value)); return; }
  const auto count = value < 0x800 ? 2u : value < 0x10000 ? 3u : 4u;
  text.push_back(static_cast<char>((0xffu << (8 - count)) | (value >> ((count - 1) * 6))));
  for (auto i = count - 1; i > 0; --i) text.push_back(static_cast<char>(0x80 | ((value >> ((i - 1) * 6)) & 63)));
}

std::string DecodeText(ByteView bytes, unsigned encoding) {
  if (encoding > 3 || bytes.size() > MaxAudioText) return {};
  bytes = bytes.first(TextEnd(bytes, encoding));
  if (bytes.empty()) return {};
  if (encoding == 3) return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
  std::string text;
  if (!encoding) {
    for (auto value : bytes) AppendUtf8(text, std::to_integer<unsigned>(value));
    return text.size() <= MaxAudioText ? text : std::string{};
  }
  bool little = false;
  if (encoding == 1) {
    if (bytes.size() < 2) return {};
    const auto bom = Number<std::uint16_t>(bytes);
    if (bom != 0xfeff && bom != 0xfffe) return {};
    little = bom == 0xfffe;
    bytes = bytes.subspan(2);
  }
  if (bytes.size() % 2) return {};
  for (std::size_t i = 0; i < bytes.size(); i += 2) {
    std::uint32_t value = Number<std::uint16_t>(bytes, i, little);
    if (value >= 0xd800 && value < 0xe000) {
      if (value >= 0xdc00 || i + 4 > bytes.size()) return {};
      const auto low = Number<std::uint16_t>(bytes, i + 2, little);
      if (low < 0xdc00 || low >= 0xe000) return {};
      value = 0x10000 + ((value - 0xd800) << 10) + low - 0xdc00;
      i += 2;
    }
    AppendUtf8(text, value);
  }
  return text.size() <= MaxAudioText ? text : std::string{};
}

void ReadAttachedPicture(ByteView bytes, MediaAudioMetadata& metadata) {
  if (metadata.cover || bytes.size() < 4) return;
  const auto encoding = Number<std::uint8_t>(bytes);
  bytes = bytes.subspan(1);
  const auto mime_end = TextEnd(bytes, 0);
  if (mime_end + 2 > bytes.size() || encoding > 3) return;
  const std::string_view mime(reinterpret_cast<const char*>(bytes.data()), mime_end);
  if (!IsCoverMime(mime) || Number<std::uint8_t>(bytes, mime_end + 1) != 3) return;
  bytes = bytes.subspan(mime_end + 2);
  const auto description_end = TextEnd(bytes, encoding);
  const auto width = encoding == 1 || encoding == 2 ? 2u : 1u;
  if (description_end + width >= bytes.size()) return;
  bytes = bytes.subspan(description_end + width);
  if (bytes.size() > MaxContainerPacket - 64) return;
  auto owner = std::make_shared<const Bytes>(bytes.begin(), bytes.end());
  metadata.cover = MediaAudioMetadata::Cover{std::string(mime), BufferReference(*owner, owner)};
}

MediaAudioMetadata ReadId3(ContainerInput& input) {
  MediaAudioMetadata metadata;
  const auto prefix = input.Peek(10);
  if (!TagPrefix(prefix)) return metadata;
  if (prefix.size() != 10) Fail(Bad, "Truncated ID3 header");
  const auto version = Number<std::uint8_t>(prefix, 3);
  if (version < 2 || version > 4) Fail(ContainerErrorCode::Unsupported, "Unsupported ID3 version");
  const auto flags = Number<std::uint8_t>(prefix, 5);
  const auto count = Syncsafe(prefix.subspan(6));
  if (count > MaxContainerMetadata) Fail(ContainerErrorCode::LimitExceeded, "ID3 metadata exceeds its size limit");
  input.Skip(10);
  if (version == 2) {
    input.Skip(count);
    return metadata;
  }
  auto storage = input.ReadBytes(count);
  if (version == 4 && (flags & 16)) input.Skip(10);
  if (version == 3 && (flags & 128)) storage = Unsynchronize(storage);
  ByteView bytes = storage;
  if (flags & 64) {
    const auto extent = version == 4 ? Syncsafe(bytes) : AddOffset(4, Number<std::uint32_t>(bytes));
    if (extent > bytes.size()) Fail(Bad, "Truncated ID3 extended header");
    bytes = bytes.subspan(static_cast<std::size_t>(extent));
  }
  while (!bytes.empty() && bytes.front() != std::byte{}) {
    if (bytes.size() < 10) Fail(Bad, "Truncated ID3 frame header");
    const std::string_view id(reinterpret_cast<const char*>(bytes.data()), 4);
    const auto size = version == 4 ? Syncsafe(bytes.subspan(4)) : Number<std::uint32_t>(bytes, 4);
    const auto format = Number<std::uint8_t>(bytes, 9);
    auto value = Slice(bytes, 10, size);
    bytes = bytes.subspan(10 + size);
    if (value.empty() || (version == 3 ? format & 0xe0 : format & 0x4c)) continue;
    Bytes original;
    if (version == 4 && ((flags & 128) || (format & 2))) {
      original = Unsynchronize(value);
      value = original;
    }
    if (version == 4 && (format & 1)) {
      Syncsafe(value);
      value = value.subspan(4);
    }
    if (value.empty()) continue;
    if (id == "APIC") {
      ReadAttachedPicture(value, metadata);
    } else {
      const auto key = id == "TYER" ? std::string_view("TDRC") : id;
      const auto tag = std::ranges::find(AudioTags, key, &AudioTag::id3);
      if (tag != AudioTags.end() && (metadata.*tag->field).empty())
        metadata.*tag->field = DecodeText(value.subspan(1), Number<std::uint8_t>(value));
    }
  }
  return metadata;
}

void PutSyncsafe(Bytes& bytes, std::uint32_t value) {
  for (int shift = 21; shift >= 0; shift -= 7) bytes.push_back(std::byte((value >> shift) & 127));
}

void WriteId3(ContainerOutput& output, const MediaAudioMetadata& metadata) {
  Bytes frames;
  const auto frame = [&](std::string_view key, ByteView payload) {
    Append(frames, std::as_bytes(std::span(key)));
    PutSyncsafe(frames, static_cast<std::uint32_t>(payload.size() + 1));
    Put<std::uint16_t>(frames, 0);
    frames.push_back(std::byte{3});
    Append(frames, payload);
  };
  for (const auto& tag : AudioTags) {
    const auto& value = metadata.*tag.field;
    if (!value.empty()) frame(tag.id3, std::as_bytes(std::span(value)));
  }
  if (metadata.cover) {
    const auto& cover = *metadata.cover;
    Bytes picture;
    Append(picture, std::as_bytes(std::span(cover.mime_type)));
    picture.insert(picture.end(), {std::byte{}, std::byte{3}, std::byte{}});
    Append(picture, cover.data.AsBytes());
    frame("APIC", picture);
  }
  if (!frames.empty()) {
    Bytes header{std::byte{'I'}, std::byte{'D'}, std::byte{'3'}, std::byte{4}, std::byte{}, std::byte{}};
    PutSyncsafe(header, static_cast<std::uint32_t>(frames.size()));
    output.Write(header);
    output.Write(frames);
  }
}

struct Mp3Timing {
  std::optional<std::uint64_t> samples;
  std::optional<std::uint64_t> delay;
  std::optional<std::uint64_t> padding;
};

std::optional<Mp3Timing> ReadXing(ByteView frame, const Mp3FrameHeader& header) {
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

std::optional<Mp3Timing> ReadVbri(ByteView frame, const Mp3FrameHeader& header) {
  constexpr std::size_t offset = 36;
  if (frame.size() < offset + 4 || Number<std::uint32_t>(frame, offset) != Tag("VBRI")) return {};
  if (Number<std::uint16_t>(frame, offset + 4) != 1) return {};
  const auto frames = Number<std::uint32_t>(frame, offset + 14);
  if (!frames) Fail(Bad, "VBRI frame count is zero");
  return Mp3Timing{.samples = std::uint64_t(frames) * header.samples};
}

bool Restartable(ByteView bytes, const Mp3FrameHeader& header) {
  return Number<std::uint8_t>(bytes, header.side_offset) == 0 &&
      (header.version != 3 || !(Number<std::uint8_t>(bytes, header.side_offset + 1) & 128u));
}

class Mp3Reader final : public FramedAudioReader {
public:
  explicit Mp3Reader(ContainerInput input) : FramedAudioReader(std::move(input), ContainerFormat::Mp3) {
    auto metadata = ReadId3(this->input);
    const auto first = ParseMp3FrameHeader(this->input.Peek(4));
    if (!first) Fail(Bad, "Missing MPEG Layer III frame");
    info_ = *first;
    const auto bytes = this->input.Peek(info_.length);
    if (bytes.size() != info_.length) Fail(Bad, "Truncated MPEG Layer III frame");
    auto timing = ReadXing(bytes, info_);
    if (!timing) timing = ReadVbri(bytes, info_);
    if (timing) this->input.Skip(info_.length);
    MediaTrack track{.id = 1, .kind = TrackKind::Audio, .codec = CodecId::Mp3, .timescale = info_.rate,
                     .sample_rate = info_.rate, .channel_count = info_.channels,
                     .priming_samples = timing ? timing->delay : std::nullopt,
                     .padding_samples = timing ? timing->padding : std::nullopt};
    if (HasAudioMetadata(metadata)) track.audio_metadata = std::move(metadata);
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
    const auto header = ParseMp3FrameHeader(prefix);
    if (!header || header->version != info_.version || header->rate != info_.rate ||
        header->channels != info_.channels) Fail(Bad, "Invalid or changing MPEG Layer III frame");
    auto bytes = input.ReadBytes(header->length);
    const auto restartable = Restartable(bytes, *header);
    return AudioPacket{std::move(bytes), header->samples, restartable};
  }
  Mp3FrameHeader info_{};
};

class Mp3Writer final : public ContainerWriter {
public:
  Mp3Writer(ContainerOutput output, std::span<const MediaTrack> descriptions)
      : ContainerWriter(std::move(output), descriptions) {
    if (const auto& tags = tracks.front().audio_metadata) WriteId3(this->output, *tags);
    xing_offset_ = this->output.Position();
  }

  std::optional<std::uint32_t> Write(std::uint32_t, const ContainerPacket& packet) override {
    const auto bytes = packet.data.AsBytes();
    const auto header = ParseMp3FrameHeader(bytes);
    const auto& track = tracks.front();
    if (!header || header->length != bytes.size() || header->rate != track.sample_rate || header->channels != track.channel_count)
      Fail(ContainerErrorCode::InvalidArgument, "MP3 output requires matching complete MPEG Layer III frames");
    if (offsets_.size() == MaxContainerSamples)
      Fail(ContainerErrorCode::LimitExceeded, "MP3 output exceeds its frame index limit");
    const auto priming = static_cast<std::int64_t>(track.priming_samples.value_or(0));
    ValidateAudioPacketTime(packet, samples_ - priming, header->samples, header->rate);
    const auto bitrate = Number<std::uint8_t>(bytes, 2) >> 4;
    if (offsets_.empty()) {
      if (!Restartable(bytes, *header)) Fail(ContainerErrorCode::InvalidArgument, "First MP3 frame depends on a previous reservoir");
      // Reserve a header-only frame with the stream geometry and space for Xing and LAME fields.
      Put(xing_, (Number<std::uint32_t>(bytes) & ~0xf200u) | 0xe000u | 0x10000u);
      const auto xing_header = ParseMp3FrameHeader(xing_).value();
      xing_.resize(xing_header.length);
      xing_data_ = xing_header.side_offset + xing_header.side_size;
      if (xing_.size() - xing_data_ < 144) Fail(ContainerErrorCode::InvalidArgument, "MP3 header frame has no room for seek data");
      output.Write(xing_);
      first_bitrate_ = bitrate;
    }
    const auto offset = output.Position() - xing_offset_;
    if (AddOffset(offset, bytes.size()) > UINT32_MAX)
      Fail(ContainerErrorCode::LimitExceeded, "MP3 output exceeds the Xing byte count");
    offsets_.push_back(static_cast<std::uint32_t>(offset));
    variable_ |= bitrate != first_bitrate_;
    output.Write(bytes);
    samples_ = AddTime(samples_, header->samples);
    return {};
  }

  void Finish() override {
    if (offsets_.empty()) Fail(ContainerErrorCode::InvalidArgument, "MP3 output needs at least one frame");
    const auto& track = tracks.front();
    const auto delay = track.priming_samples.value_or(0), padding = track.padding_samples.value_or(0);
    if (delay + padding >= static_cast<std::uint64_t>(samples_))
      Fail(ContainerErrorCode::InvalidArgument, "MP3 trim removes all coded samples");
    const auto size = output.Position() - xing_offset_;
    Bytes fields;
    Put(fields, Tag(variable_ ? "Xing" : "Info"));
    Put(fields, 7u);
    Put(fields, static_cast<std::uint32_t>(offsets_.size()));
    Put(fields, static_cast<std::uint32_t>(size));
    fields.push_back(std::byte{});
    for (std::size_t i = 1; i < 100; ++i) {
      const auto offset = offsets_[i * offsets_.size() / 100];
      fields.push_back(std::byte(std::uint64_t(offset) * 256 / size));
    }
    if (track.priming_samples && track.padding_samples) {
      Put(fields, Tag("LAME"));
      fields.resize(fields.size() + 17);
      fields.push_back(std::byte(delay >> 4));
      fields.push_back(std::byte(((delay & 15) << 4) | (padding >> 8)));
      fields.push_back(std::byte(padding & 255));
    }
    std::ranges::copy(fields, xing_.begin() + xing_data_);
    output.Patch(xing_offset_, xing_);
  }

private:
  Bytes xing_;
  std::vector<std::uint32_t> offsets_;
  std::uint64_t xing_offset_ = 0;
  std::size_t xing_data_ = 0;
  std::int64_t samples_ = 0;
  int first_bitrate_ = 0;
  bool variable_ = false;
};
} // namespace

bool LooksLikeMp3(ByteView bytes) {
  return TagPrefix(bytes) || ParseMp3FrameHeader(bytes).has_value();
}

std::unique_ptr<ContainerReader> OpenMp3Reader(ContainerInput input) {
  return std::make_unique<Mp3Reader>(std::move(input));
}

void ValidateMp3(std::span<const MediaTrack> tracks) {
  constexpr std::array rates{8000u, 11025u, 12000u, 16000u, 22050u, 24000u, 32000u, 44100u, 48000u};
  if (tracks.size() != 1 || tracks.front().kind != TrackKind::Audio || tracks.front().codec != CodecId::Mp3 ||
      tracks.front().framing != PacketFraming::CodecPacket)
    Fail(ContainerErrorCode::Unsupported, "MP3 output requires one MPEG Layer III track");
  const auto& track = tracks.front();
  if (std::ranges::find(rates, track.sample_rate.value_or(0)) == rates.end() ||
      (track.channel_count != 1 && track.channel_count != 2) || !track.initialization_data.empty() ||
      track.priming_samples.has_value() != track.padding_samples.has_value() ||
      track.priming_samples.value_or(0) > 4095 || track.padding_samples.value_or(0) > 4095)
    Fail(ContainerErrorCode::InvalidArgument, "Invalid MP3 output geometry or trim");
  if (track.audio_metadata) ValidateAudioMetadata(*track.audio_metadata);
}

std::unique_ptr<ContainerWriter> OpenMp3Writer(ContainerOutput output, std::span<const MediaTrack> tracks) {
  return std::make_unique<Mp3Writer>(std::move(output), tracks);
}
} // namespace huxerui::media::detail
