#include <huxerui/file.h>
#include <huxerui/media_codec.h>
#include <huxerui/media_container.h>
#include "video_frame_transformer.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <thread>

#ifdef _WIN32
#include <d3d11.h>
#include <future>
#include <huxerui/windows/external_texture.h>
#include <windows.h>
#include <psapi.h>
#include <wrl/client.h>
#else
#include <sys/resource.h>
#endif

#ifdef __APPLE__
#import <CoreVideo/CoreVideo.h>
#import <Metal/Metal.h>
#include <TargetConditionals.h>
#include <dlfcn.h>
#if TARGET_OS_IOS
#include <huxerui/ios/external_texture.h>
#else
#include <huxerui/macos/external_texture.h>
#endif
#elif defined(__ANDROID__)
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <huxerui/android/external_texture.h>
#include <media/NdkMediaCodec.h>
#endif

using namespace huxerui;
using namespace huxerui::media;
using namespace std::chrono_literals;

namespace {

#ifndef _WIN32
std::atomic<bool> audit_pixels{false};
std::atomic<unsigned> cpu_pixel_accesses{0};

#ifdef __ANDROID__
std::atomic<AMediaCodec*> audited_decoder{nullptr};
#endif

struct PixelAudit {
  PixelAudit() { cpu_pixel_accesses = 0; audit_pixels = true; }
  ~PixelAudit() { audit_pixels = false; }
};
#endif

void Check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

template<class T, class E> T Take(Result<T, E> result) {
  if (!result.Succeeded()) {
    const auto& error = result.Error();
    if constexpr (requires { error.platform_code; })
      throw std::runtime_error(error.message + " (" + error.platform_code + ")");
    else
      throw std::runtime_error(error.message);
  }
  if constexpr (!std::is_void_v<T>) return std::move(result).Value();
}

#ifdef _WIN32
struct MissingHardware {};

template<class Codec, class Config> bool CodecAvailable(const Config& config) {
  auto outcome = std::make_shared<std::promise<CodecResult<void>>>();
  auto future = outcome->get_future();
  Codec::Probe(config, [outcome](auto result) { outcome->set_value(std::move(result)); });
  Check(future.wait_for(10s) == std::future_status::ready, "Hardware codec probe timed out");
  auto result = future.get();
  if (!result.Succeeded() && result.Error().code == CodecErrorCode::Unsupported) {
    std::cout << result.Error().message << '\n';
    return false;
  }
  Take(std::move(result));
  return true;
}
#endif

template<class Codec> class TestSession {
public:
  template<class Config> explicit TestSession(Config config) : codec(Create(std::move(config))) {}
  ~TestSession() { Close(); }
  bool Close() {
    codec.Close();
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!*retired_ && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(1ms);
    return *retired_ == 1;
  }

private:
  template<class Config> Codec Create(Config config) {
    config.cleanup_completed = [retired = retired_](auto result) { *retired = result.Succeeded() ? 1 : 2; };
    return Take(Codec::Create(config));
  }
  std::shared_ptr<std::atomic<unsigned>> retired_ = std::make_shared<std::atomic<unsigned>>(0);
public:
  Codec codec;
};

struct Clip {
  MediaTrack track;
  std::vector<EncodedPacket> packets;
  std::vector<MediaTimestamp> times;
};

Clip ReadClip(const File& file) {
  auto source = Take(MediaDemuxer::Open(Take(file.OpenRandomAccess())));
  const auto video = std::ranges::find(source.Tracks(), TrackKind::Video, &MediaTrack::kind);
  Check(video != source.Tracks().end() && video->timescale && video->descriptions.front().coded_width && video->descriptions.front().coded_height, "No timed video track");
  Clip clip{.track = *video};
  Take(source.SelectTracks(std::span(&clip.track.id, 1)));
  while (auto item = Take(source.Read())) {
    if (const auto* packet = std::get_if<ContainerPacket>(&*item)) {
      Check(packet->pts && packet->duration, "Video packet is missing timing");
      clip.packets.push_back({packet->data, *packet->pts, packet->dts, packet->duration, packet->sync_sample.value_or(false)});
      clip.times.push_back(*packet->pts);
    }
  }
  std::ranges::sort(clip.times, [](auto a, auto b) { return a.Compare(b) < 0; });
  Check(!clip.times.empty(), "Empty video fixture");
  return clip;
}

VideoDecoderConfig DecoderConfig(const MediaTrack& track, bool gpu) {
  const auto& description = track.descriptions.front();
  VideoDecoderConfig config{.initialization_data = description.initialization_data,
      .width = *description.coded_width, .height = *description.coded_height, .codec = description.codec,
      .output_storage = gpu ? VideoStorage::Platform : VideoStorage::Cpu};
  if (description.codec == CodecId::H264) {
    config.width = (config.width + 15) & ~15u;
    config.height = (config.height + 15) & ~15u;
  }
#ifdef __ANDROID__
  if (gpu) config.output = VideoPixelFormat::Opaque;
#endif
  const auto bytes = std::size_t(config.width) * config.height *
      (config.output == VideoPixelFormat::Opaque ? 4 : 2);
  config.limits.output_bytes = std::max(config.limits.output_bytes, bytes * 34);
  return config;
}

struct DecoderFeed {
  std::size_t next = 0;
  bool sealed = false;
  bool Pump(VideoDecoder& decoder, const Clip& clip) {
    if (next < clip.packets.size()) {
      if (Take(decoder.TrySubmit(clip.packets[next])) == SubmitStatus::WouldBlock) return false;
      ++next;
      return true;
    }
    if (sealed) return false;
    Take(decoder.Finish());
    sealed = true;
    return true;
  }
};

void SetTransform(VideoFrame& frame, bool rotate) {
  auto& area = frame.format.visible;
  Check(area.width > 16 && area.height > 16, "Video fixture is too small to crop");
  area.x += 8;
  area.y += 8;
  area.width -= 16;
  area.height -= 16;
  if (rotate) frame.format.rotation = VideoRotation::Clockwise90;
}

VideoFrame ScaleCpu(const VideoFrame& source, unsigned width, unsigned height) {
  auto format = source.format;
  Check(format.pixel_format == VideoPixelFormat::NV12, "CPU baseline expects NV12");
  const auto area = format.visible;
  const bool rotate = format.rotation == VideoRotation::Clockwise90;
  format.width = width;
  format.height = height;
  format.visible = {0, 0, width, height};
  format.rotation = VideoRotation::None;
  const auto luma = std::size_t(width) * height;
  auto data = std::make_shared<Bytes>(luma * 3 / 2);
  for (unsigned plane = 0; plane < 2; ++plane) {
    const auto step = plane ? 2u : 1u;
    const auto& pixels = source.planes[plane];
    const auto bytes = pixels.data.AsBytes();
    auto* target = data->data() + (plane ? luma : 0);
    for (unsigned y = 0; y < height / step; ++y) {
      for (unsigned x = 0; x < width / step; ++x) {
        const auto u = rotate ? (2 * y + 1) * step * area.width / (2 * height) :
                               (2 * x + 1) * step * area.width / (2 * width);
        const auto v = rotate ? area.height - 1 - (2 * x + 1) * step * area.height / (2 * width) :
                               (2 * y + 1) * step * area.height / (2 * height);
        const auto offset = (area.y + v) / step * pixels.row_stride + (area.x + u) / step * pixels.pixel_stride;
        std::copy_n(bytes.data() + offset, step, target);
        target += step;
      }
    }
  }
  BufferReference pixels(*data, data);
  return {format, source.pts, source.duration, {{pixels.Slice(0, luma), width, 1}, {pixels.Slice(luma, luma / 2), width, 2}}};
}

void SetDuration(VideoFrame& frame, const Clip& clip, std::size_t index) {
  Check(index < clip.times.size() && frame.pts == clip.times[index], "Decode changed presentation order or PTS");
  if (index + 1 == clip.times.size()) return;
  const auto clock = *clip.track.timescale;
  frame.duration = MediaTimestamp{clip.times[index + 1].Rescale(clock)->ticks - frame.pts.Rescale(clock)->ticks, clock};
}

VideoFrame ReadPicture(VideoDecoder& decoder, const Clip& clip, DecoderFeed& feed) {
  const auto deadline = std::chrono::steady_clock::now() + 15s;
  while (std::chrono::steady_clock::now() < deadline) {
    if (auto output = Take(decoder.TryRead()))
      if (auto* frame = std::get_if<VideoFrame>(&*output)) return std::move(*frame);
    if (!feed.Pump(decoder, clip)) std::this_thread::sleep_for(1ms);
  }
  throw std::runtime_error("Timed out waiting for a picture");
}

VideoFrame FirstPicture(VideoDecoder& decoder, const Clip& clip, std::size_t first = 0) {
  DecoderFeed feed{.next = first};
  return ReadPicture(decoder, clip, feed);
}

void CheckPixels(const Clip& source, const Clip& exported, unsigned width, unsigned height) {
  TestSession<VideoDecoder> input(DecoderConfig(source.track, false));
  TestSession<VideoDecoder> output(DecoderConfig(exported.track, false));
  auto frame = FirstPicture(input.codec, source);
  SetTransform(frame, true);
  const auto expected = ScaleCpu(frame, width, height);
  const auto actual = FirstPicture(output.codec, exported);
  unsigned sum = 0;
  const auto& luma = actual.planes.front();
  for (unsigned y = 1; y <= 7; ++y)
    for (unsigned x = 1; x <= 7; ++x) {
      const auto px = x * width / 8, py = y * height / 8;
      const auto a = std::to_integer<int>(luma.data.AsBytes()[(py + actual.format.visible.y) * luma.row_stride +
          (px + actual.format.visible.x) * luma.pixel_stride]);
      const auto b = std::to_integer<int>(expected.planes.front().data.AsBytes()[py * width + px]);
      sum += std::abs(a - b);
    }
  Check(sum < 49 * 18, "Readback pixels differ from the expected crop and rotation");
}

double ProcessCpuSeconds() {
#ifdef _WIN32
  FILETIME created{}, exited{}, kernel{}, user{};
  Check(GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user), "Cannot read process CPU time");
  const auto ticks = [](const FILETIME& time) {
    return (std::uint64_t(time.dwHighDateTime) << 32) | time.dwLowDateTime;
  };
  return double(ticks(kernel) + ticks(user)) / 10'000'000;
#else
  return double(std::clock()) / CLOCKS_PER_SEC;
#endif
}

