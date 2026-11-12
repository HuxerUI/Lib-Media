#include "annex_b.h"
#include "pcm.h"

namespace huxerui::media::detail {
namespace {
constexpr auto Invalid = ContainerErrorCode::InvalidData;
constexpr auto Unsupported = ContainerErrorCode::Unsupported;

struct Chunk {
  std::uint32_t type;
  ByteView data;
};

Chunk NextChunk(ByteView& remaining) {
  const auto type = Number<std::uint32_t>(remaining);
  const auto size = Number<std::uint32_t>(remaining, 4, true);
  const auto data = Slice(remaining, 8, size);
  remaining = remaining.subspan(Slice(remaining, 0, 8 + std::uint64_t(size) + (size & 1)).size());
  return {type, data};
}

class AviReader final : public IndexedImport {
public:
  explicit AviReader(ContainerInput source) : IndexedImport(std::move(source), ContainerFormat::Avi) {
    const auto bytes = std::make_shared<const Bytes>(ReadImportBytes(input));
    source_ = BufferReference(*bytes, bytes);
    auto body = source_.AsBytes();
    const auto riff = NextChunk(body);
    if (!body.empty() || riff.type != Tag("RIFF") || Number<std::uint32_t>(riff.data) != Tag("AVI "))
      Fail(Unsupported, "AVI import requires a single RIFF AVI segment");
    auto chunks = riff.data.subspan(4);
    bool media = false;
    while (!chunks.empty()) {
      const auto chunk = NextChunk(chunks);
      if (chunk.type != Tag("LIST")) continue;
      const auto type = Number<std::uint32_t>(chunk.data);
      if (type == Tag("hdrl")) {
        if (!streams_.empty() || media) Fail(Invalid, "Misplaced AVI stream headers");
        ReadHeaders(chunk.data.subspan(4));
      } else if (type == Tag("movi")) {
        if (streams_.empty() || media) Fail(Invalid, "AVI requires one media list following its stream headers");
        media = true;
        ReadMedia(chunk.data.subspan(4), false);
      }
    }
    for (auto& stream : streams_) {
      if (stream.video) {
        if (stream.video->Description().codec == CodecId::Unknown) Fail(Invalid, "AVI video has no picture");
        stream.track.descriptions = {stream.video->Description()};
      }
      tracks.push_back(std::move(stream.track));
    }
    Complete();
    source_ = {};
  }

private:
  struct Stream {
    MediaTrack track;
    std::uint32_t scale = 0, rate = 0, alignment = 0;
    std::int64_t next = 0;
    std::optional<AnnexBTrack> video;
  };

