#include <huxerui/media_codec.h>
#include <huxerui/media_container.h>
#include <huxerui/file.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <thread>
#ifdef __APPLE__
#include <TargetConditionals.h>
#endif
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <huxerui/web/external_texture.h>
#endif

using namespace huxerui;
using namespace huxerui::media;
namespace {
void Check(bool value, const std::string& message) { if (!value) throw std::runtime_error(message); }
template<class T> void Require(const CodecResult<T>& value) {
  if (!value.Succeeded()) throw std::runtime_error(value.Error().message + " (" + value.Error().platform_code + ")");
}
template<class F> void Eventually(F work, const char* message) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
  do {
    if (work()) return;
#ifdef __EMSCRIPTEN__
    emscripten_sleep(1);
#else
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
#endif
  } while (std::chrono::steady_clock::now() < deadline);
  throw std::runtime_error(message);
}
VideoEncoderConfig Config(H264Profile profile, std::optional<std::uint32_t> bframes, VideoPixelFormat layout = VideoPixelFormat::NV12,
    std::uint32_t height = 240, std::uint32_t width = 320) {
  VideoEncoderConfig config;
  config.input = {.width = width, .height = height, .visible = {0, 0, width, height}, .pixel_format = layout};
  config.profile = profile; config.max_b_frames = bframes; config.key_frame_interval = 8; config.bitrate = 2'000'000;
  config.frame_rate_numerator = 30000; config.frame_rate_denominator = 1001;
  return config;
}
MediaTimestamp PictureTime(unsigned index, bool variable) {
  return {90000 + std::int64_t(index) * 3003 + (variable && index % 2 ? 900 : 0), 90000};
}
VideoFrame Frame(const VideoEncoderConfig& config, unsigned index, bool variable) {
  const auto width = config.input.width, height = config.input.height;
  const bool ten_bit = config.input.pixel_format == VideoPixelFormat::P010;
  const auto component = ten_bit ? 2u : 1u;
  const auto stride = (width + 16) * component;
  auto storage = std::make_shared<Bytes>(std::size_t(stride) * height * 3 / 2, std::byte{255});
  BufferReference owner(*storage, storage);
  const auto luma = std::size_t(stride) * height;
  const auto pts = PictureTime(index, variable);
  const MediaTimestamp duration{PictureTime(index + 1, variable).ticks - pts.ticks, 90000};
  VideoFrame frame{config.input, pts, duration,
      {{owner.Slice(0, luma), stride, component}}};
  if (config.input.pixel_format != VideoPixelFormat::I420)
    frame.planes.push_back({owner.Slice(luma, luma / 2), stride, component * 2});
  else {
    frame.planes.push_back({owner.Slice(luma, luma / 4), stride / 2, 1});
    frame.planes.push_back({owner.Slice(luma + luma / 4, luma / 4), stride / 2, 1});
  }
  auto write = [&](std::size_t offset, unsigned value) {
    if (ten_bit) {
      (*storage)[offset] = std::byte{0x80};
      (*storage)[offset + 1] = std::byte(value);
    } else (*storage)[offset] = std::byte(value);
  };
  for (unsigned y = 0; y < height; ++y) for (unsigned x = 0; x < width; ++x)
    write(std::size_t(y) * stride + x * component, 32 + ((x / 8 + y / 8 + index * 3) % 160));
  for (unsigned y = 0; y < height / 2; ++y) for (unsigned x = 0; x < width / 2; ++x) {
    if (config.input.pixel_format != VideoPixelFormat::I420) {
      write(luma + std::size_t(y) * stride + x * 2 * component, 100);
      write(luma + std::size_t(y) * stride + (x * 2 + 1) * component, 150);
    } else {
      (*storage)[luma + std::size_t(y) * (stride / 2) + x] = std::byte{100};
      (*storage)[luma + luma / 4 + std::size_t(y) * (stride / 2) + x] = std::byte{150};
    }
  }
  return frame;
}
struct Encoded {
  VideoEncodeFormat format;
  std::vector<EncodedPacket> packets;
  unsigned keys = 0, bframes = 0;
  bool reordered = false;
  bool variable = false;
};
bool IsB(const EncodedPacket& packet) {
  auto bytes = packet.data.AsBytes();
  for (std::size_t offset = 0; offset < bytes.size();) {
    Check(bytes.size() - offset >= 4, "Truncated public AVC length");
    std::size_t size = 0;
    for (unsigned i = 0; i < 4; ++i) size = (size << 8) | std::to_integer<unsigned>(bytes[offset++]);
    Check(size && size <= bytes.size() - offset, "Invalid public AVC payload");
    const auto nal = bytes.subspan(offset, size); offset += size;
    const auto type = std::to_integer<unsigned>(nal[0]) & 31;
    if (type != 1 && type != 5) continue;
    std::vector<unsigned char> rbsp;
    unsigned zeros = 0;
    for (std::size_t i = 1; i < nal.size(); ++i) {
      const auto value = std::to_integer<unsigned>(nal[i]);
      if (zeros >= 2 && value == 3) { zeros = 0; continue; }
      rbsp.push_back(static_cast<unsigned char>(value)); zeros = value == 0 ? zeros + 1 : 0;
    }
    std::size_t bit = 0;
    auto read = [&] {
      Check(bit < rbsp.size() * 8, "Truncated public AVC slice");
      const auto value = (rbsp[bit / 8] >> (7 - bit % 8)) & 1; ++bit; return value;
    };
    auto ue = [&] {
      unsigned count = 0, value = 1;
      while (!read()) Check(++count < 31, "Invalid public AVC slice integer");
      while (count--) value = (value << 1) | read();
      return value - 1;
    };
    ue();
    if (ue() % 5 == 1) return true;
  }
  return false;
}
Encoded Encode(VideoEncoderConfig config, unsigned count, bool request_key = false, bool variable = false,
    std::span<const VideoFrame> images = {}) {
  if (request_key) config.key_frame_interval = 1000;
  auto cleaned = std::make_shared<std::atomic<bool>>(false), cleanup_ok = std::make_shared<std::atomic<bool>>(false);
  config.cleanup_completed = [cleaned, cleanup_ok](auto result) { *cleanup_ok = result.Succeeded(); *cleaned = true; };
  auto created = VideoEncoder::Create(config); Require(created);
  auto encoder = std::move(created).Value();
  Encoded encoded; encoded.variable = variable;
  bool described = false, ended = false;
  auto drain = [&] {
    for (;;) {
      auto read = encoder.TryRead(); Require(read);
      if (!read.Value()) return;
      Check(!ended, "Video output followed EOS");
      auto& value = *read.Value();
      if (auto* format = std::get_if<VideoEncodeFormat>(&value)) {
        Check(!described, "Duplicate video configuration");
        const bool hevc = config.codec == CodecId::Hevc;
        const auto length = hevc ? 21u : 4u;
        Check(format->width >= config.input.width && format->height >= config.input.height &&
            format->visible == config.input.visible && format->codec == config.codec &&
            format->initialization_data.size() > length &&
            (std::to_integer<unsigned>(format->initialization_data[length]) & 3) == 3, "Wrong coded/cropped picture or NAL length size");
        if (hevc) {
          const auto depth = std::to_integer<unsigned>(format->initialization_data[17]) & 7;
          Check(depth == (config.input.pixel_format == VideoPixelFormat::P010 ? 2 : 0), "HEVC bit depth was silently changed");
        } else {
          const auto profile = config.profile == H264Profile::Baseline ? 66 : config.profile == H264Profile::Main ? 77 : 100;
          Check(std::to_integer<unsigned>(format->initialization_data[1]) == unsigned(profile), "Profile was silently changed");
        }
        encoded.format = *format; described = true;
      } else if (auto* packet = std::get_if<EncodedPacket>(&value)) {
        Check(described && packet->data.AsBytes().size() <= config.max_packet_bytes, "Video packet preceded format or exceeded bounds");
        unsigned identity = 0;
        while (identity < count && PictureTime(identity, variable) != packet->pts) ++identity;
        Check(identity < count && packet->duration == MediaTimestamp{
            PictureTime(identity + 1, variable).ticks - PictureTime(identity, variable).ticks, 90000}, "Input video timing was not preserved");
        if (encoded.packets.empty()) Check(packet->key_frame, "First picture is not random-access");
        else {
          encoded.reordered |= packet->pts.Compare(encoded.packets.back().pts) < 0;
          if (packet->dts && encoded.packets.back().dts) Check(packet->dts->Compare(*encoded.packets.back().dts) > 0, "DTS did not follow decode order");
        }
        encoded.keys += packet->key_frame;
        if (config.codec == CodecId::H264) encoded.bframes += IsB(*packet);
        if (!encoded.packets.empty()) {
          auto copy = std::make_shared<Bytes>(packet->data.AsBytes().begin(), packet->data.AsBytes().end());
          packet->data = BufferReference(*copy, copy);
        }
        encoded.packets.push_back(std::move(*packet));
      } else ended = true;
    }
  };
  bool blocked = false;
  for (unsigned i = 0; i < count; ++i) {
    auto frame = images.empty() ? Frame(config, i, variable) : images[i % images.size()];
    if (!images.empty()) {
      frame.pts = PictureTime(i, variable);
      frame.duration = MediaTimestamp{PictureTime(i + 1, variable).ticks - frame.pts.ticks, frame.pts.timescale};
    }
    Eventually([&] {
      drain();
      auto result = encoder.TrySubmit(frame); Require(result);
      blocked |= i > 0 && result.Value() == SubmitStatus::WouldBlock;
      return result.Value() == SubmitStatus::Accepted;
    }, "Video input stalled");
    if (images.empty()) {
      // Mutate the original allocation after acceptance to verify the CPU input snapshot.
      const auto source = frame.planes.front().data.AsBytes();
      std::fill(const_cast<std::byte*>(source.data()), const_cast<std::byte*>(source.data()) + source.size(), std::byte{0});
    }
    if (request_key && i == count / 2) Require(encoder.RequestKeyFrame());
  }
  if (!images.empty()) Check(blocked, "Platform input did not exercise a submission retry");
  Eventually([&] {
    drain(); auto result = encoder.Finish();
    if (!result.Succeeded() && result.Error().code == CodecErrorCode::InvalidState) return false;
    Require(result); return true;
  }, "Video startup did not finish");
  Eventually([&] { drain(); return ended; }, "Video drain stalled");
  Require(encoder.Finish());
  auto late = encoder.TryRead(); Require(late); Check(!late.Value(), "Repeated video EOS");
  Check(encoded.packets.size() == count, "Video encoder lost or duplicated a picture");
  if (request_key) Check(encoded.keys >= 2, "A key-picture request produced no later IDR");
  if (config.max_b_frames == 0) Check(!encoded.bframes && !encoded.reordered, "Disabled B frames were produced");
  BufferReference retained;
  Bytes snapshot;
  if (!encoded.packets.empty()) {
    retained = encoded.packets.front().data.Slice(0, encoded.packets.front().data.AsBytes().size());
    snapshot.assign(retained.AsBytes().begin(), retained.AsBytes().end());
  }
  Require(encoder.Reset()); encoder.Close(); encoder.Close();
  Eventually([&] { return cleaned->load(); }, "Video cleanup stalled with held packets");
  Check(cleanup_ok->load(), "Video platform cleanup failed");
  Check(std::ranges::equal(snapshot, retained.AsBytes()), "Retained video bytes changed");
  return encoded;
}
void Decode(const Encoded& encoded, const VideoEncoderConfig& input) {
  if (encoded.packets.empty()) return;
  VideoDecoderConfig config;
  config.initialization_data = encoded.format.initialization_data;
  config.width = encoded.format.width; config.height = encoded.format.height; config.codec = encoded.format.codec;
  const bool ten_bit = input.input.pixel_format == VideoPixelFormat::P010;
  config.output = ten_bit ? VideoPixelFormat::P010 : VideoPixelFormat::NV12;
  auto cleaned = std::make_shared<std::atomic<bool>>(false), cleanup_ok = std::make_shared<std::atomic<bool>>(false);
  config.cleanup_completed = [cleaned, cleanup_ok](auto result) { *cleanup_ok = result.Succeeded(); *cleaned = true; };
  auto created = VideoDecoder::Create(config); Require(created);
  auto decoder = std::move(created).Value();
  unsigned frames = 0; bool ended = false;
  double error = 0; std::size_t samples = 0;
  auto drain = [&] {
    for (;;) {
      auto read = decoder.TryRead(); Require(read);
      if (!read.Value()) return;
      if (auto* frame = std::get_if<VideoFrame>(&*read.Value())) {
        Check(frame->Validate() && frame->format.pixel_format == config.output &&
            frame->format.visible.width == input.input.width && frame->format.visible.height == input.input.height,
            "Roundtrip video geometry is wrong");
        Check(frame->pts == PictureTime(frames, encoded.variable), "Roundtrip video presentation order or PTS changed");
        const auto& plane = frame->planes[0]; const auto bytes = plane.data.AsBytes();
        if (input.input_storage == VideoStorage::Platform) {
          const auto area = frame->format.visible;
          const auto sample = [&](unsigned x, unsigned y) {
            return std::to_integer<int>(bytes[(area.y + y) * plane.row_stride + (area.x + x) * plane.pixel_stride]);
          };
          const auto mark = sample(area.width / 4, (frames % 2 ? 3 : 1) * area.height / 4);
          Check(mark > sample(area.width / 4, (frames % 2 ? 1 : 3) * area.height / 4) + 80 &&
                mark > sample(3 * area.width / 4, area.height / 4) + 80, "Roundtrip changed the texture watermark");
          ++frames;
          continue;
        }
        for (unsigned y = 0; y < input.input.height; y += 4) for (unsigned x = 0; x < input.input.width; x += 4) {
          const auto expected = 32 + ((x / 8 + y / 8 + frames * 3) % 160);
          const auto actual = std::to_integer<unsigned>(bytes[(y + frame->format.visible.y) * plane.row_stride +
              (x + frame->format.visible.x) * plane.pixel_stride + (ten_bit ? 1 : 0)]);
          const auto delta = double(actual) - expected; error += delta * delta; ++samples;
        }
        const auto& uv = frame->planes[1];
        const auto offset = (frame->format.visible.y / 2) * uv.row_stride +
            (frame->format.visible.x / 2) * uv.pixel_stride;
        const auto component = ten_bit ? 2u : 1u;
        const auto u = std::to_integer<int>(uv.data.AsBytes()[offset + component - 1]);
        const auto v = std::to_integer<int>(uv.data.AsBytes()[offset + component * 2 - 1]);
        Check(std::abs(u - 100) < 12 && std::abs(v - 150) < 12, "Roundtrip video chroma layout changed");
        ++frames;
      } else if (std::holds_alternative<EndOfStream>(*read.Value())) ended = true;
    }
  };
  for (const auto& packet : encoded.packets) Eventually([&] {
    drain(); auto result = decoder.TrySubmit(packet); Require(result); return result.Value() == SubmitStatus::Accepted;
  }, "Roundtrip video input stalled");
  Require(decoder.Finish());
  Eventually([&] { drain(); return ended; }, "Roundtrip video drain stalled");
  decoder.Close();
  Eventually([&] { return cleaned->load(); }, "Roundtrip decoder cleanup stalled");
  Check(cleanup_ok->load(), "Roundtrip decoder cleanup failed");
  Check(frames == encoded.packets.size() && (input.input_storage == VideoStorage::Platform || (samples && std::sqrt(error / samples) < 12)),
        "Roundtrip video count or luma quality failed");
  std::cout << (config.codec == CodecId::Hevc ? ten_bit ? "HEVC Main10" : "HEVC Main" : "H.264")
      << " pictures=" << frames;
  if (config.codec == CodecId::H264) std::cout << " B=" << encoded.bframes;
  std::cout << " reordered=" << encoded.reordered;
  if (samples) std::cout << " luma-RMSE=" << std::sqrt(error / samples);
  else std::cout << " SDK texture watermark verified";
  std::cout << '\n';
}
bool Supported(const VideoEncoderConfig& config) {
  auto done = std::make_shared<std::atomic<bool>>(false);
  auto failure = std::make_shared<std::optional<CodecError>>();
  VideoEncoder::Probe(config, [done, failure](auto result) {
    if (!result.Succeeded()) *failure = result.Error();
    *done = true;
  });
  Eventually([&] { return done->load(); }, "Video capability probe stalled");
  if (!*failure) return true;
  if ((*failure)->code != CodecErrorCode::Unsupported) throw std::runtime_error((*failure)->message);
  std::cout << "Unsupported " << (config.codec == CodecId::Hevc ? "HEVC" : "H.264")
      << " layout=" << int(config.input.pixel_format) << " B="
      << (config.max_b_frames ? std::to_string(*config.max_b_frames) : "auto")
      << ": " << (*failure)->message << " (" << (*failure)->platform_code << ")\n";
  return false;
}

