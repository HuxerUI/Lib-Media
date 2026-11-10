#pragma once

#include <huxerui/media_format.h>

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wrl/client.h>

namespace huxerui::media::detail {

inline HRESULT OpenSynchronousTransform(GUID category, const MFT_REGISTER_TYPE_INFO* input,
    const MFT_REGISTER_TYPE_INFO* output, Microsoft::WRL::ComPtr<IMFTransform>& transform) {
  IMFActivate** candidates = nullptr;
  UINT32 count = 0;
  auto result = MFTEnumEx(category, MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_TRANSCODE_ONLY |
      MFT_ENUM_FLAG_SORTANDFILTER, input, output, &candidates, &count);
  if (FAILED(result)) return result;
  result = MF_E_TOPO_CODEC_NOT_FOUND;
  for (UINT32 i = 0; i < count; ++i) {
    if (!transform) result = candidates[i]->ActivateObject(IID_PPV_ARGS(&transform));
    candidates[i]->Release();
  }
  CoTaskMemFree(candidates);
  return result;
}

inline void ReadMediaFoundationVideoColor(IMFAttributes* type, VideoFormat& format) {
  const auto primaries = MFGetAttributeUINT32(type, MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_Unknown);
  const auto transfer = MFGetAttributeUINT32(type, MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_Unknown);
  const auto matrix = MFGetAttributeUINT32(type, MF_MT_YUV_MATRIX, MFVideoTransferMatrix_Unknown);
  const auto range = MFGetAttributeUINT32(type, MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_Unknown);
  switch (primaries) {
    case MFVideoPrimaries_BT709: format.primaries = ColorPrimaries::BT709; break;
    case MFVideoPrimaries_BT470_2_SysBG: format.primaries = ColorPrimaries::BT601_625; break;
    case MFVideoPrimaries_SMPTE170M: format.primaries = ColorPrimaries::BT601_525; break;
    case MFVideoPrimaries_BT2020: format.primaries = ColorPrimaries::BT2020; break;
    default: format.primaries = ColorPrimaries::Unknown; break;
  }
  switch (transfer) {
    case MFVideoTransFunc_10: format.transfer = ColorTransfer::Linear; break;
    case MFVideoTransFunc_709: format.transfer = ColorTransfer::BT709; break;
    case MFVideoTransFunc_sRGB: format.transfer = ColorTransfer::SRGB; break;
    case MFVideoTransFunc_2084: format.transfer = ColorTransfer::PQ; break;
    case MFVideoTransFunc_HLG: format.transfer = ColorTransfer::HLG; break;
    default: format.transfer = ColorTransfer::Unknown; break;
  }
  switch (matrix) {
    case MFVideoTransferMatrix_BT601: format.matrix = ColorMatrix::BT601; break;
    case MFVideoTransferMatrix_BT709: format.matrix = ColorMatrix::BT709; break;
    case MFVideoTransferMatrix_BT2020_10:
    case MFVideoTransferMatrix_BT2020_12: format.matrix = ColorMatrix::BT2020; break;
    default: format.matrix = ColorMatrix::Unknown; break;
  }
  format.range = range == MFNominalRange_0_255 ? ColorRange::Full :
      range == MFNominalRange_16_235 ? ColorRange::Limited : ColorRange::Unknown;
}

} // namespace huxerui::media::detail
