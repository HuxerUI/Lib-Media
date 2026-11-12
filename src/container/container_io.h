#pragma once

#include <huxerui/media_container.h>

#include <algorithm>
#include <array>
#include <bit>
#include <limits>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace huxerui::media::detail {

inline constexpr std::size_t MaxContainerTracks = 8;
inline constexpr std::size_t MaxContainerPacket = 16 * 1024 * 1024;
inline constexpr std::size_t MaxContainerMetadata = 64 * 1024 * 1024;
inline constexpr std::size_t MaxContainerSamples = 1000000;

[[noreturn]] void Fail(ContainerErrorCode code, std::string message);

template<class T> T UnwrapIo(IoResult<T> result) {
  if (!result.Succeeded()) {
    auto cause = std::move(result).Error();
    throw ContainerError{ContainerErrorCode::Io, cause.message, std::move(cause)};
  }
  return std::move(result).Value();
}

template<class T, class Operation> ContainerResult<T> ContainerCall(Operation operation) {
  try {
    if constexpr (std::is_void_v<T>) {
      operation();
      return ContainerResult<void>::Success();
    } else {
      return ContainerResult<T>::Success(operation());
    }
  } catch (ContainerError& error) {
    return ContainerResult<T>::Failure(std::move(error));
  } catch (const std::logic_error& error) {
    return ContainerResult<T>::Failure({ContainerErrorCode::InvalidState, error.what(), {}});
  }
}

class ContainerInput {
public:
  explicit ContainerInput(InputStream input) : stream_(std::move(input)) {}
  explicit ContainerInput(RandomAccessStream input);

  bool Random() const { return std::holds_alternative<RandomAccessStream>(stream_); }
  std::optional<std::uint64_t> Size() const { return size_; }
  void RefreshSize();
  std::uint64_t Position() const { return position_; }
  std::span<const std::byte> Peek(std::size_t count);
  bool ReadExact(std::span<std::byte> bytes);
  Bytes ReadBytes(std::size_t count);
  void Skip(std::uint64_t count);
  void Seek(std::uint64_t offset);
  void Close();

private:
  std::size_t Pull(std::uint64_t offset, std::span<std::byte> bytes);

  std::variant<InputStream, RandomAccessStream> stream_;
  std::optional<std::uint64_t> size_;
  std::uint64_t position_ = 0;
  Bytes prefix_;
};

class ContainerOutput {
public:
  explicit ContainerOutput(OutputStream output) : stream_(std::move(output)) {}
  explicit ContainerOutput(RandomAccessStream output);

  bool Random() const { return std::holds_alternative<RandomAccessStream>(stream_); }
  std::uint64_t Position() const { return position_; }
  void Write(std::span<const std::byte> bytes);
  void Patch(std::uint64_t offset, std::span<const std::byte> bytes);
  void RequireReadable();
  void Insert(std::uint64_t offset, std::span<const std::byte> bytes);
  void Close();

private:
  std::variant<OutputStream, RandomAccessStream> stream_;
  std::uint64_t position_ = 0;
};

using ByteView = std::span<const std::byte>;

constexpr std::uint32_t Tag(std::string_view name) {
  std::uint32_t value = 0;
  for (const unsigned char ch : name) value = (value << 8) | ch;
  return value;
}

inline ByteView Slice(ByteView bytes, std::size_t offset, std::size_t size) {
  if (offset > bytes.size() || size > bytes.size() - offset)
    Fail(ContainerErrorCode::InvalidData, "Truncated container field");
  return bytes.subspan(offset, size);
}

template<class T> T Number(ByteView bytes, std::size_t offset = 0, bool little = false) {
  auto field = Slice(bytes, offset, sizeof(T));
  std::uint64_t value = 0;
  for (std::size_t i = 0; i < sizeof(T); ++i) {
    value = (value << 8) | std::to_integer<unsigned>(field[little ? sizeof(T) - 1 - i : i]);
  }
  return std::bit_cast<T>(static_cast<std::make_unsigned_t<T>>(value));
}

template<class T> void Put(Bytes& bytes, T value, bool little = false) {
  auto bits = static_cast<std::make_unsigned_t<T>>(value);
  for (std::size_t i = 0; i < sizeof(T); ++i) {
    auto shift = 8 * (little ? i : sizeof(T) - 1 - i);
    bytes.push_back(static_cast<std::byte>((bits >> shift) & 255));
  }
}

