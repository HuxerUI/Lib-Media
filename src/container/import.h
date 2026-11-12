#pragma once

#include "container_io.h"

namespace huxerui::media::detail {

inline constexpr std::size_t MaxImportBytes = 128 * 1024 * 1024;
Bytes ReadImportBytes(ContainerInput& input, std::size_t limit = MaxImportBytes);

class IndexedImport : public ContainerReader {
public:
  using ContainerReader::ContainerReader;
  std::optional<ContainerPacket> Read() override;
  std::optional<SeekPoint> Find(std::uint32_t id, MediaTimestamp time, SeekDirection direction, bool seek) override;
  std::optional<MediaPresentationRange> PresentationRange(std::size_t track) const override { return ranges_[track]; }

protected:
  void Add(ContainerPacket packet, std::optional<std::uint64_t> offset = {});
  void Complete();

private:
  struct Entry {
    ContainerPacket packet;
    std::optional<std::uint64_t> offset;
  };
  std::vector<Entry> entries_;
  std::array<std::vector<std::size_t>, MaxContainerTracks> anchors_;
  std::array<std::optional<MediaPresentationRange>, MaxContainerTracks> ranges_;
  std::array<std::size_t, MaxContainerTracks> seek_floors_{};
  std::size_t cursor_ = 0;
};

std::optional<CodecId> LooksLikeAnnexB(ByteView prefix);
bool LooksLikeMpegTs(ByteView prefix);
std::optional<ContainerFormat> LooksLikeSubtitle(ByteView prefix);
std::unique_ptr<ContainerReader> OpenAnnexBReader(ContainerInput input, CodecId codec);
std::unique_ptr<ContainerReader> OpenMpegTsReader(ContainerInput input);
std::unique_ptr<ContainerReader> OpenAviReader(ContainerInput input);
std::unique_ptr<ContainerReader> OpenSubtitleReader(ContainerInput input, ContainerFormat format);
std::unique_ptr<ContainerWriter> OpenSubtitleWriter(ContainerOutput output, std::span<const MediaTrack> tracks,
                                                    ContainerFormat format);
void ValidateSubtitle(std::span<const MediaTrack> tracks, ContainerFormat format);

} // namespace huxerui::media::detail
