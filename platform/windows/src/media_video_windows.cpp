#include "media_video_windows.h"

#include "codec/common.h"
#include "format/platform_video_storage.h"
#include "format/video_frame_transformer.h"

#include <huxerui/windows/external_texture.h>

#include <chrono>
#include <d3d10.h>
#include <d3d11_1.h>
#include <mfapi.h>
#include <thread>

namespace huxerui::media::detail {
namespace {
using Microsoft::WRL::ComPtr;

template<class Function> Function MediaFoundationFunction(const char* name) {
  const auto module = GetModuleHandleW(L"mfplat.dll");
  return module ? reinterpret_cast<Function>(GetProcAddress(module, name)) : nullptr;
}

CodecError VideoError(HRESULT status, const char* message) {
  return Error(status == E_NOTIMPL ? CodecErrorCode::Unsupported : CodecErrorCode::PlatformFailure,
      message, std::to_string(status));
}

struct TextureStorage final : PlatformVideoStorage {
  TextureStorage(const VideoFormat& format, std::shared_ptr<WindowsVideoDevice> device,
      ComPtr<ID3D11Texture2D> image, UINT slice, IMFSample* sample = nullptr)
      : PlatformVideoStorage(format.width, format.height, format.pixel_format), device(std::move(device)),
        image(std::move(image)), slice(slice), sample(sample) {}

  std::shared_ptr<WindowsVideoDevice> device;
  ComPtr<ID3D11Texture2D> image;
  UINT slice;
  ComPtr<IMFSample> sample;
};

class InputImageLease final : public IUnknown {
public:
  explicit InputImageLease(std::shared_ptr<const PlatformVideoStorage> image) : image_(std::move(image)) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) override {
    if (!result) return E_POINTER;
    *result = nullptr;
    if (iid != IID_IUnknown) return E_NOINTERFACE;
    *result = static_cast<IUnknown*>(this);
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&references_); }
  ULONG STDMETHODCALLTYPE Release() override {
    const auto count = InterlockedDecrement(&references_);
    if (!count) delete this;
    return count;
  }

private:
  LONG references_ = 1;
  std::shared_ptr<const PlatformVideoStorage> image_;
};

HRESULT WaitForVideo(ID3D11DeviceContext& context, ID3D11Query& query) {
  context.End(&query);
  context.Flush();
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  for (;;) {
    const auto status = context.GetData(&query, nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH);
    if (status != S_FALSE) return status;
    if (std::chrono::steady_clock::now() >= deadline) return HRESULT_FROM_WIN32(WAIT_TIMEOUT);
    std::this_thread::yield();
  }
}