inline void Append(Bytes& bytes, ByteView tail) { bytes.insert(bytes.end(), tail.begin(), tail.end()); }

std::uint32_t VorbisSpeakerMask(unsigned channels);
unsigned WaveToVorbisChannel(unsigned channel, unsigned channels);

struct OpusHead {
  std::uint32_t channels;
  std::uint32_t input_rate;
  std::uint16_t pre_skip;
  unsigned family = 0, streams = 1, coupled = 0;
  std::array<unsigned char, 8> mapping{};
  std::int64_t Delay() const { return static_cast<std::int64_t>(pre_skip) * 1'000'000'000 / 48000; }
};

OpusHead ParseOpus(ByteView bytes, ContainerErrorCode error);

unsigned OpusPacketSamples(ByteView packet, ContainerErrorCode error);

struct AlacConfig {
  std::uint32_t frame_length, sample_rate;
  unsigned channels, bits;
};

AlacConfig ParseAlac(ByteView config, ContainerErrorCode error);
unsigned AlacPacketSamples(ByteView bytes, const AlacConfig& config, ContainerErrorCode error);

struct FlacStreamInfo {
  unsigned min_block, max_block, max_frame, rate, channels, bits;
  std::uint64_t total;
};

FlacStreamInfo ParseFlacStreamInfo(ByteView bytes, ContainerErrorCode error);
unsigned FlacPacketSamples(ByteView bytes, const FlacStreamInfo& info);
AudioFrame DecodeMultichannelFlac(ByteView bytes, const FlacStreamInfo& info, AudioFormat format, MediaTimestamp pts);

struct Mp3FrameHeader {
  unsigned version, rate, channels, samples, length, side_offset, side_size;
};

std::optional<Mp3FrameHeader> ParseMp3FrameHeader(ByteView bytes);

MediaSampleDescription DescribeAdts(ByteView header);
ByteView AacLcInitialization(ByteView config);
std::uint64_t AddOffset(std::uint64_t offset, std::uint64_t size);
std::int64_t AddTime(std::int64_t time, std::int64_t delta);
std::int64_t SubTime(std::int64_t time, std::int64_t start);
std::uint32_t CommonScale(std::uint32_t first, std::uint32_t second);
std::int64_t ExactTicks(MediaTimestamp time, std::uint32_t scale);
std::uint32_t PacketTrack(std::span<const MediaTrack> tracks, std::uint32_t id);

template<class Visit>
std::int64_t VisitEdits(std::span<const MediaTrack::Edit> edits, std::uint32_t scale,
                        ContainerErrorCode error, Visit visit) {
  std::int64_t begin = 0, previous_media_end = 0;
  for (const auto& edit : edits) {
    const auto length = ExactTicks(edit.duration, scale);
    if (length <= 0) Fail(error, "Presentation edit duration must be positive");
    const auto end = AddTime(begin, length);
    if (edit.media_start) {
      const auto media = ExactTicks(*edit.media_start, scale);
      if (media < previous_media_end) Fail(error, "Media edits must be forward and nonoverlapping");
      previous_media_end = AddTime(media, length);
      visit(begin, end, media, previous_media_end);
    }
    begin = end;
  }
  return begin;
}

class ContainerReader {
public:
  ContainerReader(ContainerInput input, ContainerFormat format) : input(std::move(input)), format(format) {}
  virtual ~ContainerReader() = default;
  virtual std::optional<ContainerPacket> Read() = 0;
  virtual std::optional<SeekPoint> Find(std::uint32_t id, MediaTimestamp time, SeekDirection direction, bool seek);
  virtual std::optional<MediaPresentationRange> PresentationRange(std::size_t) const { return {}; }
  virtual std::optional<std::chrono::nanoseconds> EbmlTimestampScale() const { return {}; }
  virtual MatroskaMetadata ReadMatroskaMetadata();

  std::uint32_t RegisterAudioDescription(MediaTrack& track, MediaSampleDescription description);

