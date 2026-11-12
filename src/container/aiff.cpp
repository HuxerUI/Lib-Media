#include "pcm.h"

#include "format/packed_pcm.h"

namespace huxerui::media::detail {
namespace {

constexpr auto Invalid = ContainerErrorCode::InvalidData;

std::uint32_t SampleRate(ByteView bytes) {
  const auto exponent = Number<std::uint16_t>(bytes);
  const auto significand = Number<std::uint64_t>(bytes, 2);
  if (exponent & 0x8000 || exponent == 0x7fff || !(significand >> 63))
    Fail(Invalid, "Invalid AIFF sample rate");
  if (exponent < 16383 || exponent > 16383 + 31)
    Fail(ContainerErrorCode::Unsupported, "AIFF sample rate is outside the integer sample clock");
  const auto shift = 63 - (exponent - 16383);
  if (significand & ((std::uint64_t{1} << shift) - 1))
    Fail(ContainerErrorCode::Unsupported, "Fractional AIFF sample rates are unsupported");
  return static_cast<std::uint32_t>(significand >> shift);
}

void PutRate(Bytes& bytes, std::uint32_t rate) {
  const auto exponent = std::bit_width(rate) - 1;
  Put<std::uint16_t>(bytes, static_cast<std::uint16_t>(16383 + exponent));
  Put(bytes, std::uint64_t(rate) << (63 - exponent));
}

class AiffReader final : public PcmReader {
public:
  explicit AiffReader(ContainerInput input) : PcmReader(std::move(input), ContainerFormat::Aiff) {
    const auto header = this->input.ReadBytes(12);
    const auto end = std::uint64_t{8} + Number<std::uint32_t>(header, 4);
    if (end < 12 || (this->input.Size() && end > *this->input.Size()))
      Fail(Invalid, "AIFF FORM exceeds its input");
    std::optional<MediaTrack> track;
    struct Sound { std::uint64_t offset, bytes; };
    std::optional<Sound> sound;
    Bytes layout;
    std::uint32_t frames = 0;
    unsigned alignment = 0;
    std::size_t chunks = 0;
    while (end - this->input.Position() >= 8) {
      if (++chunks > 4096 || (!this->input.Random() && this->input.Position() > MaxContainerMetadata))
        Fail(ContainerErrorCode::LimitExceeded, "AIFF chunk discovery exceeds its budget");
      const auto chunk = this->input.ReadBytes(8);
      const auto type = Number<std::uint32_t>(chunk);
      const std::uint64_t size = Number<std::uint32_t>(chunk, 4);
      const auto payload_end = AddOffset(this->input.Position(), size);
      const auto next = payload_end == end ? end : AddOffset(payload_end, size & 1);
      if (next > end) Fail(Invalid, "AIFF chunk exceeds FORM size");
      if (type == Tag("COMM")) {
        if (track || size != 18) Fail(Invalid, "Invalid AIFF Common chunk");
        const auto common = this->input.ReadBytes(18);
        const auto channels = Number<std::uint16_t>(common);
        const auto bits = Number<std::uint16_t>(common, 6);
        if (!channels) Fail(Invalid, "AIFF channel count is zero");
        if (channels > 8) Fail(ContainerErrorCode::Unsupported, "AIFF PCM supports up to eight channels");
        track = PcmTrack(SignedPcmCodec(bits, true), SampleRate(ByteView(common).subspan(8)), channels);
        frames = Number<std::uint32_t>(common, 2);
        alignment = channels * (bits / 8);
      } else if (type == Tag("CHAN")) {
        if (!layout.empty() || size < 12 || size > 4096) Fail(Invalid, "Invalid AIFF Channel Layout chunk");
        layout = this->input.ReadBytes(static_cast<std::size_t>(size));
      } else if (type == Tag("SSND")) {
        if (sound || size < 8) Fail(Invalid, "Invalid AIFF Sound Data chunk");
        const auto data = this->input.ReadBytes(8);
        const auto offset = Number<std::uint32_t>(data);
        if (offset > size - 8) Fail(Invalid, "AIFF sound offset exceeds the chunk");
        sound = Sound{AddOffset(this->input.Position(), offset), size - 8 - offset};
        if (!track && !this->input.Random())
          Fail(ContainerErrorCode::NeedRandomAccess, "AIFF audio precedes its Common chunk");
      }
      if (track && sound && (!this->input.Random() || !layout.empty() ||
          track->descriptions.front().channel_count.value_or(0) <= 2)) break;
      if (this->input.Random()) this->input.Seek(next);
      else SkipHeader(next);
    }
    if (!track || (!sound && (frames || end != this->input.Position()))) Fail(Invalid, "AIFF is missing required audio chunks");
    if (!layout.empty()) ReadPcmChannelLayout(track->descriptions.front(), std::move(layout));
    const auto bytes = std::uint64_t(frames) * alignment;
    if (sound) {
      if (bytes > sound->bytes) Fail(Invalid, "AIFF sound data is shorter than its sample count");
      if (this->input.Random()) this->input.Seek(sound->offset);
      else SkipHeader(sound->offset);
    }
    Begin(std::move(*track), bytes, alignment);
  }

private:
  void SkipHeader(std::uint64_t offset) {
    const auto skip = offset - input.Position();
    if (skip > MaxContainerMetadata) Fail(ContainerErrorCode::LimitExceeded, "AIFF header skip exceeds its budget");
    input.Skip(skip);
  }
};

class AiffWriter final : public ContainerWriter {
public:
  AiffWriter(ContainerOutput output, std::span<const MediaTrack> tracks) : ContainerWriter(std::move(output), tracks) {
    const auto& description = tracks.front().descriptions.front();
    const auto bits = DescribePackedPcm(description.codec).bits;
    rate_ = *description.sample_rate;
    alignment_ = *description.channel_count * (bits / 8);
    Bytes header;
    Put(header, Tag("FORM"));
    Put(header, 0u);
    Put(header, Tag("AIFF"));
    Put(header, Tag("COMM"));
    Put(header, 18u);
    Put<std::uint16_t>(header, static_cast<std::uint16_t>(*description.channel_count));
    frame_count_offset_ = header.size();
    Put(header, 0u);
    Put<std::uint16_t>(header, static_cast<std::uint16_t>(bits));
    PutRate(header, rate_);
    if (const auto layout = WritePcmChannelLayout(description); !layout.empty()) {
      Put(header, Tag("CHAN"));
      Put(header, static_cast<std::uint32_t>(layout.size()));
      Append(header, layout);
      if (layout.size() & 1) header.push_back(std::byte{});
    }
    Put(header, Tag("SSND"));
    sound_size_offset_ = header.size();
    Put(header, 0u);
    Put(header, 0u);
    Put(header, 0u);
    this->output.Write(header);
  }