class VideoProcessor {
public:
  HRESULT Run(const TextureStorage& source, const VideoFormat& format, UINT width, UINT height,
      DXGI_FORMAT layout, ComPtr<ID3D11Texture2D>& output) {
    auto& device = *source.device;
    std::lock_guard lock(device.commands);
    D3D11_TEXTURE2D_DESC source_desc{};
    source.image->GetDesc(&source_desc);
    if (device_.Get() != device.device.Get() || input_width_ != source_desc.Width || input_height_ != source_desc.Height ||
        width_ != width || height_ != height || layout_ != layout) {
      const auto status = Open(device, source_desc.Width, source_desc.Height, width, height, layout);
      if (FAILED(status)) return status;
    }

    D3D11_TEXTURE2D_DESC output_desc{};
    output_desc.Width = width;
    output_desc.Height = height;
    output_desc.MipLevels = output_desc.ArraySize = output_desc.SampleDesc.Count = 1;
    output_desc.Format = layout;
    output_desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    auto status = device.device->CreateTexture2D(&output_desc, nullptr, &output);
    if (FAILED(status)) return status;

    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC input_desc{};
    input_desc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    input_desc.Texture2D.ArraySlice = source.slice;
    ComPtr<ID3D11VideoProcessorInputView> input_view;
    status = device.video->CreateVideoProcessorInputView(source.image.Get(), enumerator_.Get(), &input_desc, &input_view);
    if (FAILED(status)) return status;
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC view_desc{};
    view_desc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    ComPtr<ID3D11VideoProcessorOutputView> output_view;
    status = device.video->CreateVideoProcessorOutputView(output.Get(), enumerator_.Get(), &view_desc, &output_view);
    if (FAILED(status)) return status;

    const auto& visible = format.visible;
    const RECT crop{LONG(visible.x), LONG(visible.y), LONG(visible.x + visible.width), LONG(visible.y + visible.height)};
    const RECT target{0, 0, LONG(width), LONG(height)};
    auto* processor = processor_.Get();
    auto& context = *device.processor.Get();
    context.VideoProcessorSetStreamFrameFormat(processor, 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    context.VideoProcessorSetStreamAutoProcessingMode(processor, 0, FALSE);
    context.VideoProcessorSetStreamSourceRect(processor, 0, TRUE, &crop);
    context.VideoProcessorSetStreamDestRect(processor, 0, TRUE, &target);
    context.VideoProcessorSetOutputTargetRect(processor, TRUE, &target);

    D3D11_VIDEO_PROCESSOR_COLOR_SPACE color{};
    color.YCbCr_Matrix = format.matrix == ColorMatrix::BT709;
    color.Nominal_Range = format.range == ColorRange::Full ? D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255 :
        D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
    context.VideoProcessorSetStreamColorSpace(processor, 0, &color);
    if (layout == DXGI_FORMAT_B8G8R8A8_UNORM) color.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255;
    context.VideoProcessorSetOutputColorSpace(processor, &color);

    const bool rotated = format.rotation != VideoRotation::None;
    if (rotated && (!rotation_ || !context1_)) return E_NOTIMPL;
    if (context1_ && rotation_) {
      auto rotation = D3D11_VIDEO_PROCESSOR_ROTATION_IDENTITY;
      switch (format.rotation) {
        case VideoRotation::None: break;
        case VideoRotation::Clockwise90: rotation = D3D11_VIDEO_PROCESSOR_ROTATION_90; break;
        case VideoRotation::Clockwise180: rotation = D3D11_VIDEO_PROCESSOR_ROTATION_180; break;
        case VideoRotation::Clockwise270: rotation = D3D11_VIDEO_PROCESSOR_ROTATION_270; break;
      }
      context1_->VideoProcessorSetStreamRotation(processor, 0, rotated, rotation);
    }

    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.pInputSurface = input_view.Get();
    status = context.VideoProcessorBlt(processor, output_view.Get(), 0, 1, &stream);
    if (FAILED(status)) return status;
    return WaitForVideo(*device.context.Get(), *completed_.Get());
  }

private:
  HRESULT Open(const WindowsVideoDevice& device, UINT input_width, UINT input_height,
      UINT width, UINT height, DXGI_FORMAT layout) {
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
    content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    content.InputFrameRate = content.OutputFrameRate = {30, 1};
    content.InputWidth = input_width;
    content.InputHeight = input_height;
    content.OutputWidth = width;
    content.OutputHeight = height;
    content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
    ComPtr<ID3D11VideoProcessorEnumerator> enumerator;
    auto status = device.video->CreateVideoProcessorEnumerator(&content, &enumerator);
    if (FAILED(status)) return status;
    UINT input = 0, output = 0;
    if (FAILED(enumerator->CheckVideoProcessorFormat(DXGI_FORMAT_NV12, &input)) ||
        FAILED(enumerator->CheckVideoProcessorFormat(layout, &output)) ||
        !(input & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT) || !(output & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT))
      return E_NOTIMPL;
    ComPtr<ID3D11VideoProcessor> processor;
    status = device.video->CreateVideoProcessor(enumerator.Get(), 0, &processor);
    if (FAILED(status)) return status;
    const D3D11_QUERY_DESC query{D3D11_QUERY_EVENT, 0};
    ComPtr<ID3D11Query> completed;
    status = device.device->CreateQuery(&query, &completed);
    if (FAILED(status)) return status;
    D3D11_VIDEO_PROCESSOR_CAPS caps{};
    rotation_ = SUCCEEDED(enumerator->GetVideoProcessorCaps(&caps)) && (caps.FeatureCaps & D3D11_VIDEO_PROCESSOR_FEATURE_CAPS_ROTATION);
    context1_.Reset();
    device.processor.As(&context1_);
    enumerator_ = std::move(enumerator);
    processor_ = std::move(processor);
    completed_ = std::move(completed);
    device_ = device.device;
    input_width_ = input_width;
    input_height_ = input_height;
    width_ = width;
    height_ = height;
    layout_ = layout;
    return S_OK;
  }

  ComPtr<ID3D11Device> device_;
  ComPtr<ID3D11VideoProcessorEnumerator> enumerator_;
  ComPtr<ID3D11VideoProcessor> processor_;
  ComPtr<ID3D11VideoContext1> context1_;
  ComPtr<ID3D11Query> completed_;
  UINT input_width_ = 0, input_height_ = 0, width_ = 0, height_ = 0;
  DXGI_FORMAT layout_ = DXGI_FORMAT_UNKNOWN;
  bool rotation_ = false;
};

class D3D11VideoFrameTransformer final : public VideoFrameTransformBackend {
public:
  CodecResult<VideoFrame> Transform(const VideoFrame& frame, VideoFormat target) override {
    using R = CodecResult<VideoFrame>;
    const auto* input = dynamic_cast<const TextureStorage*>(VideoFrameAccess::PlatformStorage(frame).get());
    if (!input) return R::Failure(Error(CodecErrorCode::InvalidArgument, "Expected a D3D11 video image"));
    ComPtr<ID3D11Texture2D> output;
    const auto status = processor_.Run(*input, frame.format, target.width, target.height, DXGI_FORMAT_NV12, output);
    if (FAILED(status)) return R::Failure(VideoError(status, "Cannot transform the D3D11 video image"));
    return R::Success(VideoFrameAccess::FromPlatform(target, frame.pts, frame.duration,
        std::make_shared<TextureStorage>(target, input->device, std::move(output), 0)));
  }

private:
  VideoProcessor processor_;
};

} // namespace

