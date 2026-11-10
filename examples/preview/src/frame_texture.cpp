#include "decoded_preview.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#if defined(__ANDROID__)
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <huxerui/android/external_texture.h>
#include "media_video_android.h"
#elif defined(__APPLE__) && TARGET_OS_OSX
#include <CoreVideo/CoreVideo.h>
#include <huxerui/macos/external_texture.h>
#include "media_video_macos.h"
#elif defined(__EMSCRIPTEN__)
#include <emscripten.h>
#include <huxerui/web/external_texture.h>
#elif defined(_WIN32)
#include <huxerui/windows/external_texture.h>
#include "media_video_windows.h"
#elif defined(__linux__)
#include <huxerui/linux/external_texture.h>
#endif

using namespace huxerui;
using namespace huxerui::media;

namespace media_preview {
#if defined(__ANDROID__) || defined(__APPLE__) || defined(__EMSCRIPTEN__) || defined(_WIN32) || defined(__linux__)
namespace {

unsigned Sample(const VideoPlane& plane, unsigned x, unsigned y, bool ten_bit, unsigned component = 0) {
  const auto bytes = plane.data.AsBytes();
  const auto at = y * plane.row_stride + x * plane.pixel_stride + component * (ten_bit ? 2 : 1);
  const auto low = std::to_integer<unsigned>(bytes[at]);
  if (!ten_bit) return low;
  return (low | (std::to_integer<unsigned>(bytes[at + 1]) << 8)) >> 6;
}

} // namespace

Bytes ConvertFramePixels(const VideoFrame& frame) {
  if (frame.Storage() != VideoStorage::Cpu || !frame.Validate()) throw std::runtime_error("Expected a CPU video frame");
  if (frame.format.transfer == ColorTransfer::PQ || frame.format.transfer == ColorTransfer::HLG)
    throw std::runtime_error("HDR frames require an HDR display path");
  const auto matrix = frame.format.matrix == ColorMatrix::Unknown ? ColorMatrix::BT601 : frame.format.matrix;
  if (matrix != ColorMatrix::BT601 && matrix != ColorMatrix::BT709)
    throw std::runtime_error("The preview supports BT.601 and BT.709");
  const auto visible = frame.format.visible;
  const auto [width, height] = DisplayDimensions(visible, frame.format.rotation);
  const bool planar = frame.format.pixel_format == VideoPixelFormat::I420;
  const bool ten_bit = frame.format.pixel_format == VideoPixelFormat::P010;
  const double scale = ten_bit ? 4 : 1;
  const double maximum = ten_bit ? 1023 : 255;
  const bool limited = frame.format.range != ColorRange::Full;
  const double kr = matrix == ColorMatrix::BT601 ? 0.299 : 0.2126;
  const double kb = matrix == ColorMatrix::BT601 ? 0.114 : 0.0722;
  const auto byte = [](double value) { return std::byte(std::lround(std::clamp(value, 0., 1.) * 255)); };
  Bytes pixels(std::size_t(width) * height * 4);
  for (unsigned y = 0; y < visible.height; ++y) {
    for (unsigned x = 0; x < visible.width; ++x) {
      const auto px = x + visible.x, py = y + visible.y;
      const auto luma = (double(Sample(frame.planes[0], px, py, ten_bit)) - (limited ? 16 * scale : 0)) /
                        (limited ? 219 * scale : maximum);
      const auto u = (double(Sample(frame.planes[1], px / 2, py / 2, ten_bit)) - 128 * scale) /
                     (limited ? 224 * scale : maximum);
      const auto v = (double(Sample(frame.planes[planar ? 2 : 1], px / 2, py / 2, ten_bit, planar ? 0 : 1)) -
                      128 * scale) / (limited ? 224 * scale : maximum);
      const auto red = luma + 2 * (1 - kr) * v;
      const auto blue = luma + 2 * (1 - kb) * u;
      const auto green = (luma - kr * red - kb * blue) / (1 - kr - kb);
      unsigned dx = x, dy = y;
      switch (frame.format.rotation) {
        case VideoRotation::None: break;
        case VideoRotation::Clockwise90: dx = width - 1 - y; dy = x; break;
        case VideoRotation::Clockwise180: dx = width - 1 - x; dy = height - 1 - y; break;
        case VideoRotation::Clockwise270: dx = y; dy = height - 1 - x; break;
      }
      auto* out = pixels.data() + (std::size_t(dy) * width + dx) * 4;
      out[0] = byte(red);
      out[1] = byte(green);
      out[2] = byte(blue);
      out[3] = std::byte{255};
    }
  }
  return pixels;
}
#endif

#if defined(__ANDROID__) || (defined(__APPLE__) && TARGET_OS_OSX)
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

void PublishGl(ExternalTexture& texture, GLuint name, unsigned width, unsigned height) {
  using Texture = huxerui::android::GlTexture;
  if (glGetError() != GL_NO_ERROR) throw std::runtime_error("Cannot prepare the video texture");
  static_cast<Texture&>(texture).PublishCurrent({name, int(width), int(height), -1,
      Texture::Origin::TopLeft, Texture::Alpha::Opaque});
}

void Upload(ExternalTexture& texture, const Bytes& pixels, unsigned width, unsigned height) {
  UploadContext context;
  context.Open();
  glGenTextures(1, &context.name);
  glBindTexture(GL_TEXTURE_2D, context.name);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
  PublishGl(texture, context.name, width, height);
}

void PublishPlatformFrame(ExternalTexture& texture, const VideoFrame& frame) {
  if (dynamic_cast<huxerui::android::SurfaceStreamTexture*>(&texture)) {
    Require(media::detail::PresentSurfaceFrame(frame));
    return;
  }
  UploadContext context;
  context.Open();
  const auto client = reinterpret_cast<PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC>(eglGetProcAddress("eglGetNativeClientBufferANDROID"));
  const auto create = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
  const auto destroy = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
  const auto bind = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(eglGetProcAddress("glEGLImageTargetTexture2DOES"));
  const auto buffer = media::detail::HardwareBuffer(frame);
  if (!buffer || !client || !create || !destroy || !bind) throw std::runtime_error("Cannot import the scaled Android image");
  const auto display = eglGetCurrentDisplay();
  const EGLint attributes[]{EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
  const auto image = create(display, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID, client(buffer), attributes);
  if (image == EGL_NO_IMAGE_KHR) throw std::runtime_error("Cannot create the Android preview image");
  const auto release = [display, destroy](EGLImageKHR value) { destroy(display, value); };
  const std::unique_ptr<std::remove_pointer_t<EGLImageKHR>, decltype(release)> lease(image, release);
  GLuint source = 0, framebuffer = 0;
  glGenTextures(1, &source);
  glBindTexture(GL_TEXTURE_2D, source);
  bind(GL_TEXTURE_2D, image);
  glGenFramebuffers(1, &framebuffer);
  glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, source, 0);
  const bool complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
  if (complete) {
    // The SDK exports a new EGL image, which requires a texture with its own storage.
    glGenTextures(1, &context.name);
    glBindTexture(GL_TEXTURE_2D, context.name);
    glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 0, 0, frame.format.width, frame.format.height, 0);
  }
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glDeleteFramebuffers(1, &framebuffer);
  glDeleteTextures(1, &source);
  if (!complete) throw std::runtime_error("Cannot attach the scaled Android preview image");
  PublishGl(texture, context.name, frame.format.width, frame.format.height);
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
  static_cast<huxerui::macos::PixelBufferTexture&>(texture).Publish(buffer);
}