double PeakMemoryMiB() {
#ifdef _WIN32
  PROCESS_MEMORY_COUNTERS memory{};
  Check(GetProcessMemoryInfo(GetCurrentProcess(), &memory, sizeof(memory)), "Cannot read process memory usage");
  return double(memory.PeakWorkingSetSize) / (1024 * 1024);
#else
  rusage usage{};
  Check(getrusage(RUSAGE_SELF, &usage) == 0, "Cannot read process memory usage");
#ifdef __APPLE__
  return double(usage.ru_maxrss) / (1024 * 1024);
#else
  return double(usage.ru_maxrss) / 1024;
#endif
#endif
}

#ifdef __ANDROID__
template<class Work> void WithTestEgl(Work work) {
  const auto display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  const EGLint config_attributes[]{EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
      EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE};
  const EGLint context_attributes[]{EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
  const EGLint surface_attributes[]{EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
  EGLConfig config{};
  EGLint count = 0;
  Check(display != EGL_NO_DISPLAY && eglInitialize(display, nullptr, nullptr) &&
        eglChooseConfig(display, config_attributes, &config, 1, &count) && count && eglBindAPI(EGL_OPENGL_ES_API),
        "Cannot open the Android texture consumer EGL display");
  struct Consumer {
    EGLDisplay display;
    EGLContext context;
    EGLSurface surface;
    ~Consumer() {
      eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
      if (context != EGL_NO_CONTEXT) eglDestroyContext(display, context);
      if (surface != EGL_NO_SURFACE) eglDestroySurface(display, surface);
    }
  } consumer{display, eglCreateContext(display, config, EGL_NO_CONTEXT, context_attributes),
             eglCreatePbufferSurface(display, config, surface_attributes)};
  Check(consumer.context != EGL_NO_CONTEXT && consumer.surface != EGL_NO_SURFACE &&
        eglMakeCurrent(display, consumer.surface, consumer.surface, consumer.context),
        "Cannot activate the Android texture consumer EGL context");
  work();
}

void CheckGlImport(const android::GlFrame& frame) {
  WithTestEgl([&] {
    const auto name = frame.ImportCurrent();
    Check(glIsTexture(name), "The SDK snapshot did not import as a GLES texture");
    glDeleteTextures(1, &name);
  });
}
#endif

void CheckTexture(const ExternalTexture& texture, const VideoFormat& format) {
  Check(texture.IntrinsicSize() == huxerui::Size{float(format.width), float(format.height)} && texture.Revision() == 1,
        "The texture changed the coded geometry or lost its published frame");
#ifdef __APPLE__
#if TARGET_OS_IOS
  using Texture = huxerui::ios::PixelBufferTexture;
#else
  using Texture = huxerui::macos::PixelBufferTexture;
#endif
  const auto* sdk = dynamic_cast<const Texture*>(&texture);
  const std::unique_ptr<std::remove_pointer_t<CVPixelBufferRef>, decltype(&CVPixelBufferRelease)>
      frame(sdk ? sdk->AcquireFrame() : nullptr, CVPixelBufferRelease);
  Check(frame && CVPixelBufferGetWidth(frame.get()) == format.width && CVPixelBufferGetHeight(frame.get()) == format.height,
        "The SDK pixel buffer is unavailable or has the wrong dimensions after decoder close");
#else
#ifdef _WIN32
  using Texture = windows::D3D11Texture;
#else
  using Texture = android::GlTexture;
#endif
  const auto* sdk = dynamic_cast<const Texture*>(&texture);
  const auto frame = sdk ? sdk->AcquireFrame() : nullptr;
  Check(frame && frame->PixelWidth() == int(format.width) && frame->PixelHeight() == int(format.height),
        "The SDK GPU snapshot is unavailable or has the wrong dimensions after decoder close");
#ifdef __ANDROID__
  CheckGlImport(*frame);
#endif
#endif
}

std::array<VideoFrame, 2> WatermarkFrames(const VideoFormat& format) {
#ifdef __APPLE__
#if TARGET_OS_IOS
  using Texture = huxerui::ios::PixelBufferTexture;
#else
  using Texture = huxerui::macos::PixelBufferTexture;
#endif
#elif defined(__ANDROID__)
  using Texture = android::GlTexture;
#else
  using Texture = windows::D3D11Texture;
#endif
  const auto width = format.width, height = format.height;
  auto texture = std::make_shared<Texture>(huxerui::Size{float(width) / 2, float(height) / 2});
  const auto empty = MakeVideoFrame(texture, format, {0, 1});
  Check(!empty.Succeeded() && empty.Error().code == CodecErrorCode::InvalidState, "An unpublished texture yielded a frame");
  std::array<VideoFrame, 2> frames;
  const auto acquire = [&](unsigned index) {
    auto wrong_size = format;
    wrong_size.width += 2;
    const auto wrong = MakeVideoFrame(texture, wrong_size, {0, 1});
    Check(!wrong.Succeeded() && wrong.Error().code == CodecErrorCode::InvalidArgument, "Texture pixel dimensions were not checked");
    auto result = MakeVideoFrame(texture, format, {index, 1});
#ifdef _WIN32
    if (!result.Succeeded() && result.Error().code == CodecErrorCode::Unsupported &&
        result.Error().platform_code == std::to_string(E_NOTIMPL)) {
      std::cout << result.Error().message << '\n';
      throw MissingHardware{};
    }
#endif
    frames[index] = Take(std::move(result));
    Check(frames[index].Validate() && frames[index].planes.empty(), "Texture input exposed invalid storage");
  };
#ifdef __APPLE__
  @autoreleasepool {
    const auto device = MTLCreateSystemDefaultDevice();
    const auto queue = [device newCommandQueue];
    NSString* source = @R"metal(
      #include <metal_stdlib>
      using namespace metal;
      kernel void mark(texture2d<float, access::write> y [[texture(0)]],
                       texture2d<float, access::write> uv [[texture(1)]],
                       constant uint& index [[buffer(0)]], uint2 p [[thread_position_in_grid]]) {
        const bool inside = p.x < y.get_width() / 2 && (p.y < y.get_height() / 2) == (index == 0);
        y.write(float4(inside ? 0.85 : 0.2), p);
        if (p.x < uv.get_width() && p.y < uv.get_height()) uv.write(float4(0.5), p);
      }
    )metal";
    const auto library = [device newLibraryWithSource:source options:nil error:nil];
    const auto function = [library newFunctionWithName:@"mark"];
    const auto pipeline = function ? [device newComputePipelineStateWithFunction:function error:nil] : nil;
    Check(queue && pipeline, "Cannot open the Metal watermark pipeline");
    CVMetalTextureCacheRef cache = nullptr;
    Check(CVMetalTextureCacheCreate(nullptr, nullptr, device, nullptr, &cache) == kCVReturnSuccess, "Cannot create the test Metal cache");
    const std::unique_ptr<std::remove_pointer_t<CVMetalTextureCacheRef>, decltype(&CFRelease)> cache_owner(cache, CFRelease);
    for (unsigned index = 0; index < frames.size(); ++index) {
      CVPixelBufferRef buffer = nullptr;
      NSDictionary* attributes = @{(__bridge NSString*)kCVPixelBufferIOSurfacePropertiesKey: @{},
                                   (__bridge NSString*)kCVPixelBufferMetalCompatibilityKey: @YES};
      Check(CVPixelBufferCreate(nullptr, width, height, kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
          (__bridge CFDictionaryRef)attributes, &buffer) == kCVReturnSuccess, "Cannot allocate the NV12 watermark image");
      const std::unique_ptr<std::remove_pointer_t<CVPixelBufferRef>, decltype(&CVPixelBufferRelease)> owner(buffer, CVPixelBufferRelease);
      const auto command = [queue commandBuffer];
      const auto compute = [command computeCommandEncoder];
      [compute setComputePipelineState:pipeline];
      using Plane = std::unique_ptr<std::remove_pointer_t<CVMetalTextureRef>, decltype(&CFRelease)>;
      std::array planes{Plane(nullptr, CFRelease), Plane(nullptr, CFRelease)};
      for (unsigned plane = 0; plane < planes.size(); ++plane) {
        CVMetalTextureRef image = nullptr;
        NSDictionary* options = @{(__bridge NSString*)kCVMetalTextureUsage: @(MTLTextureUsageShaderWrite)};
        Check(CVMetalTextureCacheCreateTextureFromImage(nullptr, cache, buffer, (__bridge CFDictionaryRef)options,
            plane ? MTLPixelFormatRG8Unorm : MTLPixelFormatR8Unorm, width >> plane, height >> plane, plane, &image) == kCVReturnSuccess,
            "Cannot map the watermark plane to Metal");
        planes[plane].reset(image);
        [compute setTexture:CVMetalTextureGetTexture(image) atIndex:plane];
      }
      [compute setBytes:&index length:sizeof(index) atIndex:0];
      [compute dispatchThreads:MTLSizeMake(width, height, 1) threadsPerThreadgroup:MTLSizeMake(16, 8, 1)];
      [compute endEncoding];
      [command commit];
      [command waitUntilCompleted];
      Check(command.status == MTLCommandBufferStatusCompleted, "Metal watermark did not complete");
      texture->Publish(buffer);
      acquire(index);
    }
  }
#elif defined(_WIN32)
  using Microsoft::WRL::ComPtr;
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  Check(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
      nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context)), "Cannot open the D3D11 watermark producer device");
  struct Target {
    ComPtr<ID3D11Texture2D> image;
    ComPtr<ID3D11RenderTargetView> view;
  };
  const auto target = [&](UINT w, UINT h) {
    D3D11_TEXTURE2D_DESC description{};
    description.Width = w;
    description.Height = h;
    description.MipLevels = description.ArraySize = description.SampleDesc.Count = 1;
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    description.BindFlags = D3D11_BIND_RENDER_TARGET;
    Target result;
    Check(SUCCEEDED(device->CreateTexture2D(&description, nullptr, &result.image)) &&
          SUCCEEDED(device->CreateRenderTargetView(result.image.Get(), nullptr, &result.view)),
          "Cannot allocate the D3D11 watermark texture");
    return result;
  };
  const auto image = target(width, height), marker = target(width / 2, height / 2);
  const float green[]{0.2f, 0.9f, 0.25f, 1.0f}, gray[]{0.2f, 0.2f, 0.2f, 1.0f};
  context->ClearRenderTargetView(marker.view.Get(), green);
  for (unsigned index = 0; index < frames.size(); ++index) {
    context->ClearRenderTargetView(image.view.Get(), gray);
    context->CopySubresourceRegion(image.image.Get(), 0, 0, index * height / 2, 0, marker.image.Get(), 0, nullptr);
    texture->Publish({image.image.Get(), Texture::Alpha::Opaque});
    acquire(index);
  }
