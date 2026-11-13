#include "media_video_macos.h"

#include "codec/common.h"
#include "format/platform_video_storage.h"
#include "format/video_frame_transformer.h"

#include <huxerui/macos/external_texture.h>

#import <CoreImage/CoreImage.h>
#import <CoreVideo/CoreVideo.h>
#import <Metal/Metal.h>

#include <cstring>

#include <stdexcept>

namespace huxerui::media {
namespace {

struct PixelBufferStorage final : detail::PlatformVideoStorage {
  PixelBufferStorage(CVPixelBufferRef buffer, const VideoFormat& format)
      : PlatformVideoStorage(format.width, format.height, format.pixel_format), buffer(CVPixelBufferRetain(buffer)) {}
  ~PixelBufferStorage() override { CVPixelBufferRelease(buffer); }
  CVPixelBufferRef buffer;
};

using MetalImage = std::unique_ptr<std::remove_pointer_t<CVMetalTextureRef>, decltype(&CFRelease)>;

class MetalVideoFrameTransformer final : public detail::VideoFrameTransformBackend {
public:
  ~MetalVideoFrameTransformer() override {
    if (pool_) CVPixelBufferPoolRelease(pool_);
    if (cache_) CFRelease(cache_);
  }

  CodecResult<VideoFrame> Transform(const VideoFrame& source, VideoFormat target) override {
    @autoreleasepool {
      if (!pipeline_) Open();
      const auto input = macos::PixelBuffer(source);
      if (!input) return CodecResult<VideoFrame>::Failure(detail::Error(CodecErrorCode::InvalidArgument, "Expected a macOS pixel buffer"));
      const auto pixel_type = CVPixelBufferGetPixelFormatType(input);
      if (!pool_ || width_ != target.width || height_ != target.height || pixel_type_ != pixel_type) {
        NSDictionary* attributes = @{
          (__bridge NSString*)kCVPixelBufferWidthKey: @(target.width),
          (__bridge NSString*)kCVPixelBufferHeightKey: @(target.height),
          (__bridge NSString*)kCVPixelBufferPixelFormatTypeKey: @(pixel_type),
          (__bridge NSString*)kCVPixelBufferMetalCompatibilityKey: @YES,
          (__bridge NSString*)kCVPixelBufferIOSurfacePropertiesKey: @{}
        };
        CVPixelBufferPoolRef replacement = nullptr;
        if (CVPixelBufferPoolCreate(nullptr, nullptr, (__bridge CFDictionaryRef)attributes, &replacement) != kCVReturnSuccess)
          throw std::runtime_error("Cannot create the Metal video buffer pool");
        if (pool_) CVPixelBufferPoolRelease(pool_);
        pool_ = replacement;
        width_ = target.width;
        height_ = target.height;
        pixel_type_ = pixel_type;
      }
      CVPixelBufferRef result = nullptr;
      if (CVPixelBufferPoolCreatePixelBuffer(nullptr, pool_, &result) != kCVReturnSuccess)
        throw std::runtime_error("Cannot allocate a Metal video buffer");
      const std::unique_ptr<std::remove_pointer_t<CVPixelBufferRef>, decltype(&CVPixelBufferRelease)>
          owned(result, CVPixelBufferRelease);
      CVBufferRemoveAllAttachments(result);
      CVBufferPropagateAttachments(input, result);
      CVBufferRemoveAttachment(result, kCVImageBufferCleanApertureKey);
      CVBufferRemoveAttachment(result, kCVImageBufferPixelAspectRatioKey);

      const auto luma = Texture(input, 0, MTLTextureUsageShaderRead);
      const auto chroma = Texture(input, 1, MTLTextureUsageShaderRead);
      const auto out_luma = Texture(result, 0, MTLTextureUsageShaderWrite);
      const auto out_chroma = Texture(result, 1, MTLTextureUsageShaderWrite);
      const auto coordinates = detail::VideoSampleCoordinates(source.format);
      id<MTLCommandBuffer> command = [queue_ commandBuffer];
      id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
      if (!encoder) throw std::runtime_error("Cannot open the Metal video compute pass");
      [encoder setComputePipelineState:pipeline_];
      [encoder setBytes:coordinates.data() length:sizeof(coordinates) atIndex:0];
      const auto dispatch = [&](const MetalImage& in, const MetalImage& out) {
        const auto destination = CVMetalTextureGetTexture(out.get());
        [encoder setTexture:CVMetalTextureGetTexture(in.get()) atIndex:0];
        [encoder setTexture:destination atIndex:1];
        [encoder dispatchThreads:MTLSizeMake(destination.width, destination.height, 1)
            threadsPerThreadgroup:MTLSizeMake(16, 8, 1)];
      };
      dispatch(luma, out_luma);
      dispatch(chroma, out_chroma);
      [encoder endEncoding];
      [command commit];
      [command waitUntilCompleted];
      if (command.status != MTLCommandBufferStatusCompleted)
        throw std::runtime_error("Metal could not complete the video transform");
      return macos::RetainPixelBuffer(result, target, source.pts, source.duration);
    }
  }

private:
  void Open() {
    auto device = MTLCreateSystemDefaultDevice();
    auto queue = [device newCommandQueue];
    if (!queue) throw std::runtime_error("No Metal device for the video transform");
    NSError* error = nil;
    NSString* source = @R"metal(
      #include <metal_stdlib>
      using namespace metal;
      kernel void resize_video(texture2d<float> input [[texture(0)]],
                               texture2d<float, access::write> output [[texture(1)]],
                               constant float2* uv [[buffer(0)]],
                               uint2 pixel [[thread_position_in_grid]]) {
        const uint2 size(output.get_width(), output.get_height());
        if (any(pixel >= size)) return;
        constexpr sampler filter(coord::normalized, address::clamp_to_edge, filter::linear);
        const float2 p = (float2(pixel) + 0.5) / float2(size);
        const float2 opposite = uv[0] + uv[1] + uv[2];
        const float2 half_texel = 0.5 / float2(input.get_width(), input.get_height());
        const float2 low = min(uv[0], opposite), high = max(uv[0], opposite);
        const float2 margin = min(half_texel, (high - low) * 0.5);
        const float2 sample = clamp(uv[0] + p.x * uv[1] + p.y * uv[2], low + margin, high - margin);
        output.write(input.sample(filter, sample), pixel);
      }
    )metal";
    const auto library = [device newLibraryWithSource:source options:nil error:&error];
    const auto function = [library newFunctionWithName:@"resize_video"];
    if (!function) throw std::runtime_error(error.localizedDescription.UTF8String ?: "Cannot compile the Metal video scaler");
    auto pipeline = [device newComputePipelineStateWithFunction:function error:&error];
    if (!pipeline) throw std::runtime_error(error.localizedDescription.UTF8String ?: "Cannot create the Metal video pipeline");
    if (CVMetalTextureCacheCreate(nullptr, nullptr, device, nullptr, &cache_) != kCVReturnSuccess)
      throw std::runtime_error("Cannot open the Metal video texture cache");
    queue_ = queue;
    pipeline_ = pipeline;
  }

