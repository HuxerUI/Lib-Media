#include "import.h"

#include "codec/common.h"
#include "ebml.h"

#include <charconv>
#include <limits>

namespace huxerui::media::detail {
namespace {

constexpr auto Invalid = ContainerErrorCode::InvalidData;
constexpr auto Unsupported = ContainerErrorCode::Unsupported;
constexpr std::size_t TextLimit = 8 * 1024 * 1024;

std::string_view Line(std::string_view& text) {
  const auto end = text.find('\n');
  const auto line = text.substr(0, end);
  text.remove_prefix(end == std::string_view::npos ? text.size() : end + 1);
  return line;
}

bool WebVttHeader(std::string_view text) {
  return text == "WEBVTT" || text.starts_with("WEBVTT ") || text.starts_with("WEBVTT\t");
}

bool Decimal(std::string_view text) {
  return !text.empty() && std::ranges::all_of(text, [](char byte) { return byte >= '0' && byte <= '9'; });
}

std::string_view Trim(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
  return text;
}

std::string Normalize(ByteView bytes) {
  std::string text;
  text.reserve(bytes.size());
  std::size_t at = 0;
  if (ebml::Text(bytes).starts_with("\xef\xbb\xbf")) at = 3;
  while (at < bytes.size()) {
    const auto first = std::to_integer<unsigned>(bytes[at++]);
    if (!first) Fail(Invalid, "Subtitle text contains NUL");
    if (first == '\r') {
      if (at < bytes.size() && bytes[at] == std::byte{'\n'}) ++at;
      text += '\n';
    } else if (first < 128) text += static_cast<char>(first);
    else {
      const unsigned count = first >= 0xf0 ? 4 : first >= 0xe0 ? 3 : 2;
      if (first < 0xc2 || first > 0xf4 || bytes.size() - at < count - 1)
        Fail(Invalid, "Invalid UTF-8 in subtitle text");
      auto value = first & (0x7f >> count);
      text += static_cast<char>(first);
      for (unsigned i = 1; i < count; ++i) {
        const auto tail = std::to_integer<unsigned>(bytes[at++]);
        if ((tail & 0xc0) != 0x80) Fail(Invalid, "Invalid UTF-8 in subtitle text");
        value = (value << 6) | (tail & 63);
        text += static_cast<char>(tail);
      }
      if ((count == 3 && value < 0x800) || (count == 4 && value < 0x10000) ||
          (value >= 0xd800 && value <= 0xdfff) || value > 0x10ffff)
        Fail(Invalid, "Invalid UTF-8 in subtitle text");
    }
  }
  return text;
}

std::int64_t Timestamp(std::string_view text, bool web) {
  const auto fraction = text.rfind(web ? '.' : ',');
  if (fraction == std::string_view::npos || text.size() - fraction != 4)
    Fail(Invalid, "Subtitle timestamp requires milliseconds");
  const auto number = [](std::string_view token, std::size_t minimum) {
    unsigned value = 0;
    const auto result = std::from_chars(token.data(), token.data() + token.size(), value);
    if (token.size() < minimum || result.ec != std::errc{} || result.ptr != token.data() + token.size())
      Fail(Invalid, "Invalid subtitle timestamp number");
    return value;
  };
  const auto ms = number(text.substr(fraction + 1), 3);
  text = text.substr(0, fraction);
  const auto last = text.rfind(':');
  if (last == std::string_view::npos || text.size() - last != 3) Fail(Invalid, "Subtitle timestamp requires minutes and seconds");
  const auto seconds = number(text.substr(last + 1), 2);
  text = text.substr(0, last);
  const auto first = text.find(':');
  if ((first == std::string_view::npos && (!web || text.size() != 2)) ||
      (first != std::string_view::npos && text.size() - first != 3))
    Fail(Invalid, "Invalid subtitle hours or minutes");
  const auto hours = first == std::string_view::npos ? 0 : number(text.substr(0, first), 2);
  const auto minutes = number(first == std::string_view::npos ? text : text.substr(first + 1), 2);
  if (minutes >= 60 || seconds >= 60) Fail(Invalid, "Subtitle timestamp is out of range");
  return ((std::int64_t(hours) * 60 + minutes) * 60 + seconds) * 1000 + ms;
}

std::string TimeText(std::int64_t ticks, bool web) {
  if (ticks / 3600000 > std::numeric_limits<unsigned>::max()) Fail(Invalid, "Subtitle timestamp is out of range");
  const auto padded = [](std::int64_t value, std::size_t digits) {
    auto text = std::to_string(value);
    if (text.size() < digits) text.insert(0, digits - text.size(), '0');
    return text;
  };
  return padded(ticks / 3600000, 2) + ":" + padded(ticks / 60000 % 60, 2) + ":" +
      padded(ticks / 1000 % 60, 2) + (web ? "." : ",") + padded(ticks % 1000, 3);
}

std::string WebVttCueText(std::string_view text, std::int64_t start, std::int64_t length, bool import) {
  std::string result;
  while (!text.empty()) {
    const auto begin = text.find('<');
    if (begin == std::string_view::npos) break;
    result += text.substr(0, begin + 1);
    text.remove_prefix(begin + 1);
    const auto end = text.find('>');
    if (end == std::string_view::npos) break;
    const auto tag = text.substr(0, end);
    if (!tag.empty() && tag.front() >= '0' && tag.front() <= '9' &&
        tag.find(':') != std::string_view::npos && tag.find('.') != std::string_view::npos) {
      const auto time = Timestamp(tag, true);
      const auto relative = import ? SubTime(time, start) : time;
      if (relative <= 0 || relative >= length) Fail(Invalid, "WebVTT inline timestamp lies outside its cue");
      result += TimeText(import ? relative : AddTime(start, relative), true);
      result += '>';
      text.remove_prefix(end + 1);
    }
  }
  result += text;
  return result;
}

std::string_view Block(std::string_view& text) {
  while (!text.empty() && text.front() == '\n') text.remove_prefix(1);
  const auto end = text.find("\n\n");
  auto block = text.substr(0, end);
  text.remove_prefix(end == std::string_view::npos ? text.size() : end + 2);
  if (block.ends_with('\n')) block.remove_suffix(1);
  return block;
}

bool IsNote(std::string_view line) {
  return line == "NOTE" || line.starts_with("NOTE ") || line.starts_with("NOTE\t");
}

BufferReference KeepText(std::string_view text) {
  const auto bytes = std::as_bytes(std::span(text));
  return Own(Bytes(bytes.begin(), bytes.end()));
}

class SubtitleReader final : public IndexedImport {
public:
  SubtitleReader(ContainerInput source, ContainerFormat format) : IndexedImport(std::move(source), format) {
    const bool web = format == ContainerFormat::WebVtt;
    const auto content = Normalize(ReadImportBytes(input, TextLimit));
    std::string_view remaining = content;
    std::string header, notes;
    if (web) {
      const auto first = Block(remaining);
      auto lines = first;
      if (!WebVttHeader(Line(lines))) Fail(Invalid, "Missing WebVTT signature");
      header = first;
    }
    bool have_cue = false;
    std::int64_t last = 0;
    for (auto block = Block(remaining); !block.empty(); block = Block(remaining)) {
      auto payload = block;
      auto timing = Line(payload);
      if (web && (IsNote(timing) || timing == "STYLE" || timing == "REGION")) {
        if (have_cue && !IsNote(timing)) Fail(Invalid, "WebVTT style or region appears after a cue");
        auto& metadata = have_cue ? notes : header;
        if (!metadata.empty()) metadata += "\n\n";
        metadata += block;
        continue;
      }
      std::string_view identifier;
      if (timing.find("-->") == std::string_view::npos) {
        if (web) identifier = timing;
        else if (!Decimal(Trim(timing))) Fail(Invalid, "Invalid SubRip cue number");
        timing = Line(payload);
      }
      const auto arrow = timing.find("-->");
      if (arrow == std::string_view::npos) Fail(Invalid, "Missing subtitle cue separator");
      const auto first = Timestamp(Trim(timing.substr(0, arrow)), web);
      const auto tail = Trim(timing.substr(arrow + 3));
      const auto space = tail.find_first_of(" \t");
      const auto end = Timestamp(tail.substr(0, space), web);
      const auto settings = space == std::string_view::npos ? std::string_view{} : Trim(tail.substr(space));
      if (end <= first || (have_cue && first < last) || (!web && !settings.empty()))
        Fail(Unsupported, "Subtitle cues require ordered starts, positive duration, and supported timing fields");
      last = first;
      have_cue = true;
      const MediaTimestamp pts{first, 1000};
      ContainerPacket packet{1, web ? KeepText(WebVttCueText(payload, first, end - first, true)) : KeepText(payload),
          pts, pts, MediaTimestamp{end - first, 1000}, true};
      if (web && (!settings.empty() || !identifier.empty() || !notes.empty())) {
        Bytes additions;
        ebml::Master(additions, 0xa6, [&](Bytes& more) {
          ebml::PutUInt(more, 0xee, 1);
          ebml::PutText(more, 0xa5, std::string(settings) + "\n" + std::string(identifier) + "\n" + notes);
        });
        packet.matroska = ContainerPacket::MatroskaAttributes{false, Own(std::move(additions))};
      }
      notes.clear();
      Add(std::move(packet));
    }
    if (!notes.empty()) Fail(Unsupported, "Trailing WebVTT notes have no associated Matroska cue");
    if (header.size() > 65536) Fail(ContainerErrorCode::LimitExceeded, "WebVTT header exceeds 64 KiB");
    const auto bytes = std::as_bytes(std::span(header));
    tracks.push_back({.id = 1, .kind = TrackKind::Subtitle, .timescale = 1000,
        .descriptions = {{.framing = PacketFraming::ContainerSample, .initialization_data = Bytes(bytes.begin(), bytes.end())}},
        .matroska = MediaTrack::MatroskaAttributes{.subtitle_codec = web ? "S_TEXT/WEBVTT" : "S_TEXT/UTF8"}});
    Complete();
  }
};

std::string Additions(const ContainerPacket& packet) {
  if (!packet.matroska) return {};
  if (packet.matroska->invisible) Fail(Unsupported, "Subtitle text cannot represent an invisible cue");
  auto bytes = packet.matroska->block_additions.AsBytes();
  if (bytes.empty()) return {};
  const auto entry = ebml::ReadField(bytes);
  if (!bytes.empty() || entry.id != 0xa6) Fail(Unsupported, "Unsupported WebVTT BlockAdditions");
  auto fields = entry.data;
  std::string text;
  bool found = false;
  while (!fields.empty()) {
    const auto field = ebml::ReadField(fields);
    if (field.id == 0xee && ebml::UInt(field.data) == 1) continue;
    if (field.id != 0xa5 || found) Fail(Unsupported, "Unsupported WebVTT BlockMore fields");
    text = Normalize(field.data);
    found = true;
  }
  return text;
}

class SubtitleWriter final : public ContainerWriter {
public:
  SubtitleWriter(ContainerOutput output, std::span<const MediaTrack> tracks, bool web)
      : ContainerWriter(std::move(output), tracks), web_(web) {
    if (web_) {
      auto text = Normalize(tracks.front().descriptions.front().initialization_data);
      while (text.ends_with('\n')) text.pop_back();
      WriteText(text.empty() ? "WEBVTT\n\n" : text + "\n\n");
    }
  }