#else
  WithTestEgl([&] {
    struct Target {
      GLuint image = 0, framebuffer = 0;
      ~Target() { glDeleteFramebuffers(1, &framebuffer); glDeleteTextures(1, &image); }
    };
    for (unsigned index = 0; index < frames.size(); ++index) {
      Target target;
      glGenTextures(1, &target.image);
      glBindTexture(GL_TEXTURE_2D, target.image);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
      glGenFramebuffers(1, &target.framebuffer);
      glBindFramebuffer(GL_FRAMEBUFFER, target.framebuffer);
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target.image, 0);
      Check(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "Cannot render the GLES watermark");
      glClearColor(0.2f, 0.2f, 0.2f, 1);
      glClear(GL_COLOR_BUFFER_BIT);
      glEnable(GL_SCISSOR_TEST);
      glScissor(0, index == 0 ? height / 2 : 0, width / 2, height / 2);
      glClearColor(0.85f, 0.85f, 0.85f, 1);
      glClear(GL_COLOR_BUFFER_BIT);
      glDisable(GL_SCISSOR_TEST);
      Check(glGetError() == GL_NO_ERROR, "Cannot paint the GLES watermark");
      texture->PublishCurrent({target.image, int(width), int(height), -1, Texture::Origin::BottomLeft, Texture::Alpha::Opaque});
      acquire(index);
    }
  });
