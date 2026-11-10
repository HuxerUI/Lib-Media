#pragma once

#include <huxerui/media_codec.h>

#include <d3d11.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mutex>
#include <wrl/client.h>

namespace huxerui {
class ExternalTexture;
}

namespace huxerui::media::detail {

struct WindowsVideoDevice {
  Microsoft::WRL::ComPtr<ID3D11Device> device;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
  Microsoft::WRL::ComPtr<ID3D11VideoDevice> video;
  Microsoft::WRL::ComPtr<ID3D11VideoContext> processor;
  std::mutex commands;
};

HRESULT OpenVideoDevice(std::shared_ptr<WindowsVideoDevice>& device);
HRESULT BindVideoDevice(IMFTransform& transform, const WindowsVideoDevice& device,
    Microsoft::WRL::ComPtr<IMFDXGIDeviceManager>& manager);
CodecResult<VideoFrame> CopyVideoSample(IMFSample& sample, const std::shared_ptr<WindowsVideoDevice>& device,
    VideoFormat format, MediaTimestamp pts, std::optional<MediaTimestamp> duration);
HRESULT MakeVideoInputBuffer(const VideoFrame& frame, const WindowsVideoDevice& device,
    Microsoft::WRL::ComPtr<IMFMediaBuffer>& buffer);
CodecResult<void> PublishWindowsVideo(ExternalTexture& texture, const VideoFrame& frame);

} // namespace huxerui::media::detail