  std::optional<std::uint32_t> Write(std::uint32_t, const ContainerPacket& packet) override {
    const auto frames = PcmPacketFrames(packet, alignment_, frames_, rate_);
    const auto end = AddOffset(output.Position(), packet.data.AsBytes().size());
    if (AddOffset(end, end & 1) - 8 > UINT32_MAX)
      Fail(ContainerErrorCode::LimitExceeded, "AIFF exceeds its 32-bit FORM size");
    output.Write(packet.data.AsBytes());
    frames_ += frames;
    return {};
  }

  void Finish() override {
    const auto bytes = frames_ * alignment_;
    if (bytes & 1) {
      constexpr std::array padding{std::byte{0}};
      output.Write(padding);
    }
    Patch(4, static_cast<std::uint32_t>(output.Position() - 8));
    Patch(frame_count_offset_, static_cast<std::uint32_t>(frames_));
    Patch(sound_size_offset_, static_cast<std::uint32_t>(bytes + 8));
  }

private:
  void Patch(std::uint64_t offset, std::uint32_t value) {
    Bytes bytes;
    Put(bytes, value);
    output.Patch(offset, bytes);
  }

  std::uint32_t rate_;
  unsigned alignment_;
  std::uint64_t frame_count_offset_, sound_size_offset_;
  std::int64_t frames_ = 0;
};

} // namespace

void ValidateAiff(std::span<const MediaTrack> tracks) {
  const auto& track = tracks.front();
  const auto& description = track.descriptions.front();
  const auto pcm = DescribePackedPcm(description.codec);
  if (tracks.size() != 1 || track.kind != TrackKind::Audio || description.framing != PacketFraming::CodecPacket ||
      !pcm.big_endian || pcm.floating_point)
    Fail(ContainerErrorCode::Unsupported, "AIFF requires one big-endian signed PCM track");
  ValidatePcmTrack(track);
}

std::unique_ptr<ContainerReader> OpenAiffReader(ContainerInput input) {
  return std::make_unique<AiffReader>(std::move(input));
}

std::unique_ptr<ContainerWriter> OpenAiffWriter(ContainerOutput output, std::span<const MediaTrack> tracks) {
  return std::make_unique<AiffWriter>(std::move(output), tracks);
}

} // namespace huxerui::media::detail