#endif
  const std::weak_ptr<ExternalTexture> source = texture;
  texture.reset();
  Check(!source.expired(), "Acquired frames did not retain the texture owner");
  return frames;
}

void CheckWatermark(const Clip& clip) {
  TestSession<VideoDecoder> decoder(DecoderConfig(clip.track, false));
  DecoderFeed feed;
  for (const bool lower : {false, true}) {
    const auto frame = ReadPicture(decoder.codec, clip, feed);
    const auto area = frame.format.visible;
    const auto& y = frame.planes.front();
    const auto sample = [&](unsigned x, unsigned row) {
      return std::to_integer<int>(y.data.AsBytes()[(area.y + row) * y.row_stride + (area.x + x) * y.pixel_stride]);
    };
    const auto mark_row = (lower ? 3u : 1u) * area.height / 4;
    const auto opposite_row = (lower ? 1u : 3u) * area.height / 4;
    const auto mark = sample(area.width / 4, mark_row);
    Check(mark > sample(area.width * 3 / 4, mark_row) + 80 && mark > sample(area.width / 4, opposite_row) + 80,
          "An encoded picture lost or flipped the captured texture watermark");
#ifdef _WIN32
    const auto& chroma = frame.planes[1];
    const auto x = (area.x + area.width / 4) / 2, row = (area.y + mark_row) / 2;
    const auto bytes = chroma.data.AsBytes().subspan(row * chroma.row_stride + x * chroma.pixel_stride);
    Check(std::to_integer<int>(bytes[0]) < 112 && std::to_integer<int>(bytes[1]) < 112,
          "D3D11 texture conversion changed the green watermark's chroma");
#endif
  }
}