  ContainerInput input;
  ContainerFormat format;
  ContainerLayout layout = ContainerLayout::Default;
  std::vector<MediaTrack> tracks;
  std::optional<MediaAudioMetadata> audio_metadata;
  std::optional<MediaTimestamp> start;
  std::optional<MediaTimestamp> duration;
  std::vector<std::uint32_t> selected;
  std::array<std::uint32_t, MaxContainerTracks> active_descriptions{};
  std::array<std::optional<MediaTimestamp>, MaxContainerTracks> first_audible_pts;
  std::optional<ContainerPacket> pending;
  bool failed = false;
};

class ContainerWriter {
public:
  ContainerWriter(ContainerOutput output, std::span<const MediaTrack> tracks)
      : output(std::move(output)), tracks(tracks.begin(), tracks.end()), ended(tracks.size()) {}
  virtual ~ContainerWriter() = default;
  virtual std::optional<std::uint32_t> Write(std::uint32_t track, const ContainerPacket& packet) = 0;
  virtual void TrackEnded(std::uint32_t) {}
  virtual bool SupportsTrim() const { return false; }
  virtual bool SupportsPacketTrim() const { return false; }
  virtual bool SupportsMatroskaPacketAttributes() const { return false; }
  virtual void Finish() = 0;

  ContainerOutput output;
  std::vector<MediaTrack> tracks;
  std::vector<bool> ended;
};

std::unique_ptr<ContainerReader> OpenWaveReader(ContainerInput input, ContainerFormat format);
bool LooksLikeWave64(ByteView prefix);
void ValidateFlac(std::span<const MediaTrack> tracks);
void ValidateMp3(std::span<const MediaTrack> tracks);
std::unique_ptr<ContainerWriter> OpenFlacWriter(ContainerOutput output, std::span<const MediaTrack> tracks,
                                               const std::optional<MediaAudioMetadata>& tags);
std::unique_ptr<ContainerWriter> OpenMp3Writer(ContainerOutput output, std::span<const MediaTrack> tracks,
                                              const std::optional<MediaAudioMetadata>& tags);
std::unique_ptr<ContainerWriter> OpenWaveWriter(ContainerOutput output, ContainerFormat format,
                                                std::span<const MediaTrack> tracks);
void ValidateWave(std::span<const MediaTrack> tracks);
std::unique_ptr<ContainerReader> OpenAiffReader(ContainerInput input);
std::unique_ptr<ContainerWriter> OpenAiffWriter(ContainerOutput output, std::span<const MediaTrack> tracks);
void ValidateAiff(std::span<const MediaTrack> tracks);
std::unique_ptr<ContainerReader> OpenCafReader(ContainerInput input);
std::unique_ptr<ContainerWriter> OpenCafWriter(ContainerOutput output, std::span<const MediaTrack> tracks);
void ValidateCaf(std::span<const MediaTrack> tracks);
std::unique_ptr<ContainerReader> OpenAdtsReader(ContainerInput input);
std::unique_ptr<ContainerWriter> OpenAdtsWriter(ContainerOutput output, std::span<const MediaTrack> tracks);
void ValidateAdts(std::span<const MediaTrack> tracks);
std::unique_ptr<ContainerReader> OpenOggReader(ContainerInput input);
std::unique_ptr<ContainerWriter> OpenOggWriter(ContainerOutput output, std::span<const MediaTrack> tracks);
void ValidateOgg(std::span<const MediaTrack> tracks);
std::unique_ptr<ContainerReader> OpenFlacReader(ContainerInput input);
std::unique_ptr<ContainerReader> OpenMp3Reader(ContainerInput input);
bool LooksLikeMp3(ByteView bytes);
std::unique_ptr<ContainerReader> OpenMp4Reader(ContainerInput input, std::optional<ContainerFormat> hint,
                                               ContainerInputMode mode);
std::unique_ptr<ContainerWriter> OpenMp4Writer(ContainerOutput output, ContainerFormat format,
                                               std::span<const MediaTrack> tracks, const MediaMuxerOptions& options);
void ValidateMp4(ContainerFormat format, std::span<const MediaTrack> tracks, const MediaMuxerOptions& options);
std::unique_ptr<ContainerReader> OpenMatroskaReader(ContainerInput input, std::optional<ContainerFormat> hint);
std::unique_ptr<ContainerWriter> OpenMatroskaWriter(ContainerOutput output, ContainerFormat format,
                                                    std::span<const MediaTrack> tracks, const MediaMuxerOptions& options);
void ValidateMatroska(ContainerFormat format, std::span<const MediaTrack> tracks, const MediaMuxerOptions& options);

} // namespace huxerui::media::detail
