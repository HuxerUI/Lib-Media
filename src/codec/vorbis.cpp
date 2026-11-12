#include "vorbis.h"

namespace huxerui::media::detail {
namespace {

constexpr std::size_t MaxVorbisHeaders = 65536;

std::size_t TakeXiphSize(ByteView& fields, ContainerErrorCode error) {
  std::size_t size = 0;
  while (!fields.empty()) {
    const auto next = std::to_integer<unsigned>(fields.front());
    fields = fields.subspan(1);
    size += next;
    if (size > MaxVorbisHeaders) break;
    if (next < 255) return size;
  }
  Fail(error, "Invalid Vorbis Xiph lace length");
}

} // namespace

std::array<ByteView, 3> VorbisHeaderPackets(ByteView configuration, ContainerErrorCode error) {
  if (configuration.empty() || configuration.size() > MaxVorbisHeaders || configuration.front() != std::byte{2})
    Fail(error, "Vorbis requires three Xiph-laced headers within 64 KiB");
  auto fields = configuration.subspan(1);
  const auto first = TakeXiphSize(fields, error), second = TakeXiphSize(fields, error);
  if (!first || !second || first + second >= fields.size()) Fail(error, "Vorbis header lengths exceed the configuration");
  return {fields.first(first), fields.subspan(first, second), fields.subspan(first + second)};
}

Bytes PackVorbisHeaders(const std::array<Bytes, 3>& headers) {
  const auto size = 3 + headers[0].size() / 255 + headers[1].size() / 255 +
      headers[0].size() + headers[1].size() + headers[2].size();
  if (size > MaxVorbisHeaders) Fail(ContainerErrorCode::LimitExceeded, "Vorbis headers exceed 64 KiB");
  Bytes configuration;
  configuration.reserve(size);
  configuration.push_back(std::byte{2});
  for (auto size : {headers[0].size(), headers[1].size()}) {
    configuration.insert(configuration.end(), size / 255, std::byte{255});
    configuration.push_back(static_cast<std::byte>(size % 255));
  }
  for (const auto& header : headers) Append(configuration, header);
  return configuration;
}

ogg_packet VorbisPacket(ByteView data, bool beginning) {
  return {reinterpret_cast<unsigned char*>(const_cast<std::byte*>(data.data())), static_cast<long>(data.size()),
      beginning, 0, -1, 0};
}

VorbisHeaders::VorbisHeaders(ByteView configuration, ContainerErrorCode error) {
  const auto headers = VorbisHeaderPackets(configuration, error);
  if (headers[0].size() != 30) Fail(error, "Invalid Vorbis identification header length");
  const auto channels = Number<std::uint8_t>(headers[0], 11);
  if (channels != 1 && channels != 2) Fail(ContainerErrorCode::Unsupported, "Vorbis requires mono or stereo");
  vorbis_comment comments{};
  vorbis_info_init(&info_);
  vorbis_comment_init(&comments);
  bool valid = true;
  for (std::size_t i = 0; i < headers.size(); ++i) {
    auto packet = VorbisPacket(headers[i], i == 0);
    if (vorbis_synthesis_headerin(&info_, &comments, &packet) != 0) {
      valid = false;
      break;
    }
  }
  vorbis_comment_clear(&comments);
  if (!valid || info_.rate <= 0 || MaxSamples() == 0) {
    vorbis_info_clear(&info_);
    Fail(error, "Invalid Vorbis header sequence");
  }
}

VorbisHeaders::~VorbisHeaders() { vorbis_info_clear(&info_); }

unsigned VorbisHeaders::MaxSamples() const {
  const auto block = vorbis_info_blocksize(&info_, 1);
  return block > 0 ? static_cast<unsigned>(block) / 2 : 0;
}

unsigned VorbisHeaders::BlockSize(ByteView data, ContainerErrorCode error) const {
  if (data.empty() || data.size() > MaxContainerPacket) Fail(error, "Invalid Vorbis packet length");
  auto packet = VorbisPacket(data);
  const auto size = vorbis_packet_blocksize(&info_, &packet);
  if (size <= 0) Fail(error, "Invalid Vorbis audio packet mode");
  return static_cast<unsigned>(size);
}

} // namespace huxerui::media::detail
