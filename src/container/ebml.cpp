#include "ebml.h"

#include <array>
#include <cmath>

namespace huxerui::media::detail::ebml {
namespace {

unsigned Width(std::byte first, bool id) {
  const auto width = static_cast<unsigned>(std::countl_zero(std::to_integer<std::uint8_t>(first))) + 1;
  if (width > (id ? 4u : 8u)) Fail(BadData, "Invalid EBML variable integer width");
  return width;
}

std::optional<Vint> PullVint(ContainerInput& input, bool id, std::optional<std::uint64_t> end) {
  std::array<std::byte, 8> raw;
  const auto position = input.Position();
  if (end && position >= *end) return {};
  if (!input.ReadExact(std::span(raw).first(1))) return {};
  const auto width = Width(raw.front(), id);
  if (end && width > *end - position) Fail(BadData, "EBML element header crosses its parent boundary");
  if (!input.ReadExact(std::span(raw).subspan(1, width - 1))) Fail(BadData, "Truncated EBML variable integer");
  auto bytes = ByteView(raw).first(width);
  return ReadVint(bytes, id);
}

void PutNumber(Bytes& out, std::uint64_t value, unsigned width) {
  for (unsigned i = width; i; --i) Put<std::uint8_t>(out, static_cast<std::uint8_t>(value >> (8 * (i - 1))));
}

} // namespace

Vint ReadVint(ByteView& bytes, bool id) {
  if (bytes.empty()) Fail(BadData, "Missing EBML variable integer");
  const auto width = Width(bytes.front(), id);
  if (width > bytes.size()) Fail(BadData, "Truncated EBML variable integer");
  auto value = UInt(bytes.first(width));
  bytes = bytes.subspan(width);
  const auto marker = std::uint64_t{1} << (7 * width);
  const bool unknown = (value & (marker - 1)) == marker - 1;
  if (id && unknown) Fail(BadData, "Reserved EBML element ID");
  return {id ? value : value & (marker - 1), width, unknown};
}

std::optional<Element> ReadElement(ContainerInput& input, std::optional<std::uint64_t> end) {
  if (!end) end = input.Size();
  const auto position = input.Position();
  if (end && position > *end) Fail(BadData, "EBML input exceeds its parent boundary");
  const auto id = PullVint(input, true, end);
  if (!id) {
    if (end && position != *end) Fail(BadData, "Truncated EBML element");
    return {};
  }
  const auto size = PullVint(input, false, end);
  if (!size) Fail(BadData, "Missing EBML element size");
  const auto data = input.Position();
  if (size->unknown) {
    if (id->value != Segment && id->value != Cluster) Fail(BadData, "Unexpected unknown-sized EBML element");
    return Element{static_cast<std::uint32_t>(id->value), position, data, {}};
  }
  const auto finish = AddOffset(data, size->value);
  if (end && finish > *end) Fail(BadData, "EBML element exceeds its parent boundary");
  return Element{static_cast<std::uint32_t>(id->value), position, data, finish};
}

Field ReadField(ByteView& bytes) {
  const auto id = ReadVint(bytes, true);
  const auto size = ReadVint(bytes);
  if (size.unknown || size.value > bytes.size()) Fail(BadData, "Invalid nested EBML element size");
  const auto field = bytes.first(static_cast<std::size_t>(size.value));
  bytes = bytes.subspan(field.size());
  return {static_cast<std::uint32_t>(id.value), field};
}

void SkipElement(ContainerInput& input, const Element& element) {
  if (!element.end) Fail(BadData, "Cannot skip an unknown-sized EBML element");
  if (input.Random()) {
    input.Seek(*element.end);
  } else {
    const auto size = *element.end - input.Position();
    if (size > MaxContainerMetadata) Fail(ContainerErrorCode::LimitExceeded, "EBML skip exceeds input budget");
    input.Skip(size);
  }
}

Bytes ReadBody(ContainerInput& input, const Element& element, std::size_t limit) {
  if (!element.end) Fail(BadData, "EBML element has no finite payload");
  const auto size = *element.end - element.data;
  if (size > limit) Fail(ContainerErrorCode::LimitExceeded, "EBML element exceeds its payload budget");
  return input.ReadBytes(static_cast<std::size_t>(size));
}

bool SegmentChild(std::uint32_t id) {
  return id == SeekHead || id == Info || id == Tracks || id == Cluster || id == Cues ||
         id == 0x1941a469 || id == 0x1043a770 || id == 0x1254c367;
}

std::uint64_t UInt(ByteView data) {
  if (data.size() > 8) Fail(BadData, "EBML integer exceeds 64 bits");
  std::uint64_t value = 0;
  for (auto byte : data) value = (value << 8) | std::to_integer<unsigned>(byte);
  return value;
}

std::int64_t SInt(ByteView data) {
  auto bits = UInt(data);
  if (!data.empty() && data.size() < 8 && (std::to_integer<unsigned>(data.front()) & 128))
    bits |= std::numeric_limits<std::uint64_t>::max() << (8 * data.size());
  return std::bit_cast<std::int64_t>(bits);
}

double Float(ByteView data) {
  double value = 0;
  if (data.size() == 4) value = std::bit_cast<float>(Number<std::uint32_t>(data));
  else if (data.size() == 8) value = std::bit_cast<double>(Number<std::uint64_t>(data));
  else if (!data.empty()) Fail(BadData, "Invalid EBML float size");
  if (!std::isfinite(value)) Fail(BadData, "Nonfinite EBML float");
  return value;
}

std::string_view Text(ByteView data) {
  if (data.empty()) return {};
  return {reinterpret_cast<const char*>(data.data()), data.size()};
}

std::int64_t SignedTime(std::uint64_t value) {
  if (value > std::numeric_limits<std::int64_t>::max()) Fail(ContainerErrorCode::LimitExceeded, "EBML time exceeds signed range");
  return static_cast<std::int64_t>(value);
}

std::int64_t ScaleTime(std::int64_t ticks, std::uint64_t scale) {
  const auto factor = SignedTime(scale);
  if (!factor || ticks > std::numeric_limits<std::int64_t>::max() / factor ||
      ticks < std::numeric_limits<std::int64_t>::min() / factor)
    Fail(ContainerErrorCode::LimitExceeded, "Matroska timestamp exceeds the nanosecond timeline");
  return ticks * factor;
}

std::int64_t ToNs(MediaTimestamp time) {
  const auto result = time.Rescale(Nanoseconds);
  if (!result) Fail(ContainerErrorCode::LimitExceeded, "Timestamp cannot be represented in nanoseconds");
  return result->ticks;
}

std::optional<MediaTimestamp> CodecDuration(const MediaTrack& track, ByteView packet, ContainerErrorCode error) {
  if (track.codec == CodecId::Aac) return MediaTimestamp{1024, *track.sample_rate};
  if (track.codec == CodecId::Opus) return MediaTimestamp{OpusPacketSamples(packet, error), 48000};
  return {};
}

void PutVint(Bytes& out, std::uint64_t value) {
  unsigned width = 1;
  while (width <= 8 && value >= (std::uint64_t{1} << (7 * width)) - 1) ++width;
  if (width > 8) Fail(ContainerErrorCode::LimitExceeded, "EBML variable integer exceeds 56 bits");
  PutNumber(out, value | (std::uint64_t{1} << (7 * width)), width);
}

void PutId(Bytes& out, std::uint32_t id) {
  PutNumber(out, id, (std::bit_width(id) + 7) / 8);
}

void PutElement(Bytes& out, std::uint32_t id, ByteView bytes) {
  PutId(out, id);
  PutVint(out, bytes.size());
  Append(out, bytes);
}

void PutUInt(Bytes& out, std::uint32_t id, std::uint64_t value) {
  const auto width = std::max<unsigned>(1, (std::bit_width(value) + 7) / 8);
  PutId(out, id);
  PutVint(out, width);
  PutNumber(out, value, width);
}

void PutInt(Bytes& out, std::uint32_t id, std::int64_t value) {
  PutId(out, id);
  PutVint(out, 8);
  Put(out, value);
}

void PutFloat(Bytes& out, std::uint32_t id, double value) {
  PutId(out, id);
  PutVint(out, 8);
  Put(out, std::bit_cast<std::uint64_t>(value));
}

void PutText(Bytes& out, std::uint32_t id, std::string_view text) {
  PutElement(out, id, std::as_bytes(std::span(text.data(), text.size())));
}

} // namespace huxerui::media::detail::ebml
