#include "decoded_preview.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#if defined(__ANDROID__)
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <huxerui/android/external_texture.h>
#elif defined(MEDIA_PREVIEW_MACOS)
#include <CoreVideo/CoreVideo.h>
#include <huxerui/macos/external_texture.h>
#elif defined(__EMSCRIPTEN__)
#include <emscripten.h>
#include <huxerui/web/external_texture.h>
#elif defined(_WIN32)
#include <huxerui/windows/external_texture.h>
#endif

using namespace huxerui;
using namespace huxerui::media;

namespace media_preview {
#if defined(__ANDROID__) || defined(MEDIA_PREVIEW_MACOS) || defined(MEDIA_PREVIEW_IOS) || defined(__EMSCRIPTEN__) || defined(_WIN32)
namespace {

unsigned Sample(const VideoPlane& plane, unsigned x, unsigned y, unsigned component = 0) {
  return std::to_integer<unsigned>(plane.data.AsBytes()[y * plane.row_stride + x * plane.pixel_stride + component]);
}

} // namespace

Bytes ConvertFramePixels(const VideoFrame& frame) {
  if (!frame.Validate()) throw std::runtime_error("Invalid VideoFrame");
  const auto matrix = frame.format.matrix == ColorMatrix::Unknown ? ColorMatrix::BT601 : frame.format.matrix;
  if (matrix != ColorMatrix::BT601 && matrix != ColorMatrix::BT709)
    throw std::runtime_error("The preview supports BT.601 and BT.709");
  const auto visible = frame.format.visible;
  const bool planar = frame.format.pixel_format == VideoPixelFormat::I420;
  const bool limited = frame.format.range != ColorRange::Full;
  const double kr = matrix == ColorMatrix::BT601 ? 0.299 : 0.2126;
  const double kb = matrix == ColorMatrix::BT601 ? 0.114 : 0.0722;
  const auto byte = [](double value) { return std::byte(std::lround(std::clamp(value, 0., 1.) * 255)); };
  Bytes pixels(std::size_t(visible.width) * visible.height * 4);
  for (unsigned y = 0; y < visible.height; ++y) {
    for (unsigned x = 0; x < visible.width; ++x) {
      const auto px = x + visible.x, py = y + visible.y;
      const auto luma = (double(Sample(frame.planes[0], px, py)) - (limited ? 16 : 0)) / (limited ? 219 : 255);
      const auto u = (double(Sample(frame.planes[1], px / 2, py / 2)) - 128) / (limited ? 224 : 255);
      const auto v = (double(Sample(frame.planes[planar ? 2 : 1], px / 2, py / 2, planar ? 0 : 1)) - 128) /
                     (limited ? 224 : 255);
      const auto red = luma + 2 * (1 - kr) * v;
      const auto blue = luma + 2 * (1 - kb) * u;
      const auto green = (luma - kr * red - kb * blue) / (1 - kr - kb);
      auto* out = pixels.data() + (y * visible.width + x) * 4;
      out[0] = byte(red);
      out[1] = byte(green);
      out[2] = byte(blue);
      out[3] = std::byte{255};
    }
  }
  return pixels;
}
#endif

#if defined(__ANDROID__) || defined(MEDIA_PREVIEW_MACOS)
namespace {
#if defined(__ANDROID__)
class UploadContext {
public:
  void Open() {
    display_ = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display_ == EGL_NO_DISPLAY || !eglInitialize(display_, nullptr, nullptr))
      throw std::runtime_error("Cannot initialize the EGL display");
    constexpr EGLint attributes[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
                                      EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
    EGLConfig config{};
    EGLint count = 0;
    if (!eglChooseConfig(display_, attributes, &config, 1, &count) || count == 0)
      throw std::runtime_error("Cannot choose an EGL upload configuration");
    constexpr EGLint surface_attributes[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
    constexpr EGLint context_attributes[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    surface_ = eglCreatePbufferSurface(display_, config, surface_attributes);
    if (!eglBindAPI(EGL_OPENGL_ES_API)) throw std::runtime_error("Cannot bind OpenGL ES");
    context_ = eglCreateContext(display_, config, EGL_NO_CONTEXT, context_attributes);
    if (surface_ == EGL_NO_SURFACE || context_ == EGL_NO_CONTEXT || !eglMakeCurrent(display_, surface_, surface_, context_))
      throw std::runtime_error("Cannot activate the EGL upload context");
    current_ = true;
  }
  ~UploadContext() {
    if (current_) {
      glDeleteTextures(1, &name);
      if (previous_context_ != EGL_NO_CONTEXT)
        eglMakeCurrent(previous_display_, previous_draw_, previous_read_, previous_context_);
      else
        eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }
    eglBindAPI(previous_api_);
    if (context_ != EGL_NO_CONTEXT) eglDestroyContext(display_, context_);
    if (surface_ != EGL_NO_SURFACE) eglDestroySurface(display_, surface_);
    // The renderer can still use the default display.
  }

  GLuint name = 0;
private:
  EGLDisplay previous_display_ = eglGetCurrentDisplay();
  EGLContext previous_context_ = eglGetCurrentContext();
  EGLSurface previous_draw_ = eglGetCurrentSurface(EGL_DRAW), previous_read_ = eglGetCurrentSurface(EGL_READ);
  EGLenum previous_api_ = eglQueryAPI();
  EGLDisplay display_ = EGL_NO_DISPLAY;
  EGLContext context_ = EGL_NO_CONTEXT;
  EGLSurface surface_ = EGL_NO_SURFACE;
  bool current_ = false;
};

void Upload(ExternalTexture& texture, const Bytes& pixels, unsigned width, unsigned height) {
  UploadContext context;
  context.Open();
  glGenTextures(1, &context.name);
  glBindTexture(GL_TEXTURE_2D, context.name);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
  if (glGetError() != GL_NO_ERROR) throw std::runtime_error("Cannot upload the RGBA frame");
  static_cast<android::GlTexture&>(texture).PublishCurrent({context.name, int(width), int(height), -1,
      android::GlTexture::Origin::TopLeft, android::GlTexture::Alpha::Opaque});
}
#else
void Upload(ExternalTexture& texture, const Bytes& pixels, unsigned width, unsigned height) {
  auto surface = CFDictionaryCreate(kCFAllocatorDefault, nullptr, nullptr, 0,
                                    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
  const void* keys[] = {kCVPixelBufferIOSurfacePropertiesKey, kCVPixelBufferMetalCompatibilityKey};
  const void* values[] = {surface, kCFBooleanTrue};
  auto attributes = CFDictionaryCreate(kCFAllocatorDefault, keys, values, 2,
                                       &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
  CVPixelBufferRef buffer = nullptr;
  const auto created = CVPixelBufferCreate(kCFAllocatorDefault, width, height,
                                           kCVPixelFormatType_32BGRA, attributes, &buffer);
  CFRelease(attributes);
  CFRelease(surface);
  if (created != kCVReturnSuccess) throw std::runtime_error("Cannot allocate the BGRA pixel buffer");
  const std::unique_ptr<std::remove_pointer_t<CVPixelBufferRef>, decltype(&CVPixelBufferRelease)>
      owned(buffer, CVPixelBufferRelease);
  if (CVPixelBufferLockBaseAddress(buffer, 0) != kCVReturnSuccess)
    throw std::runtime_error("Cannot lock the BGRA pixel buffer");
  auto* base = static_cast<std::byte*>(CVPixelBufferGetBaseAddress(buffer));
  const auto stride = CVPixelBufferGetBytesPerRow(buffer);
  for (unsigned y = 0; y < height; ++y) {
    auto* row = base + y * stride;
    for (unsigned x = 0; x < width; ++x) {
      const auto* rgb = pixels.data() + (y * width + x) * 4;
      row[x * 4] = rgb[2];
      row[x * 4 + 1] = rgb[1];
      row[x * 4 + 2] = rgb[0];
      row[x * 4 + 3] = rgb[3];
    }
  }
  CVPixelBufferUnlockBaseAddress(buffer, 0);
  static_cast<macos::PixelBufferTexture&>(texture).Publish(buffer);
}
#endif
} // namespace

std::shared_ptr<ExternalTexture> MakeFrameTexture(huxerui::Size size) {
#if defined(__ANDROID__)
  return std::make_shared<android::GlTexture>(size);
#else
  return std::make_shared<macos::PixelBufferTexture>(size);
#endif
}

void PublishFrame(ExternalTexture& texture, const VideoFrame& frame) {
  Upload(texture, ConvertFramePixels(frame), frame.format.visible.width, frame.format.visible.height);
}
#elif defined(__EMSCRIPTEN__)
namespace {

EM_JS(emscripten::EM_VAL, CreatePreviewVideoFrame,
      (emscripten::EM_VAL bytes, unsigned width, unsigned height, double timestamp), {
  try {
    return Emval.toHandle(new VideoFrame(Emval.toValue(bytes), {
      format: 'RGBA', codedWidth: width, codedHeight: height, timestamp
    }));
  } catch (_) {
    return Emval.toHandle(null);
  }
});

} // namespace

std::shared_ptr<ExternalTexture> MakeFrameTexture(huxerui::Size size) {
  if (emscripten::val::global("VideoFrame").isUndefined())
    throw std::runtime_error("Web VideoFrame requires a supported browser and a secure context");
  return std::make_shared<web::VideoFrameTexture>(size);
}

void PublishFrame(ExternalTexture& texture, const VideoFrame& frame) {
  const auto pixels = ConvertFramePixels(frame);
  const auto timestamp = frame.pts.Rescale(1'000'000);
  constexpr std::int64_t max_safe_integer = 9'007'199'254'740'991;
  if (!timestamp || timestamp->ticks < -max_safe_integer || timestamp->ticks > max_safe_integer)
    throw std::runtime_error("VideoFrame timestamp exceeds the browser range");
  const auto bytes = emscripten::val(emscripten::typed_memory_view(
      pixels.size(), reinterpret_cast<const unsigned char*>(pixels.data())));
  const auto visible = frame.format.visible;
  auto native = emscripten::val::take_ownership(
      CreatePreviewVideoFrame(bytes.as_handle(), visible.width, visible.height, double(timestamp->ticks)));
  if (native.isNull()) throw std::runtime_error("Cannot create the Web preview VideoFrame");
  try {
    static_cast<web::VideoFrameTexture&>(texture).Publish(native);
  } catch (...) {
    native.call<void>("close");
    throw;
  }
  native.call<void>("close");
}
#elif defined(_WIN32)
std::shared_ptr<ExternalTexture> MakeFrameTexture(huxerui::Size size) {
  return std::make_shared<windows::PixelTexture>(size);
}

void PublishFrame(ExternalTexture& texture, const VideoFrame& frame) {
  const auto pixels = ConvertFramePixels(frame);
  const auto visible = frame.format.visible;
  static_cast<windows::PixelTexture&>(texture).Publish({
      .pixel_width = int(visible.width), .pixel_height = int(visible.height),
      .bytes_per_row = std::size_t(visible.width) * 4,
      .format = windows::PixelFormat::Rgba8888, .pixels = pixels});
}
#elif !defined(MEDIA_PREVIEW_IOS)
std::shared_ptr<ExternalTexture> MakeFrameTexture(huxerui::Size) { return {}; }
#endif
} // namespace media_preview