HRESULT OpenVideoDevice(std::shared_ptr<WindowsVideoDevice>& device) {
  static std::mutex mutex;
  static std::weak_ptr<WindowsVideoDevice> current;
  std::lock_guard lock(mutex);
  device = current.lock();
  if (device && SUCCEEDED(device->device->GetDeviceRemovedReason())) return S_OK;
  auto created = std::make_shared<WindowsVideoDevice>();
  const D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
  auto status = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
      D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2, D3D11_SDK_VERSION,
      &created->device, nullptr, &created->context);
  if (FAILED(status)) return status;
  ComPtr<ID3D10Multithread> threads;
  if (FAILED(status = created->device.As(&threads))) return status;
  threads->SetMultithreadProtected(TRUE);
  if (FAILED(status = created->device.As(&created->video)) || FAILED(status = created->context.As(&created->processor)))
    return status;
  current = created;
  device = std::move(created);
  return S_OK;
}

HRESULT BindVideoDevice(IMFTransform& transform, const WindowsVideoDevice& device, ComPtr<IMFDXGIDeviceManager>& manager) {
  const auto create = MediaFoundationFunction<HRESULT (WINAPI*)(UINT*, IMFDXGIDeviceManager**)>("MFCreateDXGIDeviceManager");
  if (!create) return E_NOTIMPL;
  ComPtr<IMFAttributes> attributes;
  auto status = transform.GetAttributes(&attributes);
  if (FAILED(status)) return status;
  if (!MFGetAttributeUINT32(attributes.Get(), MF_SA_D3D11_AWARE, FALSE)) return E_NOTIMPL;
  UINT token = 0;
  status = create(&token, &manager);
  if (FAILED(status)) return status;
  status = manager->ResetDevice(device.device.Get(), token);
  if (FAILED(status)) return status;
  return transform.ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, reinterpret_cast<ULONG_PTR>(manager.Get()));
}