void Transcode(const Clip& clip, const File& file, unsigned width, unsigned height, bool gpu, bool regression,
    std::uint32_t bitrate, bool texture_input) {
#ifdef _WIN32
  constexpr bool allow_pts_as_dts = true;
#else
  constexpr bool allow_pts_as_dts = false;
#endif
#if defined(_WIN32) || (defined(__APPLE__) && TARGET_OS_IOS && TARGET_OS_SIMULATOR)
  const bool reorder = clip.track.descriptions.front().codec == CodecId::Hevc;
#else
  constexpr bool reorder = false;
#endif
  auto decoder_config = DecoderConfig(clip.track, gpu);
  VideoFormat watermark_format{width, height, {0, 0, width, height}, decoder_config.output};
#ifdef _WIN32
  if (texture_input) {
    watermark_format.matrix = ColorMatrix::BT709;
    watermark_format.range = ColorRange::Limited;
  }
  if (gpu) {
    if (!texture_input && !CodecAvailable<VideoDecoder>(decoder_config)) throw MissingHardware{};
    VideoEncoderConfig config{.input = watermark_format, .bitrate = bitrate,
        .profile = H264Profile::Baseline, .codec = clip.track.descriptions.front().codec, .input_storage = VideoStorage::Platform};
    if (reorder) config.max_b_frames = std::nullopt;
    if (!CodecAvailable<VideoEncoder>(config)) throw MissingHardware{};
  }
#else
  PixelAudit audit;
#endif
  const auto start = std::chrono::steady_clock::now();
  const auto start_cpu = ProcessCpuSeconds();
  const auto source_clock = *clip.track.timescale;
  const auto wide_clock = reorder ? std::lcm(std::uint64_t(source_clock), 1'000'000ull) : source_clock;
  Check(wide_clock <= UINT32_MAX, "Video timestamp grids have no compatible clock");
  const auto clock = std::uint32_t(wide_clock);
  std::optional<TestSession<VideoDecoder>> source;
  if (!texture_input) source.emplace(decoder_config);
  media::detail::VideoFrameTransformer transformer;
  std::optional<TestSession<VideoEncoder>> encoder;
  std::optional<MediaMuxer> mux;
  std::optional<bool> platform_dts;
  std::optional<std::int64_t> next_dts;
  std::optional<VideoFrame> pending, retained;
  std::optional<std::array<VideoFrame, 2>> watermarks;
  if (texture_input) watermarks = WatermarkFrames(watermark_format);
  std::shared_ptr<ExternalTexture> texture;
  DecoderFeed feed;
  std::size_t decoded = 0, encoded = 0;
  bool decode_end = false, sealed = false, ended = false;
  auto deadline = start + 25s;
  while (!ended && std::chrono::steady_clock::now() < deadline) {
    bool progress = false;
    if (encoder) {
      while (auto output = Take(encoder->codec.TryRead())) {
        progress = true;
        if (const auto* format = std::get_if<VideoEncodeFormat>(&*output)) {
          const MediaTrack track{.id = 1, .kind = TrackKind::Video, .timescale = clock,
              .descriptions = {{.codec = format->codec, .framing = PacketFraming::LengthPrefixedNals,
                  .initialization_data = format->initialization_data, .coded_width = format->width,
                  .coded_height = format->height, .visible = format->visible}}};
          mux.emplace(Take(MediaMuxer::Open(Take(file.OpenRandomAccess(FileAccessMode::WriteOnly, FileOpenMode::CreateNew)),
              ContainerFormat::Mp4, std::span(&track, 1))));
        } else if (const auto* packet = std::get_if<EncodedPacket>(&*output)) {
          Check(mux && encoded < clip.times.size() && (reorder || packet->pts == clip.times[encoded]) && packet->duration,
                "Encoder changed output count, PTS, or duration");
          ContainerPacket sample{1, packet->data, packet->pts, reorder ? packet->dts : std::optional{packet->pts},
              packet->duration, packet->key_frame};
          if (reorder) {
            if (!platform_dts) platform_dts = sample.dts.has_value();
            Check(*platform_dts == sample.dts.has_value(), "Platform decode timestamp availability changed");
            if (!sample.dts) {
              // Some Windows encoders use low-delay B pictures with the same decode and presentation order.
              Check(allow_pts_as_dts && (!next_dts || packet->pts == MediaTimestamp{*next_dts, clock}),
                    "Reordered encoder output has no decode timestamp");
              sample.dts = sample.pts;
            }
            if (!next_dts) {
              const auto exact = sample.dts->Rescale(source_clock);
              if (exact && exact->Rescale(sample.dts->timescale) == sample.dts) sample.dts = exact;
              next_dts = sample.dts->Rescale(clock)->ticks;
            }
            sample.dts = MediaTimestamp{*next_dts, clock};
            const auto length = sample.duration->Rescale(clock)->ticks;
            Check(length > 0 && *next_dts <= INT64_MAX - length, "Invalid encoded video duration");
            *next_dts += length;
          }
          Check(!Take(mux->TryWrite(sample)), "Muxer requested a different track");
          ++encoded;
        } else {
          ended = true;
        }
      }
    }
    if (!pending && !decode_end) {
      if (watermarks) {
        decode_end = decoded == clip.times.size();
        if (!decode_end) {
          pending = (*watermarks)[decoded % watermarks->size()];
          pending->pts = clip.times[decoded];
          if (decoded + 1 == clip.times.size()) {
            const auto last = std::ranges::find(clip.packets, pending->pts, &EncodedPacket::pts);
            Check(last != clip.packets.end(), "The fixture has no last presentation packet");
            pending->duration = last->duration;
          }
          SetDuration(*pending, clip, decoded++);
        }
      } else if (auto result = Take(source->codec.TryRead())) {
        progress = true;
        if (auto* frame = std::get_if<VideoFrame>(&*result)) {
          Check(frame->Validate() && frame->Storage() == decoder_config.output_storage &&
                (gpu ? frame->planes.empty() : !frame->planes.empty()),
                "Decoder returned the wrong frame storage");
          SetDuration(*frame, clip, decoded++);
          SetTransform(*frame, regression);
          if (gpu && regression && !retained) {
            retained = *frame;
            texture = Take(MakeExternalTexture(*frame));
          }
          pending = gpu ? Take(transformer.Transform(*frame, width, height)) : ScaleCpu(*frame, width, height);
        } else if (std::holds_alternative<EndOfStream>(*result)) {
          decode_end = true;
        }
      }
      if (pending) {
        Check(pending->Validate() && pending->Storage() == decoder_config.output_storage &&
              (gpu ? pending->planes.empty() : !pending->planes.empty()), "Unexpected encoder input frame storage");
        if (!encoder) {
          VideoEncoderConfig config{.input = pending->format, .bitrate = bitrate, .profile = H264Profile::Baseline,
              .codec = clip.track.descriptions.front().codec, .input_storage = decoder_config.output_storage};
#ifdef __APPLE__
          if (gpu && regression && clip.track.descriptions.front().codec == CodecId::H264) config.limits.input_items = 1;
#endif
          if (reorder) config.max_b_frames = std::nullopt;
          encoder.emplace(config);
        }
      }
    }
    if (pending && Take(encoder->codec.TrySubmit(*pending)) == SubmitStatus::Accepted) {
      progress = true;
      pending.reset();
    }
    if (decode_end && !pending && !sealed) {
      Check(encoder.has_value(), "No frames reached the encoder");
      Take(encoder->codec.Finish());
      sealed = true;
      progress = true;
    }
    if (source && !decode_end) progress |= feed.Pump(source->codec, clip);
    if (progress) deadline = std::chrono::steady_clock::now() + 25s;
    else std::this_thread::sleep_for(1ms);
  }
  Check(ended && mux && decoded == clip.times.size() && encoded == decoded, "Video transcode stalled or changed frame count");
  Take(mux->Finish());
  const auto wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  const auto cpu = ProcessCpuSeconds() - start_cpu;
  const auto rss = PeakMemoryMiB();
  if (retained) {
    for (const auto target : {clip.packets.size() / 2, std::size_t(0)}) {
      auto first = target;
      while (first && !clip.packets[first].key_frame) --first;
      Take(source->codec.Reset());
      const auto picture = FirstPicture(source->codec, clip, first);
      Check(picture.Storage() == VideoStorage::Platform && picture.pts == clip.packets[first].pts,
            "Reset did not return a picture from the new key frame");
    }
  }
  Check((!source || source->Close()) && encoder->Close(), "Codec did not retire with a retained image");
  if (retained) {
    const auto format = retained->format;
    const auto image = Take(transformer.Transform(*retained, width, height));
    Check(image.Validate(), "Retained GPU image became unusable after close");
    retained.reset();
    CheckTexture(*texture, format);
  }
#ifndef _WIN32
  audit_pixels = false;
  Check(gpu == (cpu_pixel_accesses == 0), "The pixel audit did not distinguish the GPU and CPU paths");
#endif
  std::cout << (gpu ? "GPU" : "CPU") << " " << decoded << " frames wall_s=" << wall
            << " cpu_s=" << cpu << " peak_rss_mib=" << rss;
#ifndef _WIN32
  std::cout << " cpu_pixel_accesses=" << cpu_pixel_accesses;
#endif
  std::cout << '\n';
  const auto readback = ReadClip(file);
  const auto& description = readback.track.descriptions.front();
  Check(readback.times.size() == clip.times.size(), "MP4 readback changed the frame count");
  std::vector<MediaPresentationSlice> slices;
  for (std::size_t i = 0; i < readback.times.size(); ++i) {
    const auto pts = readback.times[i];
    Take(MapToPresentation(readback.track.edits, pts, {pts.ticks + 1, pts.timescale}, slices));
    Check(slices.size() == 1 && slices.front().presentation.start == clip.times[i], "MP4 readback changed picture time");
  }
  Check(description.codec == clip.track.descriptions.front().codec &&
        description.visible.value_or(VideoRectangle{0, 0, *description.coded_width, *description.coded_height}) ==
            VideoRectangle{0, 0, width, height}, "MP4 readback changed codec, visible geometry, or PTS");
  std::optional<MediaTimestamp> previous_dts;
  for (const auto& packet : readback.packets) {
    Check(packet.dts && (!previous_dts || packet.dts->Compare(*previous_dts) > 0) &&
        (platform_dts.value_or(false) || packet.dts == packet.pts) &&
        packet.duration && packet.duration->ticks > 0, "MP4 readback lost decode timing");
    previous_dts = packet.dts;
  }
  if (texture_input) {
#ifdef _WIN32
    if (readback.track.descriptions.front().codec == CodecId::Hevc && !CodecAvailable<VideoDecoder>(DecoderConfig(readback.track, false))) {
      std::cout << "HEVC SDK texture encoding and MP4 timing passed. Watermark decode requires an HEVC decoder.\n";
      return;
    }
#endif
    CheckWatermark(readback);
    std::cout << "SDK texture watermark and MP4 timing passed\n";
  } else if (regression) {
    CheckPixels(clip, readback, width, height);
  }
}

} // namespace

