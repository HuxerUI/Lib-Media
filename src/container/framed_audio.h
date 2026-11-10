#pragma once

#include "container_io.h"

#include <array>

namespace huxerui::media::detail {

struct AudioPacket {
  Bytes bytes;
  unsigned samples;
  bool restartable = true;
};

struct AudioTag {
  std::string_view flac, id3;
  std::string MediaAudioMetadata::*field;
};

inline constexpr std::array AudioTags{
    AudioTag{"TITLE", "TIT2", &MediaAudioMetadata::title},
    AudioTag{"ARTIST", "TPE1", &MediaAudioMetadata::artist},
    AudioTag{"ALBUM", "TALB", &MediaAudioMetadata::album},
    AudioTag{"TRACKNUMBER", "TRCK", &MediaAudioMetadata::track_number},
    AudioTag{"GENRE", "TCON", &MediaAudioMetadata::genre},
    AudioTag{"DATE", "TDRC", &MediaAudioMetadata::date}};
inline constexpr std::size_t MaxAudioText = 64 * 1024;

bool HasAudioMetadata(const MediaAudioMetadata& metadata);
bool IsCoverMime(std::string_view mime);
void ValidateAudioMetadata(const MediaAudioMetadata& metadata);
void ValidateAudioPacketTime(const ContainerPacket& packet, std::int64_t start, unsigned samples, unsigned rate);

// FLAC and MPEG audio frames carry their sample count and are stored in timeline order.
class FramedAudioReader : public ContainerReader {
public:
  FramedAudioReader(ContainerInput input, ContainerFormat format) : ContainerReader(std::move(input), format) {}

  std::optional<DemuxItem> Read() override;
  std::optional<SeekPoint> Find(std::uint32_t, MediaTimestamp target, SeekDirection direction, bool seek) override;
  std::optional<MediaPresentationRange> PresentationRange(std::size_t) const override;

protected:
  virtual std::optional<AudioPacket> ReadFrame(std::int64_t position) = 0;
  void Begin(MediaTrack track, std::optional<std::uint64_t> coded_samples = {});
  std::uint64_t FirstFrameOffset() const { return first_; }
  void SeekFrame(std::uint64_t offset, std::int64_t samples);

private:
  struct Entry {
    std::uint64_t offset;
    std::int64_t samples;
    bool restartable;
  };

  void BuildIndex();
  void CheckEnd(std::int64_t samples);
  MediaTimestamp At(std::int64_t samples) const {
    return {samples - priming_, *tracks.front().sample_rate};
  }

  std::uint64_t first_ = 0;
  std::int64_t position_ = 0;
  std::int64_t priming_ = 0;
  std::int64_t padding_ = 0;
  std::optional<std::uint64_t> coded_samples_;
  std::vector<Entry> index_;
};

} // namespace huxerui::media::detail