CodecResult<VideoFrame> RetainVideoSample(IMFSample& sample, const std::shared_ptr<WindowsVideoDevice>& device,
    VideoFormat format, MediaTimestamp pts, std::optional<MediaTimestamp> duration) {
  using R = CodecResult<VideoFrame>;
  ComPtr<IMFMediaBuffer> buffer;
  ComPtr<IMFDXGIBuffer> dxgi;
  ComPtr<ID3D11Texture2D> image;
  UINT slice = 0;
  if (FAILED(sample.GetBufferByIndex(0, &buffer)) || FAILED(buffer.As(&dxgi)) ||
      FAILED(dxgi->GetResource(IID_PPV_ARGS(&image))) || FAILED(dxgi->GetSubresourceIndex(&slice)))
    return R::Failure(Error(CodecErrorCode::Unsupported, "Media Foundation did not return a DXGI video image"));
  D3D11_TEXTURE2D_DESC desc{};
  image->GetDesc(&desc);
  ComPtr<ID3D11Device> owner;
  image->GetDevice(&owner);
  if (desc.Format != DXGI_FORMAT_NV12 || desc.MipLevels != 1 || desc.SampleDesc.Count != 1 || slice >= desc.ArraySize ||
      desc.Width < format.width || desc.Height < format.height || owner.Get() != device->device.Get())
    return R::Failure(Error(CodecErrorCode::Unsupported, "Media Foundation returned an incompatible D3D11 video image"));
  return R::Success(VideoFrameAccess::FromPlatform(format, pts, duration,
      std::make_shared<TextureStorage>(format, device, std::move(image), slice, &sample)));
}

HRESULT MakeVideoInputBuffer(const VideoFrame& frame, const WindowsVideoDevice& device, ComPtr<IMFMediaBuffer>& buffer) {
  const auto& storage = VideoFrameAccess::PlatformStorage(frame);
  const auto* image = dynamic_cast<const TextureStorage*>(storage.get());
  if (!image || image->device.get() != &device) return E_INVALIDARG;
  const auto create = MediaFoundationFunction<HRESULT (WINAPI*)(REFIID, IUnknown*, UINT, BOOL, IMFMediaBuffer**)>(
      "MFCreateDXGISurfaceBuffer");
  if (!create) return E_NOTIMPL;
  auto status = create(__uuidof(ID3D11Texture2D), image->image.Get(), image->slice, FALSE, &buffer);
  if (FAILED(status)) return status;
  ComPtr<IMFDXGIBuffer> dxgi;
  status = buffer.As(&dxgi);
  if (FAILED(status)) return status;
  // The wrapper can outlive the input sample. Keep the frame and any decoder pool reservation with it.
  constexpr GUID image_lease{0xc7b11760, 0xdc13, 0x4d94, {0x9c, 0x6b, 0xa2, 0x83, 0x60, 0x25, 0x63, 0x1c}};
  ComPtr<IUnknown> lease;
  lease.Attach(new InputImageLease(storage));
  status = dxgi->SetUnknown(image_lease, lease.Get());
  if (FAILED(status)) return status;
  DWORD length = 0;
  status = buffer->GetMaxLength(&length);
  return FAILED(status) ? status : buffer->SetCurrentLength(length);
}

CodecResult<void> PublishWindowsVideo(ExternalTexture& texture, const VideoFrame& frame) {
  const auto* input = dynamic_cast<const TextureStorage*>(VideoFrameAccess::PlatformStorage(frame).get());
  auto* target = dynamic_cast<windows::D3D11Texture*>(&texture);
  if (!input || !target)
    return CodecResult<void>::Failure(Error(CodecErrorCode::InvalidArgument, "Expected a D3D11 video image and texture mailbox"));
  VideoProcessor processor;
  ComPtr<ID3D11Texture2D> output;
  const auto status = processor.Run(*input, frame.format, frame.format.width, frame.format.height, DXGI_FORMAT_B8G8R8A8_UNORM, output);
  if (FAILED(status)) return CodecResult<void>::Failure(VideoError(status, "Cannot prepare the D3D11 video texture"));
  std::lock_guard lock(input->device->commands);
  target->Publish({output.Get(), windows::D3D11Texture::Alpha::Opaque});
  return CodecResult<void>::Success();
}

CodecResult<std::shared_ptr<ExternalTexture>> MakePlatformVideoTexture(const VideoFrame& frame) {
  using R = CodecResult<std::shared_ptr<ExternalTexture>>;
  auto texture = std::make_shared<windows::D3D11Texture>(Size{float(frame.format.width), float(frame.format.height)});
  if (auto result = PublishWindowsVideo(*texture, frame); !result.Succeeded()) return R::Failure(std::move(result.Error()));
  texture->Finish();
  return R::Success(std::move(texture));
}

std::unique_ptr<VideoFrameTransformBackend> MakeVideoFrameTransformBackend() { return std::make_unique<D3D11VideoFrameTransformer>(); }

} // namespace huxerui::media::detail