  void ReadHeaders(ByteView chunks) {
    while (!chunks.empty()) {
      const auto chunk = NextChunk(chunks);
      if (chunk.type != Tag("LIST") || Number<std::uint32_t>(chunk.data) != Tag("strl")) continue;
      auto fields = chunk.data.subspan(4);
      ByteView header, format;
      while (!fields.empty()) {
        const auto field = NextChunk(fields);
        if (field.type == Tag("strh")) header = field.data;
        if (field.type == Tag("strf")) format = field.data;
      }
      if (streams_.size() >= MaxContainerTracks) Fail(ContainerErrorCode::LimitExceeded, "AVI stream count exceeds its limit");
      if (header.size() < 48 || format.empty()) Fail(Invalid, "AVI stream has no complete header or format");
      auto& stream = streams_.emplace_back();
      stream.track.id = static_cast<std::uint32_t>(streams_.size());
      stream.scale = Number<std::uint32_t>(header, 20, true);
      stream.rate = Number<std::uint32_t>(header, 24, true);
      if (!stream.scale || !stream.rate) Fail(Invalid, "AVI stream has an invalid timebase");
      const auto first_sample = std::uint64_t(Number<std::uint32_t>(header, 28, true)) * stream.scale;
      if (first_sample > INT64_MAX) Fail(Invalid, "AVI stream start exceeds its timebase");
      stream.next = static_cast<std::int64_t>(first_sample);
      stream.track.timescale = stream.rate;
      const auto kind = Number<std::uint32_t>(header);
      if (kind == Tag("vids")) {
        const auto compression = Number<std::uint32_t>(format, 16);
        const auto codec = compression == Tag("H264") || compression == Tag("h264") || compression == Tag("X264") ?
            CodecId::H264 : compression == Tag("HEVC") || compression == Tag("hvc1") ? CodecId::Hevc : CodecId::Unknown;
        if (codec == CodecId::Unknown || Number<std::uint32_t>(header, 44, true))
          Fail(Unsupported, "AVI video requires frame-sized Annex B AVC or HEVC packets");
        stream.track.kind = TrackKind::Video;
        stream.video.emplace(codec);
        const auto header_size = Number<std::uint32_t>(format, 0, true);
        if (header_size < 40 || header_size > format.size()) Fail(Invalid, "Invalid AVI bitmap header size");
        if (header_size < format.size() && LooksLikeAnnexB(format.subspan(header_size)))
          stream.video->Convert(SplitAnnexB(format.subspan(header_size)), stream.track.id);
      } else if (kind == Tag("auds")) {
        const auto tag = Number<std::uint16_t>(format, 0, true);
        const auto channels = Number<std::uint16_t>(format, 2, true);
        const auto rate = Number<std::uint32_t>(format, 4, true);
        stream.alignment = Number<std::uint16_t>(format, 12, true);
        const auto bits = Number<std::uint16_t>(format, 14, true);
        if ((tag != 1 && tag != 3) || !channels || channels > 8 || !rate ||
            !stream.alignment || stream.alignment != channels * (bits / 8) ||
            std::uint64_t(stream.scale) * rate != stream.rate ||
            Number<std::uint32_t>(header, 44, true) != stream.alignment)
          Fail(Unsupported, "AVI audio requires a packed PCM sample clock");
        const auto codec = tag == 1 && bits == 8 ? CodecId::PcmU8 : SignedPcmCodec(bits, false, tag == 3);
        stream.next = ExactTicks({stream.next, stream.rate}, rate);
        stream.scale = 1;
        stream.rate = rate;
        stream.track = PcmTrack(codec, rate, channels);
        stream.track.id = static_cast<std::uint32_t>(streams_.size());
      } else Fail(Unsupported, "Unsupported AVI stream type");
    }
  }

  void ReadMedia(ByteView chunks, bool nested) {
    while (!chunks.empty()) {
      const auto offset = static_cast<std::uint64_t>(chunks.data() - source_.AsBytes().data());
      const auto chunk = NextChunk(chunks);
      if (chunk.type == Tag("LIST")) {
        if (Number<std::uint32_t>(chunk.data) == Tag("rec ")) {
          if (nested) Fail(Unsupported, "Nested AVI record lists are unsupported");
          ReadMedia(chunk.data.subspan(4), true);
        }
        continue;
      }
      const auto first = (chunk.type >> 24) & 255;
      const auto second = (chunk.type >> 16) & 255;
      const auto suffix = chunk.type & 0xffff;
      if (first < '0' || first > '9' || second < '0' || second > '9') continue;
      const auto index = (first - '0') * 10 + second - '0';
      if (index >= streams_.size()) Fail(Invalid, "AVI media refers to an absent stream");
      auto& stream = streams_[index];
      std::optional<ContainerPacket> packet;
      auto count = std::int64_t(stream.scale);
      if (stream.video && (suffix == Tag("dc") || suffix == Tag("db"))) {
        if (chunk.data.empty()) Fail(Unsupported, "AVI video contains an empty dropped-frame chunk");
        packet = stream.video->Convert(SplitAnnexB(chunk.data), stream.track.id);
        if (!packet || stream.video->IsReordered(*packet))
          Fail(Unsupported, "AVI import requires one unreordered picture per video chunk");
      } else if (stream.alignment && suffix == Tag("wb")) {
        if (chunk.data.empty()) continue;
        if (chunk.data.size() % stream.alignment || chunk.data.size() > MaxContainerPacket)
          Fail(Invalid, "AVI PCM chunk splits a frame or exceeds the packet limit");
        count = chunk.data.size() / stream.alignment;
        packet = ContainerPacket{.track_id = stream.track.id, .data = source_.Slice(offset + 8, chunk.data.size()),
            .sync_sample = true};
      }
      if (packet) {
        packet->pts = packet->dts = MediaTimestamp{stream.next, stream.rate};
        packet->duration = MediaTimestamp{count, stream.rate};
        stream.next = AddTime(stream.next, count);
        Add(std::move(*packet), offset);
      }
    }
  }

  std::vector<Stream> streams_;
  BufferReference source_;
};

} // namespace

std::unique_ptr<ContainerReader> OpenAviReader(ContainerInput input) { return std::make_unique<AviReader>(std::move(input)); }

} // namespace huxerui::media::detail