  MetalImage Texture(CVPixelBufferRef buffer, std::size_t plane, MTLTextureUsage usage) const {
    CVMetalTextureRef value = nullptr;
    NSDictionary* options = @{(__bridge NSString*)kCVMetalTextureUsage: @(usage)};
    const auto status = CVMetalTextureCacheCreateTextureFromImage(nullptr, cache_, buffer,
        (__bridge CFDictionaryRef)options, plane ? MTLPixelFormatRG8Unorm : MTLPixelFormatR8Unorm,
        CVPixelBufferGetWidthOfPlane(buffer, plane), CVPixelBufferGetHeightOfPlane(buffer, plane), plane, &value);
    if (status != kCVReturnSuccess || !value) throw std::runtime_error("Cannot import a video plane into Metal");
    return MetalImage(value, CFRelease);
  }

  id<MTLCommandQueue> queue_;
  id<MTLComputePipelineState> pipeline_;
  CVMetalTextureCacheRef cache_ = nullptr;
  CVPixelBufferPoolRef pool_ = nullptr;
  std::uint32_t width_ = 0, height_ = 0;
  OSType pixel_type_ = 0;
};

} // namespace

namespace macos {

CodecResult<VideoFrame> RetainPixelBuffer(CVPixelBufferRef image, VideoFormat format,
    MediaTimestamp pts, std::optional<MediaTimestamp> duration) {
  using R = CodecResult<VideoFrame>;
  const auto type = image ? CVPixelBufferGetPixelFormatType(image) : 0;
  const auto range = type == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange ? ColorRange::Full : ColorRange::Limited;
  if (!image || (type != kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange &&
                 type != kCVPixelFormatType_420YpCbCr8BiPlanarFullRange) ||
      format.pixel_format != VideoPixelFormat::NV12 || !CVPixelBufferGetIOSurface(image) ||
      CVPixelBufferGetWidth(image) != format.width || CVPixelBufferGetHeight(image) != format.height ||
      CVPixelBufferGetPlaneCount(image) != 2)
    return R::Failure(detail::Error(CodecErrorCode::Unsupported, "Expected an IOSurface-backed NV12 buffer"));
  if (format.range != ColorRange::Unknown && format.range != range)
    return R::Failure(detail::Error(CodecErrorCode::InvalidArgument, "Pixel buffer range differs from the video format"));
  auto frame = detail::VideoFrameAccess::FromPlatform(format, pts, duration,
      std::make_shared<PixelBufferStorage>(image, format));
  if (!frame.Validate()) return R::Failure(detail::Error(CodecErrorCode::InvalidArgument, "Invalid platform video frame metadata"));
  return R::Success(std::move(frame));
}

CVPixelBufferRef PixelBuffer(const VideoFrame& frame) noexcept {
  const auto* storage = dynamic_cast<const PixelBufferStorage*>(detail::VideoFrameAccess::PlatformStorage(frame).get());
  return storage ? storage->buffer : nullptr;
}

} // namespace macos

