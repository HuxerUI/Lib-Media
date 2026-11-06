#include "decoded_preview.h"

#include <array>
#include <cmath>
#include <iostream>
#include <stdexcept>

using namespace huxerui;
using namespace huxerui::media;

namespace {

void Check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

constexpr VideoRectangle crop{8, 8, 120, 72};
using Yuv = std::array<unsigned, 3>;

Yuv Pattern(bool planar, unsigned x, unsigned y) {
  constexpr std::array<Yuv, 6> limited{{{235,128,128}, {126,128,128}, {16,128,128},
                                       {81,90,240}, {145,54,34}, {41,240,110}}};
  constexpr std::array<Yuv, 6> full{{{255,128,128}, {128,128,128}, {0,128,128},
                                    {54,99,255}, {182,30,12}, {18,255,116}}};
  const auto& colors = planar ? full : limited;
  if (x < crop.x || y < crop.y || x >= crop.x + crop.width || y >= crop.y + crop.height)
    return planar ? Yuv{73,227,244} : Yuv{106,202,222};
  x -= crop.x;
  y -= crop.y;
  if (y >= 56) {
    const auto marker = planar ? 100u : 4u;
    return x >= marker && x < marker + 16 && y >= 60 && y < 68 ? colors[0] : colors[2];
  }
  return colors[x / 20];
}

VideoFrame MakeFrame(VideoPixelFormat layout) {
  const bool planar = layout == VideoPixelFormat::I420;
  VideoFrame frame;
  frame.format.width = 137;
  frame.format.height = 89;
  frame.format.visible = crop;
  frame.format.pixel_format = layout;
  frame.format.matrix = planar ? ColorMatrix::BT709 : ColorMatrix::BT601;
  frame.format.range = planar ? ColorRange::Full : ColorRange::Limited;
  frame.pts = {planar ? 1 : 0, 30};
  frame.duration = MediaTimestamp{1, 30};

  for (unsigned plane = 0; plane < (planar ? 3u : 2u); ++plane) {
    const auto width = plane ? (frame.format.width + 1) / 2 : frame.format.width;
    const auto height = plane ? (frame.format.height + 1) / 2 : frame.format.height;
    const unsigned pixel_stride = planar ? plane + 2 : plane + 1;
    const auto row_stride = width * pixel_stride + 11 + 4 * plane;
    auto bytes = std::make_shared<Bytes>(row_stride * height, std::byte{0xe3});
    for (unsigned y = 0; y < height; ++y) {
      for (unsigned x = 0; x < width; ++x) {
        const auto sample = Pattern(planar, plane ? x * 2 : x, plane ? y * 2 : y);
        const auto offset = y * row_stride + x * pixel_stride;
        (*bytes)[offset] = std::byte(sample[plane]);
        if (!planar && plane == 1) (*bytes)[offset + 1] = std::byte(sample[2]);
      }
    }
    frame.planes.push_back({BufferReference(*bytes, bytes), row_stride, pixel_stride});
  }
  return frame;
}

void CheckPixels(const VideoFrame& frame) {
  constexpr std::array<Yuv, 6> rgb{{{255, 255, 255}, {128, 128, 128}, {0, 0, 0},
                                    {255, 0, 0}, {0, 255, 0}, {0, 0, 255}}};
  const auto pixels = media_preview::ConvertFramePixels(frame);
  Check(pixels.size() == crop.width * crop.height * 4, "Conversion did not use the visible crop");
  const auto marker = frame.format.pixel_format == VideoPixelFormat::I420 ? 100u : 4u;
  for (unsigned y = 0; y < crop.height; ++y) {
    for (unsigned x = 0; x < crop.width; ++x) {
      const auto expected = y < 56 ? rgb[x / 20]
          : (x >= marker && x < marker + 16 && y >= 60 && y < 68 ? rgb[0] : rgb[2]);
      const auto offset = (y * crop.width + x) * 4;
      for (unsigned channel = 0; channel < 3; ++channel) {
        const auto actual = std::to_integer<int>(pixels[offset + channel]);
        Check(std::abs(actual - int(expected[channel])) <= 3, "Unexpected color, crop or stride conversion");
      }
      Check(pixels[offset + 3] == std::byte{255}, "Unexpected alpha after conversion");
    }
  }
}

} // namespace

int main() {
  try {
    auto texture = media_preview::MakeFrameTexture({300, 180});
    Check(bool(texture), "Frame texture is unavailable");
    auto revision = texture->Revision();
    for (auto layout : {VideoPixelFormat::NV12, VideoPixelFormat::I420, VideoPixelFormat::NV12}) {
      const auto frame = MakeFrame(layout);
      CheckPixels(frame);
      media_preview::PublishFrame(*texture, frame);
      Check(texture->Revision() == ++revision, "Publishing to the same texture did not advance its revision");
    }
    auto unknown = MakeFrame(VideoPixelFormat::NV12);
    unknown.format.matrix = ColorMatrix::Unknown;
    unknown.format.range = ColorRange::Unknown;
    CheckPixels(unknown);
    std::cout << "NV12/I420 color, crop, stride and texture revisions passed\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