#ifdef __APPLE__
extern "C" CVReturn CVPixelBufferLockBaseAddress(CVPixelBufferRef buffer, CVPixelBufferLockFlags flags) {
  static const auto lock = reinterpret_cast<decltype(&CVPixelBufferLockBaseAddress)>(
      dlsym(RTLD_NEXT, "CVPixelBufferLockBaseAddress"));
  if (audit_pixels) ++cpu_pixel_accesses;
  return lock ? lock(buffer, flags) : kCVReturnError;
}
#elif defined(__ANDROID__)
extern "C" AMediaCodec* __real_AMediaCodec_createDecoderByType(const char*);
extern "C" AMediaCodec* __wrap_AMediaCodec_createDecoderByType(const char* mime) {
  auto* codec = __real_AMediaCodec_createDecoderByType(mime);
  if (audit_pixels) audited_decoder = codec;
  return codec;
}

extern "C" std::uint8_t* __real_AMediaCodec_getOutputBuffer(AMediaCodec*, std::size_t, std::size_t*);
extern "C" std::uint8_t* __wrap_AMediaCodec_getOutputBuffer(AMediaCodec* codec, std::size_t index, std::size_t* size) {
  if (audit_pixels && codec == audited_decoder) ++cpu_pixel_accesses;
  return __real_AMediaCodec_getOutputBuffer(codec, index, size);
}

