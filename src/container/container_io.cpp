#include "container_io.h"

#include <array>
#include <numeric>

namespace huxerui::media::detail {

[[noreturn]] void Fail(ContainerErrorCode code, std::string message) {
  throw ContainerError{code, std::move(message), {}};
}

ByteView AacLcInitialization(ByteView config) {
  // An explicit absent-SBR sync extension is equivalent to the two-byte AAC-LC description.
  if (config.size() == 5 && Number<std::uint16_t>(config, 2) == 0x56e5 && Number<std::uint8_t>(config, 4) == 0)
    return config.first(2);
  return config;
}

OpusHead ParseOpus(ByteView bytes, ContainerErrorCode error) {
  if (bytes.size() != 19 || Number<std::uint64_t>(bytes) != 0x4f70757348656164ull ||
      Number<std::uint8_t>(bytes, 8) != 1 || Number<std::uint8_t>(bytes, 18) != 0)
    Fail(error, "Opus requires a version-one mapping-family-zero OpusHead");
  const auto channels = Number<std::uint8_t>(bytes, 9);
  if (channels != 1 && channels != 2) Fail(error, "Opus requires one or two channels");
  return {channels, Number<std::uint32_t>(bytes, 12, true), Number<std::uint16_t>(bytes, 10, true)};
}

unsigned OpusPacketSamples(ByteView packet, ContainerErrorCode error) {
  if (packet.empty()) Fail(error, "Empty Opus packet");
  const auto toc = Number<std::uint8_t>(packet);
  const auto configuration = toc >> 3;
  constexpr std::array silk{480, 960, 1920, 2880};
  constexpr std::array celt{120, 240, 480, 960};
  const auto length = configuration < 12 ? silk[configuration & 3] :
      (configuration < 16 ? silk[configuration & 1] : celt[configuration & 3]);
  unsigned frames = 1;
  if ((toc & 3) == 3) {
    if (packet.size() < 2) Fail(error, "Missing Opus frame count");
    frames = Number<std::uint8_t>(packet, 1) & 63;
  } else if (toc & 3) {
    frames = 2;
  }
  if (!frames || frames * length > 5760) Fail(error, "Opus packet duration exceeds 120 ms");
  return frames * length;
}

std::uint64_t AddOffset(std::uint64_t offset, std::uint64_t size) {
  if (size > std::numeric_limits<std::uint64_t>::max() - offset)
    Fail(ContainerErrorCode::InvalidData, "Container offset overflows");
  return offset + size;
}

std::int64_t AddTime(std::int64_t time, std::int64_t delta) {
  if ((delta > 0 && time > std::numeric_limits<std::int64_t>::max() - delta) ||
      (delta < 0 && time < std::numeric_limits<std::int64_t>::min() - delta))
    Fail(ContainerErrorCode::LimitExceeded, "Container time overflows");
  return time + delta;
}

std::int64_t SubTime(std::int64_t time, std::int64_t start) {
  if ((start > 0 && time < std::numeric_limits<std::int64_t>::min() + start) ||
      (start < 0 && time > std::numeric_limits<std::int64_t>::max() + start))
    Fail(ContainerErrorCode::LimitExceeded, "Container time overflows");
  return time - start;
}

std::uint32_t CommonScale(std::uint32_t first, std::uint32_t second) {
  if (!first || !second) Fail(ContainerErrorCode::InvalidData, "Zero container timescale");
  auto scale = static_cast<std::uint64_t>(first / std::gcd(first, second)) * second;
  if (scale > std::numeric_limits<std::uint32_t>::max())
    Fail(ContainerErrorCode::Unsupported, "Container clocks have no representable exact common scale");
  return static_cast<std::uint32_t>(scale);
}

std::int64_t ExactTicks(MediaTimestamp time, std::uint32_t scale) {
  auto scaled = time.Rescale(scale);
  if (!scaled || scaled->Compare(time) != 0)
    Fail(ContainerErrorCode::InvalidArgument, "Timestamp is outside the track's exact tick grid");
  return scaled->ticks;
}

std::uint32_t PacketTrack(std::span<const MediaTrack> tracks, std::uint32_t id) {
  auto track = std::ranges::find(tracks, id, &MediaTrack::id);
  if (track == tracks.end()) Fail(ContainerErrorCode::InvalidArgument, "Unknown container track ID");
  return static_cast<std::uint32_t>(track - tracks.begin());
}

ContainerInput::ContainerInput(RandomAccessStream input) : stream_(std::move(input)) {
  size_ = UnwrapIo(std::get<RandomAccessStream>(stream_).Size());
}

std::size_t ContainerInput::Pull(std::uint64_t offset, std::span<std::byte> bytes) {
  if (auto* input = std::get_if<RandomAccessStream>(&stream_)) return UnwrapIo(input->ReadAt(offset, bytes));
  return UnwrapIo(std::get<InputStream>(stream_).Read(bytes));
}

ByteView ContainerInput::Peek(std::size_t count) {
  // Replay probe bytes on both kinds of stream so format selection never consumes input.
  const auto previous = prefix_.size();
  if (previous >= count) return ByteView(prefix_).first(count);
  prefix_.resize(count);
  auto filled = previous;
  while (filled < count) {
    auto size = Pull(AddOffset(position_, filled), std::span(prefix_).subspan(filled));
    if (!size) break;
    filled += size;
  }
  prefix_.resize(filled);
  return prefix_;
}

bool ContainerInput::ReadExact(std::span<std::byte> bytes) {
  auto filled = std::min(prefix_.size(), bytes.size());
  std::copy_n(prefix_.begin(), filled, bytes.begin());
  prefix_.erase(prefix_.begin(), prefix_.begin() + filled);
  position_ = AddOffset(position_, filled);
  while (filled < bytes.size()) {
    const auto count = Pull(position_, bytes.subspan(filled));
    if (!count) {
      if (!filled) return false;
      Fail(ContainerErrorCode::InvalidData, "Truncated container data");
    }
    position_ = AddOffset(position_, count);
    filled += count;
  }
  return true;
}

Bytes ContainerInput::ReadBytes(std::size_t count) {
  Bytes bytes(count);
  if (!ReadExact(bytes)) Fail(ContainerErrorCode::InvalidData, "Unexpected end of container");
  return bytes;
}

void ContainerInput::Skip(std::uint64_t count) {
  if (Random() && size_) {
    Seek(AddOffset(position_, count));
    return;
  }
  std::array<std::byte, 4096> scratch;
  while (count) {
    auto step = static_cast<std::size_t>(std::min<std::uint64_t>(scratch.size(), count));
    if (!ReadExact(std::span(scratch).first(step))) Fail(ContainerErrorCode::InvalidData, "Truncated container chunk");
    count -= step;
  }
}

void ContainerInput::Seek(std::uint64_t offset) {
  if (!Random()) Fail(ContainerErrorCode::NeedRandomAccess, "Container input cannot seek");
  if (size_ && offset > *size_) Fail(ContainerErrorCode::InvalidData, "Container offset exceeds file size");
  prefix_.clear();
  position_ = offset;
}

void ContainerInput::Close() {
  if (auto* input = std::get_if<RandomAccessStream>(&stream_)) UnwrapIo(input->Close());
}

ContainerOutput::ContainerOutput(RandomAccessStream output) : stream_(std::move(output)) {
  auto& file = std::get<RandomAccessStream>(stream_);
  if (UnwrapIo(file.Size()) != 0) Fail(ContainerErrorCode::InvalidArgument, "Random-access output must be empty");
  UnwrapIo(file.WriteAt(0, {}));
}

void ContainerOutput::Write(ByteView bytes) {
  const auto end = AddOffset(position_, bytes.size());
  if (auto* output = std::get_if<RandomAccessStream>(&stream_)) {
    UnwrapIo(output->WriteAt(position_, bytes));
  } else {
    UnwrapIo(std::get<OutputStream>(stream_).Write(bytes));
  }
  position_ = end;
}

void ContainerOutput::Patch(std::uint64_t offset, ByteView bytes) {
  if (!Random()) Fail(ContainerErrorCode::NeedRandomAccess, "Container output cannot backpatch");
  UnwrapIo(std::get<RandomAccessStream>(stream_).WriteAt(offset, bytes));
}

void ContainerOutput::RequireReadable() {
  std::array<std::byte, 1> probe;
  try {
    UnwrapIo(std::get<RandomAccessStream>(stream_).ReadAt(0, probe));
  } catch (const std::logic_error&) {
    Fail(ContainerErrorCode::InvalidArgument, "Fast-start output requires a read/write stream");
  }
}

void ContainerOutput::Insert(std::uint64_t offset, ByteView bytes) {
  auto& stream = std::get<RandomAccessStream>(stream_);
  const auto end = AddOffset(position_, bytes.size());
  std::array<std::byte, 64 * 1024> buffer;
  auto remaining = position_ - offset;
  // Move the tail backwards so an overlapping write cannot overwrite unread bytes.
  while (remaining) {
    auto chunk = std::span(buffer).first(std::min<std::uint64_t>(buffer.size(), remaining));
    remaining -= chunk.size();
    const auto source = offset + remaining;
    for (std::size_t read = 0; read < chunk.size();) {
      const auto count = UnwrapIo(stream.ReadAt(source + read, chunk.subspan(read)));
      if (!count) Fail(ContainerErrorCode::InvalidData, "Container output ended during relocation");
      read += count;
    }
    Patch(source + bytes.size(), chunk);
  }
  Patch(offset, bytes);
  position_ = end;
}

void ContainerOutput::Close() {
  std::visit([](auto& output) { UnwrapIo(output.Close()); }, stream_);
}

std::optional<SeekPoint> ContainerReader::Find(std::uint32_t, MediaTimestamp, SeekDirection, bool seek) {
  if (seek && !input.Random()) Fail(ContainerErrorCode::NeedRandomAccess, "Container input cannot seek");
  Fail(ContainerErrorCode::IndexUnavailable, "This layout has no covered seek index");
}

} // namespace huxerui::media::detail