  bool SupportsMatroskaPacketAttributes() const override { return web_; }
  std::optional<std::uint32_t> Write(std::uint32_t, const ContainerPacket& packet) override {
    if (!packet.pts || !packet.duration) Fail(Invalid, "Subtitle cue has no start or duration");
    const auto first = ExactTicks(*packet.pts, 1000);
    const auto length = ExactTicks(*packet.duration, 1000);
    if (first < last_ || length <= 0) Fail(Invalid, "Subtitle cue timing is invalid");
    const auto end = AddTime(first, length);
    std::string text;
    std::string settings;
    if (web_) {
      const auto additions = Additions(packet);
      std::string_view fields = additions;
      settings = Line(fields);
      const auto identifier = Line(fields);
      if (!fields.empty()) text = std::string(fields) + "\n\n";
      if (!identifier.empty()) text += std::string(identifier) + "\n";
    } else text = std::to_string(count_ + 1) + "\n";
    auto payload = Normalize(packet.data.AsBytes());
    if (payload.find("\n\n") != std::string::npos || payload.starts_with('\n') || payload.ends_with('\n'))
      Fail(Unsupported, "Subtitle cue text contains an empty boundary line");
    if (web_) payload = WebVttCueText(payload, first, length, false);
    text += TimeText(first, web_) + " --> " + TimeText(end, web_);
    if (!settings.empty()) text += " " + settings;
    text += "\n" + payload + "\n\n";
    WriteText(text);
    last_ = first;
    ++count_;
    return {};
  }

