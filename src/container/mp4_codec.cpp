#include "mp4.h"
#include "../codec/av1.h"
#include "../codec/hevc.h"

namespace huxerui::media::detail {

Mp4EditTimeline Mp4Edits(std::span<const MediaTrack::Edit> edits, std::uint32_t movie_scale,
                         std::uint32_t media_scale, ContainerErrorCode error) {
  if (edits.size() > MaxMp4Edits) Fail(ContainerErrorCode::LimitExceeded, "MP4 edit list exceeds its limit");
  Mp4EditTimeline timeline{CommonScale(movie_scale, media_scale), {}};
  for (const auto& edit : edits) {
    ExactTicks(edit.duration, movie_scale);
    if (edit.media_start) ExactTicks(*edit.media_start, media_scale);
  }
  timeline.duration = VisitEdits(edits, timeline.scale, error, [&](auto begin, auto end, auto media, auto) {
    timeline.segments.push_back({begin, end, media});
  });
  if (timeline.segments.empty()) Fail(error, "MP4 edit list has no media interval");
  return timeline;
}

bool Mp4VideoExtensionTag(std::uint32_t tag) {
  switch (tag) {
    case Tag("colr"):
    case Tag("mdcv"):
    case Tag("clli"):
    case Tag("amve"):
    case Tag("dvcC"):
    case Tag("dvvC"): return true;
    default: return false;
  }
}

void ValidateMp4VideoExtension(std::uint32_t tag, ByteView payload, CodecId codec, ContainerErrorCode error) {
  if (tag == Tag("colr") && payload.size() >= 4 && payload.size() <= 65536) {
    const auto type = Number<std::uint32_t>(payload);
    if ((type == Tag("nclc") && payload.size() == 10) ||
        (type == Tag("nclx") && payload.size() == 11) ||
        ((type == Tag("prof") || type == Tag("rICC")) && payload.size() > 4)) return;
  } else if ((tag == Tag("mdcv") && payload.size() == 24) ||
             (tag == Tag("clli") && payload.size() == 4) ||
             (tag == Tag("amve") && payload.size() == 8) ||
             ((tag == Tag("dvcC") || tag == Tag("dvvC")) && codec == CodecId::Hevc &&
              payload.size() >= 4 && payload.size() <= 256)) {
    return;
  }
  Fail(error, "Unsupported or malformed MP4 visual extension");
}

unsigned HevcNalLength(ByteView config, bool hvc1, ContainerErrorCode error) {
  try {
    return ParseHevcConfiguration(config, hvc1, false).length;
  } catch (const std::invalid_argument& failure) {
    Fail(error, failure.what());
  }
}

bool HevcIdr(ByteView bytes, unsigned length_size, bool hvc1, ContainerErrorCode error) {
  try {
    return IsHevcIdr(bytes, length_size, hvc1, false);
  } catch (const std::invalid_argument& failure) {
    Fail(error, failure.what());
  }
}

void ValidateAv1Configuration(ByteView config, ContainerErrorCode error) {
  try {
    ParseAv1Configuration(config);
  } catch (const std::invalid_argument& failure) {
    Fail(error, failure.what());
  }
}

bool Av1Sync(ByteView bytes, ContainerErrorCode error) {
  try {
    return InspectAv1Packet(bytes).sync;
  } catch (const std::invalid_argument& failure) {
    Fail(error, failure.what());
  }
}

bool ProResEntry(std::uint32_t tag) {
  switch (tag) {
    case Tag("apco"):
    case Tag("apcs"):
    case Tag("apcn"):
    case Tag("apch"):
    case Tag("ap4h"):
    case Tag("ap4x"): return true;
    default: return false;
  }
}

void ValidateMovDataDescription(std::string_view entry, ByteView data, ContainerErrorCode error) {
  if (data.size() > 65536)
    Fail(ContainerErrorCode::LimitExceeded, "QuickTime data description exceeds 64 KiB");
  if (entry == "tmcd") {
    if (data.size() < 18 || !Number<std::uint32_t>(data, 8) || !Number<std::uint32_t>(data, 12) ||
        !Number<std::uint8_t>(data, 16))
      Fail(error, "Invalid QuickTime timecode description");
  } else {
    if (entry != "mebx") Fail(error, "Unsupported QuickTime data sample entry");
    bool keys = false;
    while (!data.empty()) {
      if (data.size() < 8) Fail(error, "Truncated QuickTime timed metadata description");
      const auto size = Number<std::uint32_t>(data);
      if (size < 8 || size > data.size()) Fail(error, "Invalid QuickTime timed metadata description size");
      if (Number<std::uint32_t>(data, 4) == Tag("keys")) {
        if (keys || size == 8) Fail(error, "Invalid QuickTime timed metadata keys");
        keys = true;
      }
      data = data.subspan(size);
    }
    if (!keys) Fail(error, "QuickTime timed metadata is missing its key table");
  }
}

} // namespace huxerui::media::detail