template<class T, class E> T Take(Result<T, E> result) {
  if (!result.Succeeded()) throw std::runtime_error(result.Error().message);
  if constexpr (!std::is_void_v<T>) return std::move(result).Value();
}

Encoded ReadbackMp4(Encoded encoded, const File& file) {
  const auto& format = encoded.format;
  constexpr std::uint32_t scale = 9000000;
  const auto first = PictureTime(0, encoded.variable);
  const auto end = PictureTime(static_cast<unsigned>(encoded.packets.size()), encoded.variable);
  const MediaTrack track{.id = 1, .kind = TrackKind::Video, .timescale = scale,
      .descriptions = {{.codec = format.codec, .framing = PacketFraming::LengthPrefixedNals,
          .initialization_data = format.initialization_data, .coded_width = format.width,
          .coded_height = format.height, .visible = format.visible}},
      .edits = {{first, {}}, {{end.ticks - first.ticks, end.timescale}, first}}};
  auto mux = Take(MediaMuxer::Open(Take(file.OpenRandomAccess(FileAccessMode::ReadWrite, FileOpenMode::CreateNew)),
      ContainerFormat::Mp4, std::span(&track, 1), {.layout = ContainerLayout::FastStart}));
  auto dts = [&](const EncodedPacket& packet) {
    if (!encoded.reordered) return *packet.pts.Rescale(scale);
    Check(packet.dts.has_value(), "Reordered encoder output did not provide DTS");
    return *packet.dts->Rescale(scale);
  };
  const auto decode_end = dts(encoded.packets.front()).ticks + MediaTimestamp{end.ticks - first.ticks, end.timescale}.Rescale(scale)->ticks;
  for (std::size_t i = 0; i < encoded.packets.size(); ++i) {
    auto& packet = encoded.packets[i];
    packet.dts = dts(packet);
    const auto next = i + 1 < encoded.packets.size() ? dts(encoded.packets[i + 1]).ticks : decode_end;
    packet.duration = MediaTimestamp{next - packet.dts->ticks, scale};
    Check(!Take(mux.TryWrite({1, packet.data, packet.pts, packet.dts, packet.duration, packet.key_frame})),
        "MP4 writer did not accept the packet");
  }
  Take(mux.Finish());
  auto demux = Take(MediaDemuxer::Open(Take(file.OpenRandomAccess())));
  const auto tracks = demux.Tracks();
  const auto& read_track = tracks.front().descriptions.front();
  const auto origin = dts(encoded.packets.front()).ticks;
  Check(demux.PresentationRange(track.id) == MediaPresentationRange{first, end}, "MP4 changed the presentation timeline");
  Check(std::holds_alternative<TrackDecodeStart>(*Take(demux.Read())), "MP4 lost its decode start");
  Check(read_track.codec == format.codec && (format.codec != CodecId::Hevc || read_track.sample_entry == "hvc1") &&
      read_track.coded_width == format.width && read_track.coded_height == format.height &&
      read_track.initialization_data == format.initialization_data &&
      read_track.visible.value_or(VideoRectangle{0, 0, format.width, format.height}) == format.visible,
      "MP4 did not retain its configuration and geometry");
  for (auto& expected : encoded.packets) {
    auto item = Take(demux.Read());
    Check(bool(item), "MP4 lost a packet");
    auto& actual = std::get<ContainerPacket>(*item);
    Check(actual.pts == MediaTimestamp{expected.pts.Rescale(scale)->ticks - origin, scale} &&
        actual.dts == MediaTimestamp{expected.dts->Rescale(scale)->ticks - origin, scale} && actual.duration == expected.duration &&
        actual.sync_sample == expected.key_frame && std::ranges::equal(actual.data.AsBytes(), expected.data.AsBytes()),
        "MP4 changed a packet");
    expected.data = std::move(actual.data);
  }
  Check(!Take(demux.Read()), "MP4 has extra packets");
  return encoded;
}