CodecResult<VideoFrame> detail::AcquirePlatformVideoFrame(const ExternalTexture& texture, VideoFormat format,
    MediaTimestamp pts, std::optional<MediaTimestamp> duration) {
  using R = CodecResult<VideoFrame>;
  const auto* source = dynamic_cast<const huxerui::macos::PixelBufferTexture*>(&texture);
  if (!source) return R::Failure(Error(CodecErrorCode::Unsupported, "Expected an SDK pixel buffer texture"));
  const std::unique_ptr<std::remove_pointer_t<CVPixelBufferRef>, decltype(&CVPixelBufferRelease)>
      image(source->AcquireFrame(), CVPixelBufferRelease);
  if (!image) return R::Failure(Error(CodecErrorCode::InvalidState, "The texture has no published video frame"));
  if (CVPixelBufferGetWidth(image.get()) != format.width || CVPixelBufferGetHeight(image.get()) != format.height)
    return R::Failure(Error(CodecErrorCode::InvalidArgument, "Pixel buffer dimensions differ from the video format"));
  return macos::RetainPixelBuffer(image.get(), format, pts, duration);
}

CodecResult<std::shared_ptr<ExternalTexture>> detail::MakePlatformVideoTexture(const VideoFrame& frame) {
  using R = CodecResult<std::shared_ptr<ExternalTexture>>;
  const auto image = macos::PixelBuffer(frame);
  if (!image) return R::Failure(Error(CodecErrorCode::Unsupported, "Expected an IOSurface-backed pixel buffer"));
  struct TextureLease {
    TextureLease(const VideoFrame& frame)
        : source(VideoFrameAccess::PlatformStorage(frame)), texture(Size{float(frame.format.width), float(frame.format.height)}) {}
    std::shared_ptr<const PlatformVideoStorage> source;
    huxerui::macos::PixelBufferTexture texture;
  };
  auto lease = std::make_shared<TextureLease>(frame);
  lease->texture.Publish(image);
  lease->texture.Finish();
  return R::Success(std::shared_ptr<ExternalTexture>(lease, &lease->texture));
}

namespace {

using PixelBufferOwner = std::unique_ptr<std::remove_pointer_t<CVPixelBufferRef>, decltype(&CVPixelBufferRelease)>;

PixelBufferOwner AllocateTextureBuffer(VideoTextureOptions options) {
  CVPixelBufferRef buffer = nullptr;
  NSDictionary* attributes = @{
    (__bridge NSString*)kCVPixelBufferIOSurfacePropertiesKey: @{},
    (__bridge NSString*)kCVPixelBufferMetalCompatibilityKey: @YES
  };
  if (CVPixelBufferCreate(nullptr, options.width, options.height, kCVPixelFormatType_32BGRA,
      (__bridge CFDictionaryRef)attributes, &buffer) != kCVReturnSuccess)
    throw std::runtime_error("Cannot allocate a BGRA video texture buffer");
  return PixelBufferOwner(buffer, CVPixelBufferRelease);
}

detail::VideoTextureResult PublishTextureBuffer(CVPixelBufferRef buffer, VideoTextureOptions options) {
  auto texture = std::make_shared<huxerui::macos::PixelBufferTexture>(Size{float(options.width), float(options.height)});
  texture->Publish(buffer);
  texture->Finish();
  return detail::VideoTextureResult::Success(std::move(texture));
}

} // namespace