void PublishPlatformFrame(ExternalTexture& texture, const VideoFrame& frame) {
  const auto buffer = huxerui::media::macos::PixelBuffer(frame);
  if (!buffer) throw std::runtime_error("Cannot import the scaled macOS image");
  static_cast<huxerui::macos::PixelBufferTexture&>(texture).Publish(buffer);
}
#endif
} // namespace

std::shared_ptr<ExternalTexture> MakeFrameTexture(huxerui::Size size, VideoStorage) {
#if defined(__ANDROID__)
  return std::make_shared<huxerui::android::GlTexture>(size);
#else
  return std::make_shared<huxerui::macos::PixelBufferTexture>(size);
#endif
}

void PublishFrame(ExternalTexture& texture, const VideoFrame& frame) {
  if (frame.Storage() == VideoStorage::Platform) {
    PublishPlatformFrame(texture, frame);
    return;
  }
  const auto [width, height] = DisplayDimensions(frame.format.visible, frame.format.rotation);
  Upload(texture, ConvertFramePixels(frame), width, height);
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

std::shared_ptr<ExternalTexture> MakeFrameTexture(huxerui::Size size, VideoStorage) {
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
  const auto [width, height] = DisplayDimensions(frame.format.visible, frame.format.rotation);
  auto video_frame = emscripten::val::take_ownership(
      CreatePreviewVideoFrame(bytes.as_handle(), width, height, double(timestamp->ticks)));
  if (video_frame.isNull()) throw std::runtime_error("Cannot create the Web preview VideoFrame");
  try {
    static_cast<web::VideoFrameTexture&>(texture).Publish(video_frame);
  } catch (...) {
    video_frame.call<void>("close");
    throw;
  }
  video_frame.call<void>("close");
}
#elif defined(_WIN32) || defined(__linux__)
#if defined(_WIN32)
namespace pixel_platform = huxerui::windows;
#else
namespace pixel_platform = huxerui::linux;
#endif

std::shared_ptr<ExternalTexture> MakeFrameTexture(huxerui::Size size, [[maybe_unused]] VideoStorage storage) {
#if defined(_WIN32) && !defined(HUXERUI_WINDOWS_7_COMPAT)
  if (storage == VideoStorage::Platform) return std::make_shared<huxerui::windows::D3D11Texture>(size);
#endif
  return std::make_shared<pixel_platform::PixelTexture>(size);
}

void PublishFrame(ExternalTexture& texture, const VideoFrame& frame) {
#if defined(_WIN32)
  if (frame.Storage() == VideoStorage::Platform) {
    Require(media::detail::PublishWindowsVideo(texture, frame));
    return;
  }
#endif
  const auto pixels = ConvertFramePixels(frame);
  const auto [width, height] = DisplayDimensions(frame.format.visible, frame.format.rotation);
  static_cast<pixel_platform::PixelTexture&>(texture).Publish({
      .pixel_width = int(width), .pixel_height = int(height),
      .bytes_per_row = std::size_t(width) * 4,
      .format = pixel_platform::PixelFormat::Rgba8888, .pixels = pixels});
}
#elif !defined(__APPLE__) || !TARGET_OS_IOS
std::shared_ptr<ExternalTexture> MakeFrameTexture(huxerui::Size, VideoStorage) { return {}; }
#endif
} // namespace media_preview