void Hevc(const char* retain = nullptr) {
  for (const auto layout : {VideoPixelFormat::NV12, VideoPixelFormat::P010}) {
    auto config = Config(H264Profile::High, 0, layout);
    config.codec = CodecId::Hevc;
#if defined(__EMSCRIPTEN__) || (defined(__APPLE__) && TARGET_OS_SIMULATOR)
    config.max_b_frames = std::nullopt;
#endif
    if (!Supported(config)) continue;
    auto encoded = Encode(config, 19, false, true);
#ifndef __EMSCRIPTEN__
    const bool keep = retain && layout == VideoPixelFormat::NV12;
    const auto path = keep ? std::filesystem::path(retain) : std::filesystem::temp_directory_path() /
        ("lib-media-hevc-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".mp4");
    encoded = ReadbackMp4(std::move(encoded), File(path.u8string()));
    if (!keep) std::filesystem::remove(path);
#endif
    Decode(encoded, config);
  }
}

#ifdef __EMSCRIPTEN__
EM_JS(emscripten::EM_VAL, PaintWatermark, (emscripten::EM_VAL handle, unsigned index), {
  const canvas = Emval.toValue(handle);
  const gl = canvas.getContext('webgl2', {alpha: false, preserveDrawingBuffer: true});
  if (!gl) return Emval.toHandle(null);
  gl.clearColor(0.2, 0.2, 0.2, 1);
  gl.clear(gl.COLOR_BUFFER_BIT);
  gl.enable(gl.SCISSOR_TEST);
  gl.scissor(0, index === 0 ? canvas.height / 2 : 0, canvas.width / 2, canvas.height / 2);
  gl.clearColor(0.85, 0.85, 0.85, 1);
  gl.clear(gl.COLOR_BUFFER_BIT);
  gl.disable(gl.SCISSOR_TEST);
  return Emval.toHandle(new VideoFrame(canvas, {timestamp: 123}));
});

EM_JS(emscripten::EM_VAL, DeferVideoEncoder, (), {
  const original = globalThis.VideoEncoder;
  const test = {
    encoder: null,
    closed: 0,
    complete(data, description) {
      const encoder = this.encoder;
      const chunk = new EncodedVideoChunk({type: 'key', timestamp: encoder.frame.timestamp, data});
      encoder.release();
      encoder.output(chunk, {decoderConfig: {codec: 'avc1.42001f', description}});
    },
    restore() { globalThis.VideoEncoder = original; }
  };
  globalThis.VideoEncoder = class {
    static async isConfigSupported(config) { return {supported: true, config}; }
    constructor({output}) {
      this.output = output;
      this.frame = null;
      this.state = 'unconfigured';
      test.encoder = this;
    }
    configure() { this.state = 'configured'; }
    get encodeQueueSize() { return this.frame ? 1 : 0; }
    encode(frame) {
      if (this.frame) throw new Error('Deferred encoder already holds a frame');
      this.frame = frame.clone();
    }
    flush() {
      return this.frame ? new Promise(resolve => { this.flushed = resolve; }) : Promise.resolve();
    }
    release() {
      this.frame?.close();
      this.frame = null;
      this.flushed?.();
      this.flushed = null;
    }
    close() {
      this.release();
      this.state = 'closed';
      ++test.closed;
    }
  };
  return Emval.toHandle(test);
});

void CheckBrowserFrameLifetime(VideoEncoderConfig config, const emscripten::val& canvas, const Encoded& encoded) {
  using emscripten::val;
  struct DeferredEncoder {
    val state = val::take_ownership(DeferVideoEncoder());
    ~DeferredEncoder() { state.call<void>("restore"); }
  };
  const DeferredEncoder deferred;
  auto cleaned = std::make_shared<std::optional<bool>>();
  config.cleanup_completed = [cleaned](auto result) { *cleaned = result.Succeeded(); };
  config.limits.input_items = 1;
  auto encoder = Take(VideoEncoder::Create(config));

  auto make_frame = [&](unsigned index) {
    auto texture = std::make_shared<web::VideoFrameTexture>(Size{160, 120});
    const auto picture = val::take_ownership(PaintWatermark(canvas.as_handle(), index));
    Check(!picture.isNull(), "WebGL2 could not render the lifetime test frame");
    texture->Publish(picture);
    picture.call<void>("close");
    return std::pair{Take(MakeVideoFrame(texture, config.input, PictureTime(index, false))), std::weak_ptr(texture)};
  };
  auto submit = [&](VideoFrame& frame) {
    Eventually([&] { return Take(encoder.TrySubmit(frame)) == SubmitStatus::Accepted; }, "Deferred video input stalled");
    frame = {};
    Eventually([&] {
      const auto current = deferred.state["encoder"];
      return !current.isNull() && !current["frame"].isNull();
    }, "WebCodecs did not acquire the deferred frame");
  };
  auto [first, first_texture] = make_frame(0);
  auto [second, second_texture] = make_frame(1);
  submit(first);
  Check(!first_texture.expired(), "SDK texture was released while WebCodecs held its frame");
  Check(Take(encoder.TrySubmit(second)) == SubmitStatus::WouldBlock, "WebCodecs released the in-flight input reservation");
  auto view = [](std::span<const std::byte> bytes) {
    return val(emscripten::typed_memory_view(bytes.size(), reinterpret_cast<const unsigned char*>(bytes.data())));
  };
  deferred.state.call<void>("complete", view(encoded.packets.front().data.AsBytes()), view(encoded.format.initialization_data));
  Eventually([&] {
    auto output = Take(encoder.TryRead());
    return output && std::holds_alternative<EncodedPacket>(*output);
  }, "Deferred video output stalled");
  Check(first_texture.expired(), "Encoded output did not release the SDK texture");

  submit(second);
  Check(!second_texture.expired(), "Reset test frame was released before WebCodecs closed");
  Take(encoder.Reset());
  Eventually([&] {
    return deferred.state["closed"].as<unsigned>() == 1 && second_texture.expired();
  }, "Reset did not release the deferred frame");

  auto [last, last_texture] = make_frame(2);
  submit(last);
  Check(!last_texture.expired(), "Close test frame was released before WebCodecs closed");
  encoder.Close();
  Eventually([&] { return cleaned->has_value(); }, "Deferred encoder cleanup stalled");
  Check(cleaned->value() && deferred.state["closed"].as<unsigned>() == 2 && last_texture.expired(),
      "Close did not release the deferred frame before cleanup completion");
}

void CheckBrowserTextureInput() {
  using emscripten::val;
  auto config = Config(H264Profile::Baseline, 0, VideoPixelFormat::Opaque);
  config.input_storage = VideoStorage::Platform;
  Check(Supported(config), "Web H.264 platform input is unavailable");
  auto canvas = val::global("document").call<val>("createElement", std::string("canvas"));
  canvas.set("width", config.input.width);
  canvas.set("height", config.input.height);
  struct PixelAudit {
    val prototype = val::global("VideoFrame")["prototype"];
    val copy = prototype["copyTo"];
    PixelAudit() {
      EM_ASM({ VideoFrame.prototype.copyTo = function() { throw new Error('Unexpected platform pixel readback'); }; });
    }
    ~PixelAudit() { prototype.set("copyTo", copy); }
  };
  const auto encode = [&](VideoEncoderConfig input) {
    const PixelAudit audit;
    input.limits.input_items = 1;
    std::array<VideoFrame, 2> images;
    auto texture = std::make_shared<web::VideoFrameTexture>(Size{160, 120});
    const auto unpublished = MakeVideoFrame(texture, input.input, {0, 1});
    Check(!unpublished.Succeeded() && unpublished.Error().code == CodecErrorCode::InvalidState, "An unpublished texture yielded a frame");
    for (unsigned index = 0; index < images.size(); ++index) {
      const auto picture = val::take_ownership(PaintWatermark(canvas.as_handle(), index));
      Check(!picture.isNull(), "WebGL2 could not render the watermark");
      texture->Publish(picture);
      picture.call<void>("close");
      images[index] = Take(MakeVideoFrame(texture, input.input, PictureTime(index, true)));
    }
    texture.reset();
    return Encode(input, 12, false, true, images);
  };
  for (auto codec : {CodecId::H264, CodecId::Hevc}) {
    config.codec = codec;
    if (codec == CodecId::Hevc) {
      config.max_b_frames = std::nullopt;
      if (!Supported(config)) continue;
    }
    auto encoded = encode(config);
    if (codec == CodecId::H264) CheckBrowserFrameLifetime(config, canvas, encoded);
    const File path(codec == CodecId::Hevc ? u8"/tmp/platform-hevc.mp4" : u8"/tmp/platform-avc.mp4");
    Decode(ReadbackMp4(std::move(encoded), path), config);
    Check(path.Delete(), "Cannot remove the Web texture test output");
  }
}
#endif
}
#ifdef __EMSCRIPTEN__
int main() {
#else
int main(int argc, char** argv) {
#endif
  try {
#ifndef __EMSCRIPTEN__
    if (argc >= 2 && std::string_view(argv[1]) == "--hevc-only") {
      Hevc(argc >= 3 ? argv[2] : nullptr);
      return 0;
    }
#endif
#ifdef __EMSCRIPTEN__
    if (EM_ASM_INT({ return new URLSearchParams(location.search).get('storage') === 'platform'; })) {
      CheckBrowserTextureInput();
      EM_ASM({ document.body.textContent = 'PASS: Web texture encoding, MP4 readback, frame lifetime and retry'; });
      return 0;
    }
    unsigned exercised = 0;
    for (auto profile : {H264Profile::Baseline, H264Profile::Main, H264Profile::High}) {
      auto config = Config(profile, profile == H264Profile::Baseline ? std::optional<std::uint32_t>(0) : std::nullopt);
      if (!Supported(config)) continue;
      for (auto count : {0u, 1u, 2u}) Encode(config, count);
      auto encoded = Encode(config, 20, true); Decode(encoded, config); ++exercised;
    }
    Check(exercised > 0, "No H.264 encoder configuration is available in this browser");
    auto extended = Config(H264Profile::High, std::nullopt, VideoPixelFormat::I420, 242, 322);
    if (Supported(extended)) {
      extended.limits.output_items = 48; extended.limits.output_bytes = 48 * extended.max_packet_bytes;
      Decode(Encode(extended, 70, true, true), extended);
    }
    Hevc();
    EM_ASM({ document.body.textContent = 'PASS: C++ WASM video encoding, roundtrip, timing and cleanup'; });
#else
    for (auto count : {0u, 1u, 2u}) Encode(Config(H264Profile::High, 0), count);
    for (auto profile : {H264Profile::Baseline, H264Profile::Main, H264Profile::High}) {
      auto config = Config(profile, 0);
      config.bitrate = 4'000'123;
      auto encoded = Encode(config, 20, true); Decode(encoded, config);
    }
    for (auto profile : {H264Profile::Main, H264Profile::High}) for (auto bframes : {1u, 2u}) {
      auto config = Config(profile, bframes, VideoPixelFormat::I420);
#if defined(__APPLE__) || defined(__ANDROID__)
      if (!Supported(config)) continue;
#endif
      auto encoded = Encode(config, 29, true); Decode(encoded, config);
#if !defined(__APPLE__) && !defined(__ANDROID__)
      Check(encoded.bframes && encoded.reordered, "Requested B-picture test did not exercise real reordering");
#endif
    }
#if defined(__APPLE__) && TARGET_OS_OSX
    // Submitting more frames than the queues can hold requires output before Finish().
    auto tight = Config(H264Profile::High, std::nullopt);
    tight.limits.output_items = 2;
    tight.limits.output_bytes = tight.limits.output_items * tight.max_packet_bytes;
    Decode(Encode(tight, 40, true, true), tight);
#endif
    auto config = Config(H264Profile::High, std::nullopt, VideoPixelFormat::I420, 242);
    auto encoded = Encode(config, 17); Decode(encoded, config);
    auto variable = Encode(config, 17, true, true); Decode(variable, config);
    auto narrow = Config(H264Profile::High, 0, VideoPixelFormat::NV12, 242, 322);
    Decode(Encode(narrow, 6), narrow);
    auto probed = std::make_shared<std::atomic<bool>>(false), supported = std::make_shared<std::atomic<bool>>(false);
    auto probe_error = std::make_shared<std::string>();
    VideoEncoder::Probe(config, [probed, supported, probe_error](auto result) {
      *supported = result.Succeeded();
      if (!result.Succeeded()) *probe_error = result.Error().message + " (" + result.Error().platform_code + ")";
      *probed = true;
    });
    Eventually([&] { return probed->load(); }, "Video probe stalled"); Check(supported->load(), "Video probe failed: " + *probe_error);
    std::cout << "H.264 profiles, B-picture ordering, retained packets, timing, crop, and roundtrip passed\n";
    Hevc();
#endif
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
#ifdef __EMSCRIPTEN__
    EM_ASM({ document.body.textContent = 'FAIL: ' + UTF8ToString($0); }, error.what());
#endif
    return 1;
  }
}