detail::VideoTextureResult detail::MakeFittedPlatformVideoTexture(const VideoFrame& frame,
    VideoTextureOptions options, const VideoTextureGeometry& geometry) {
  const auto source = macos::PixelBuffer(frame);
  if (!source) return VideoTextureResult::Failure(Error(CodecErrorCode::Unsupported, "Expected an IOSurface video image"));
  const auto coded = CGRectMake(0, 0, frame.format.width, frame.format.height);
  if (geometry.identity && CGRectEqualToRect(CVImageBufferGetCleanRect(source), coded) &&
      CGSizeEqualToSize(CVImageBufferGetDisplaySize(source), coded.size))
    return MakePlatformVideoTexture(frame);
  @autoreleasepool {
    static CIContext* const context = [] {
      auto device = MTLCreateSystemDefaultDevice();
      return device ? [CIContext contextWithMTLDevice:device options:@{kCIContextCacheIntermediates: @NO}] : nil;
    }();
    if (!context) return VideoTextureResult::Failure(Error(CodecErrorCode::Unsupported, "No Metal device for video textures"));
    auto buffer = AllocateTextureBuffer(options);
    CIImage* image = [CIImage imageWithCVPixelBuffer:source];
    const auto crop = VideoSourceRectangle(frame.format, geometry.source);
    const auto coded_crop = CGRectMake(crop.x, frame.format.height - crop.y - crop.height, crop.width, crop.height);
    image = [image imageByCroppingToRect:coded_crop];
    switch (frame.format.rotation) {
      case VideoRotation::None: break;
      case VideoRotation::Clockwise90: image = [image imageByApplyingOrientation:6]; break;
      case VideoRotation::Clockwise180: image = [image imageByApplyingOrientation:3]; break;
      case VideoRotation::Clockwise270: image = [image imageByApplyingOrientation:8]; break;
    }
    const auto extent = image.extent;
    const auto& area = geometry.destination;
    const auto sx = area.width / extent.size.width, sy = area.height / extent.size.height;
    image = [image imageByApplyingTransform:CGAffineTransformMake(
        sx, 0, 0, sy, area.x - sx * extent.origin.x,
        options.height - area.y - area.height - sy * extent.origin.y)];
    const auto output = CGRectMake(0, 0, options.width, options.height);
    auto black = [[CIImage imageWithColor:[CIColor colorWithRed:0 green:0 blue:0 alpha:1]] imageByCroppingToRect:output];
    image = [image imageByCompositingOverImage:black];
    const auto color = std::unique_ptr<std::remove_pointer_t<CGColorSpaceRef>, decltype(&CGColorSpaceRelease)>(
        CGColorSpaceCreateWithName(kCGColorSpaceSRGB), CGColorSpaceRelease);
    [context render:image toCVPixelBuffer:buffer.get() bounds:output colorSpace:color.get()];
    return PublishTextureBuffer(buffer.get(), options);
  }
}

detail::VideoTextureResult detail::PublishVideoPixels(Bytes pixels, VideoTextureOptions options) {
  @autoreleasepool {
    auto buffer = AllocateTextureBuffer(options);
    if (CVPixelBufferLockBaseAddress(buffer.get(), 0) != kCVReturnSuccess)
      throw std::runtime_error("Cannot lock the software video texture");
    auto* output = static_cast<std::byte*>(CVPixelBufferGetBaseAddress(buffer.get()));
    const auto stride = CVPixelBufferGetBytesPerRow(buffer.get());
    const auto row_bytes = std::size_t(options.width) * 4;
    for (unsigned row = 0; row < options.height; ++row) {
      const auto* input = pixels.data() + row * row_bytes;
      auto* destination = output + row * stride;
      std::memcpy(destination, input, row_bytes);
      for (std::size_t x = 0; x < row_bytes; x += 4) std::swap(destination[x], destination[x + 2]);
    }
    CVPixelBufferUnlockBaseAddress(buffer.get(), 0);
    return PublishTextureBuffer(buffer.get(), options);
  }
}

std::unique_ptr<detail::VideoFrameTransformBackend> detail::MakeVideoFrameTransformBackend() {
  return std::make_unique<MetalVideoFrameTransformer>();
}

} // namespace huxerui::media
