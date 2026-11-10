#include <huxerui/media_codec.h>

#if defined(__linux__) && !defined(__ANDROID__)
#include "codec/decoder/backend.h"
#endif
#ifdef __ANDROID__
#include <media/NdkMediaCodec.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <thread>

#ifdef __ANDROID__
// Exercise the legacy NDK contract while decoding real MediaCodec buffers.
extern "C" ssize_t __real_AMediaCodec_dequeueOutputBuffer(AMediaCodec*, AMediaCodecBufferInfo*, int64_t);
extern "C" ssize_t __wrap_AMediaCodec_dequeueOutputBuffer(AMediaCodec* codec, AMediaCodecBufferInfo* info, int64_t timeout) {
  const auto index = __real_AMediaCodec_dequeueOutputBuffer(codec, info, timeout);
  if (index >= 0 && info->size > 0) info->offset = -1;
  return index;
}
extern "C" uint8_t* __real_AMediaCodec_getOutputBuffer(AMediaCodec*, size_t, size_t*);
extern "C" uint8_t* __wrap_AMediaCodec_getOutputBuffer(AMediaCodec* codec, size_t index, size_t* size) {
  auto* data = __real_AMediaCodec_getOutputBuffer(codec, index, nullptr);
  if (size) *size = 0;
  return data;
}
#endif