extern "C" void __real_glReadPixels(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*);
extern "C" void __wrap_glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void* pixels) {
  if (audit_pixels) ++cpu_pixel_accesses;
  __real_glReadPixels(x, y, width, height, format, type, pixels);
}
#endif

int main(int argc, char** argv) {
  if (argc != 4 && argc != 6 && argc != 7) {
    std::cerr << "usage: media_video_gpu_tests INPUT OUTPUT|- gpu|cpu|texture [WIDTH HEIGHT [BITRATE]]\n";
    return 2;
  }
  try {
    const auto clip = ReadClip(File(argv[1]));
    const std::string_view mode(argv[3]);
    const bool texture_input = mode == "texture";
#if defined(_WIN32) && defined(HUXERUI_WINDOWS_7_COMPAT)
    if (texture_input) throw MissingHardware{};
#endif
    const bool gpu = texture_input || mode == "gpu";
    Check(gpu || mode == "cpu", "Unknown video test mode");
    const bool temporary = std::string_view(argv[2]) == "-";
    const auto path = temporary ? std::filesystem::temp_directory_path() /
        ("lib-media-gpu-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".mp4") :
        std::filesystem::path(argv[2]);
    struct Cleanup {
      const std::filesystem::path& path;
      bool remove;
      ~Cleanup() { if (remove) { std::error_code error; std::filesystem::remove(path, error); } }
    } cleanup{path, temporary};
    const auto width = argc >= 6 ? unsigned(std::stoul(argv[4])) : 112u;
    const auto height = argc >= 6 ? unsigned(std::stoul(argv[5])) : 144u;
    const auto bitrate = argc == 7 ? unsigned(std::stoul(argv[6])) : 4'000'000u;
    Transcode(clip, File(path.u8string()), width, height, gpu, argc == 4 && !texture_input, bitrate, texture_input);
    return 0;
#ifdef _WIN32
  } catch (const MissingHardware&) {
    return 77;
#endif
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