  void Finish() override {
    if (!count_) Fail(Invalid, "Subtitle output has no cues");
  }

private:
  void WriteText(std::string_view text) {
    if (text.size() > TextLimit - output.Position()) Fail(ContainerErrorCode::LimitExceeded, "Subtitle output exceeds 8 MiB");
    output.Write(std::as_bytes(std::span(text)));
  }
  bool web_;
  std::int64_t last_ = 0;
  std::size_t count_ = 0;
};

} // namespace

std::optional<ContainerFormat> LooksLikeSubtitle(ByteView prefix) {
  auto text = ebml::Text(prefix);
  if (text.starts_with("\xef\xbb\xbf")) text.remove_prefix(3);
  auto first = Line(text);
  if (first.ends_with('\r')) first.remove_suffix(1);
  if (WebVttHeader(first)) return ContainerFormat::WebVtt;
  if (Decimal(Trim(first)) && Line(text).find("-->") != std::string_view::npos) return ContainerFormat::Srt;
  return {};
}

void ValidateSubtitle(std::span<const MediaTrack> tracks, ContainerFormat format) {
  if (tracks.size() != 1 || tracks.front().kind != TrackKind::Subtitle || !tracks.front().matroska ||
      tracks.front().descriptions.front().framing != PacketFraming::ContainerSample)
    Fail(Unsupported, "Standalone subtitles require one compatible subtitle track");
  const auto& track = tracks.front();
  const auto& description = track.descriptions.front();
  const bool web = format == ContainerFormat::WebVtt;
  if (track.matroska->subtitle_codec != (web ? "S_TEXT/WEBVTT" : "S_TEXT/UTF8") ||
      (!web && !description.initialization_data.empty()))
    Fail(Unsupported, "Subtitle track does not match the output text format");
  if (web && !description.initialization_data.empty()) {
    const auto header = Normalize(description.initialization_data);
    auto lines = std::string_view(header);
    if (!WebVttHeader(Line(lines))) Fail(Invalid, "Invalid WebVTT initialization header");
  }
}

std::unique_ptr<ContainerReader> OpenSubtitleReader(ContainerInput input, ContainerFormat format) {
  return std::make_unique<SubtitleReader>(std::move(input), format);
}
std::unique_ptr<ContainerWriter> OpenSubtitleWriter(ContainerOutput output, std::span<const MediaTrack> tracks,
                                                    ContainerFormat format) {
  return std::make_unique<SubtitleWriter>(std::move(output), tracks, format == ContainerFormat::WebVtt);
}

} // namespace huxerui::media::detail