using namespace huxerui;
using namespace huxerui::media;
namespace {
void Check(bool value, const std::string& message) { if (!value) throw std::runtime_error(message); }
Bytes Read(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  Check(file.good(), "Cannot read fixture " + path.string());
  Bytes bytes(static_cast<std::size_t>(file.tellg()));
  file.seekg(0);
  file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  Check(file.good(), "Cannot read complete fixture");
  return bytes;
}
std::vector<EncodedPacket> Packets(const std::filesystem::path& root, const std::string& name) {
  std::ifstream manifest(root / (name + ".txt"));
  std::vector<EncodedPacket> packets;
  std::string filename;
  std::int64_t pts, dts, duration;
  std::uint32_t scale;
  bool key;
  while (manifest >> filename >> pts >> dts >> duration >> scale >> key) {
    auto bytes = std::make_shared<Bytes>(Read(root / filename));
    packets.push_back({BufferReference(*bytes, bytes), {pts, scale}, MediaTimestamp{dts, scale}, MediaTimestamp{duration, scale}, key});
  }
  Check(!packets.empty(), "No fixture packets");
  return packets;
}
std::vector<EncodedPacket> PackedPackets(const std::filesystem::path& root, const std::string& name) {
  auto bytes = std::make_shared<const Bytes>(Read(root / (name + ".bin")));
  const BufferReference data(*bytes, bytes);
  std::ifstream manifest(root / (name + ".txt"));
  std::vector<EncodedPacket> packets;
  std::size_t offset, size;
  std::int64_t pts, dts, duration;
  std::uint32_t scale;
  bool key;
  while (manifest >> offset >> size >> pts >> dts >> duration >> scale >> key)
    packets.push_back({data.Slice(offset, size), {pts, scale}, MediaTimestamp{dts, scale},
                       MediaTimestamp{duration, scale}, key});
  Check(!packets.empty(), "No packed fixture packets");
  return packets;
}

#if defined(__linux__) && !defined(__ANDROID__)
void AppSrcBackpressure(const AudioDecoderConfig& defaults, const EncodedPacket& access_unit) {
  for (bool limit_bytes : {false, true}) {
    auto config = defaults;
    config.limits.input_items = limit_bytes ? 64 : 2;
    if (limit_bytes) config.limits.input_bytes = access_unit.data.AsBytes().size() * 2 - 1;
    auto backend = huxerui::media::detail::MakeDecoderBackend();
    auto result = backend->Open(config);
    // With no reads, the downstream sink fills and appsrc must stop accepting pushes.
    auto packet = access_unit;
    bool blocked = false;
    for (int i = 0; result.Succeeded() && i < 64; ++i) {
      packet.pts = {i * 1024, config.output.sample_rate};
      packet.dts = packet.pts;
      auto submitted = backend->Submit(packet);
      if (!submitted.Succeeded()) {
        result = CodecResult<void>::Failure(submitted.Error());
        break;
      }
      if (submitted.Value() == SubmitStatus::WouldBlock) { blocked = true; break; }
    }
    auto closed = backend->Close();
    Check(result.Succeeded(), result.Succeeded() ? "" : result.Error().message);
    Check(closed.Succeeded(), closed.Succeeded() ? "" : closed.Error().message);
    Check(blocked, limit_bytes ? "appsrc ignored the next packet's byte cost" : "appsrc ignored its item limit");
  }
}
#endif
struct Signal {
  std::mutex mutex;
  std::condition_variable changed;
  std::size_t revision = 0;
  bool cleaned = false;
  std::optional<CodecError> error;
  void Wake() { std::lock_guard lock(mutex); ++revision; changed.notify_all(); }
  void Wait() { std::unique_lock lock(mutex); changed.wait_for(lock, std::chrono::milliseconds(10)); }
};
template<class Decoder, class Config, class Frame>
void Decode(Config config, const std::vector<EncodedPacket>& packets, std::size_t expected) {
  auto presentation_order = packets;
  std::stable_sort(presentation_order.begin(), presentation_order.end(),
                   [](const auto& left, const auto& right) { return left.pts.Compare(right.pts) == -1; });
  auto signal = std::make_shared<Signal>();
  config.wakeup = [signal] { signal->Wake(); };
  config.cleanup_completed = [signal](CodecResult<void> result) {
    std::lock_guard lock(signal->mutex);
    signal->cleaned = true;
    if (!result.Succeeded()) signal->error = result.Error();
    signal->changed.notify_all();
  };
  auto created = Decoder::Create(config);
  Check(created.Succeeded(), created.Succeeded() ? "" : created.Error().message);
  auto decoder = std::move(created).Value();
  std::optional<Frame> held;
  bool nonzero = false;
  for (unsigned pass = 0; pass < 2; ++pass) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    std::size_t submitted = 0, frames = 0, samples = 0, formats = 0;
    bool sealed = false, eos = false;
    while (!eos && std::chrono::steady_clock::now() < deadline) {
      while (submitted < packets.size()) {
        auto result = decoder.TrySubmit(packets[submitted]);
        Check(result.Succeeded(), result.Succeeded() ? "" : result.Error().message + ": " + result.Error().platform_code);
        if (result.Value() == SubmitStatus::WouldBlock) break;
        ++submitted;
      }
      if (submitted == packets.size() && !sealed) {
        auto result = decoder.Finish();
        Check(result.Succeeded(), "Finish failed"); sealed = true;
      }
      for (;;) {
        auto read = decoder.TryRead();
        Check(read.Succeeded(), read.Succeeded() ? "" : read.Error().message + ": " + read.Error().platform_code);
        if (!read.Value()) break;
        if (auto* frame = std::get_if<Frame>(&*read.Value())) {
          Check(formats > 0, "Frame preceded its format");
          Check(frame->Validate(), "Invalid decoded frame");
          Check(frames < presentation_order.size(), "Platform adapter produced an extra frame");
          const auto& packet = presentation_order[frames];
          Check(packet.pts.ticks == frame->pts.ticks && packet.pts.timescale == frame->pts.timescale,
                "Platform adapter changed the expected PTS sequence");
          if constexpr (std::is_same_v<Frame, VideoFrame>)
            Check(frame->duration == packet.duration, "Platform adapter changed the video frame duration");
          for (const auto& plane : frame->planes) {
            const auto data = [&] {
              if constexpr (std::is_same_v<Frame, AudioFrame>) return plane.AsBytes();
              else return plane.data.AsBytes();
            }();
            for (auto byte : data) if (byte != std::byte{0}) nonzero = true;
          }
          ++frames;
          if constexpr (std::is_same_v<Frame, AudioFrame>) samples += frame->sample_count;
          held = *frame;
        } else if (std::holds_alternative<EndOfStream>(*read.Value())) eos = true;
        else ++formats;
      }
      if (!eos) signal->Wait();
    }
    if (!eos) {
      decoder.Close();
      std::unique_lock lock(signal->mutex);
      signal->changed.wait_for(lock, std::chrono::seconds(5), [&] { return signal->cleaned; });
      throw std::runtime_error("Timed out waiting for EOS: submitted=" + std::to_string(submitted) + ", frames=" + std::to_string(frames) + ", pass=" + std::to_string(pass));
    }
    Check(frames == expected, "Unexpected frame count: " + std::to_string(frames) + "/" + std::to_string(expected));
    if constexpr (std::is_same_v<Frame, AudioFrame>) Check(samples == expected * 1024, "Unexpected PCM sample count");
    Check(decoder.Finish().Succeeded(), "Repeated Finish failed");
    auto empty = decoder.TryRead();
    Check(empty.Succeeded() && !empty.Value(), "EOS repeated");
    if (!pass) {
      Check(decoder.Reset().Succeeded(), "Reset failed");
      Check(decoder.Reset().Succeeded(), "Repeated Reset failed");
      Check(held && held->Validate(), "Reset invalidated retained CPU output");
      held.reset();
    }
  }
  Check(nonzero, "Decoded synthetic media contains no signal");
  decoder.Close();
  decoder.Close();
  auto closed = decoder.TryRead();
  Check(!closed.Succeeded() && closed.Error().code == CodecErrorCode::Closed, "Close barrier failed");
  {
    std::unique_lock lock(signal->mutex);
    Check(signal->changed.wait_for(lock, std::chrono::seconds(5), [&] { return signal->cleaned; }), "Cleanup timeout");
    Check(!signal->error, signal->error ? signal->error->message : "");
  }
  Check(held && held->Validate(), "Retained frame invalid after cleanup");
}
} // namespace
int main(int argc, char** argv) {
  try {
    const std::filesystem::path root = argc > 1 ? argv[1] : MEDIA_CODEC_FIXTURES;
    AudioDecoderConfig audio;
    audio.initialization_data = Read(root / "aac.config");
    audio.output.sample_rate = 48000;
    audio.limits.input_items = 2;
    audio.limits.output_items = 4;
    audio.limits.output_bytes = 32768;
    auto retirement = std::make_shared<std::atomic<int>>(0);
    {
      auto first_config = audio;
      first_config.cleanup_completed = [retirement](CodecResult<void> result) {
        if (result.Succeeded()) ++*retirement;
      };
      auto first = AudioDecoder::Create(first_config);
      auto second = AudioDecoder::Create(first_config);
      Check(first.Succeeded() && second.Succeeded(), "Cannot create simultaneous sessions");
      auto owner = std::move(first).Value();
      owner = std::move(second).Value();
    }
    const auto retired_by = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (*retirement != 2 && std::chrono::steady_clock::now() < retired_by) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    Check(*retirement == 2, "Move assignment/destruction failed to retire both platform sessions");
    auto aac = Packets(root, "aac");
#if defined(__APPLE__) || (defined(__linux__) && !defined(__ANDROID__))
    const auto largest = std::max_element(aac.begin(), aac.end(), [](const auto& a, const auto& b) {
      return a.data.AsBytes().size() < b.data.AsBytes().size();
    });
#endif
#ifdef __APPLE__
    // Exercise converter progress with only enough byte credit for the largest access unit.
    audio.limits.input_bytes = largest->data.AsBytes().size();
#endif
#if defined(__linux__) && !defined(__ANDROID__)
    AppSrcBackpressure(audio, *largest);
#endif
    Decode<AudioDecoder, AudioDecoderConfig, AudioFrame>(audio, aac, aac.size());
    audio.output.packing = AudioPacking::Planar;
    audio.output.sample_format = AudioSampleFormat::S16;
    Decode<AudioDecoder, AudioDecoderConfig, AudioFrame>(audio, aac, aac.size());
    audio.initialization_data = Read(root / "sync-aac.config");
    audio.output.packing = AudioPacking::Interleaved;
    audio.limits = {4, 8 * 1024 * 1024, 4, 4 * 1024 * 4};
    auto sync_aac = PackedPackets(root, "sync-aac");
    Decode<AudioDecoder, AudioDecoderConfig, AudioFrame>(audio, sync_aac, sync_aac.size());
    VideoDecoderConfig video;
    Check(video.codec == CodecId::H264, "Video decoder no longer defaults to H.264");
    video.initialization_data = Read(root / "avc.config");
    video.width = 64; video.height = 48;
    auto unsupported = video;
    for (const auto codec : {CodecId::Aac, CodecId::Vp9, CodecId::Unknown}) {
      unsupported.codec = codec;
      const auto rejected = VideoDecoder::Create(unsupported);
      Check(!rejected.Succeeded() && rejected.Error().code == CodecErrorCode::InvalidArgument,
            "Video decoder must reject codec IDs outside H.264 and HEVC");
    }
    auto avc = Packets(root, "avc");
    Decode<VideoDecoder, VideoDecoderConfig, VideoFrame>(video, avc, avc.size());
    video.output = VideoPixelFormat::I420;
    Decode<VideoDecoder, VideoDecoderConfig, VideoFrame>(video, avc, avc.size());
    video.initialization_data = Read(root / "sync-avc.config");
    video.output = VideoPixelFormat::NV12;
    video.limits.input_items = 4;
    video.limits.output_items = 34;
    video.limits.output_bytes = video.limits.output_items * video.width * video.height * 3 / 2;
    auto sync_avc = PackedPackets(root, "sync-avc");
    Decode<VideoDecoder, VideoDecoderConfig, VideoFrame>(video, sync_avc, sync_avc.size());
    std::cout << "AAC/AVC native decode, format, EOS, reset, retained output, and cleanup passed\n";
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
