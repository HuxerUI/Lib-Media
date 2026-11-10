#include "media_video_android.h"

#include "codec/common.h"
#include "format/platform_video_storage.h"
#include "format/video_frame_transformer.h"

#include <huxerui/android/external_texture.h>

#include <algorithm>
#include <android/hardware_buffer.h>
#include <dlfcn.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <stdexcept>

namespace huxerui::media {
namespace {

template<class Symbol> Symbol FindSymbol(const char* name) {
  return reinterpret_cast<Symbol>(dlsym(RTLD_DEFAULT, name));
}

struct ImageApi {
  media_status_t (*open_reader)(int32_t, int32_t, int32_t, uint64_t, int32_t, AImageReader**) =
      FindSymbol<decltype(open_reader)>("AImageReader_newWithUsage");
  void (*close_reader)(AImageReader*) = FindSymbol<decltype(close_reader)>("AImageReader_delete");
  media_status_t (*window)(AImageReader*, ANativeWindow**) = FindSymbol<decltype(window)>("AImageReader_getWindow");
  media_status_t (*read)(AImageReader*, AImage**) = FindSymbol<decltype(read)>("AImageReader_acquireNextImage");
  void (*close_image)(AImage*) = FindSymbol<decltype(close_image)>("AImage_delete");
  media_status_t (*image_buffer)(const AImage*, AHardwareBuffer**) = FindSymbol<decltype(image_buffer)>("AImage_getHardwareBuffer");
  media_status_t (*crop)(const AImage*, AImageCropRect*) = FindSymbol<decltype(crop)>("AImage_getCropRect");
  void (*describe)(const AHardwareBuffer*, AHardwareBuffer_Desc*) = FindSymbol<decltype(describe)>("AHardwareBuffer_describe");
  void (*acquire)(AHardwareBuffer*) = FindSymbol<decltype(acquire)>("AHardwareBuffer_acquire");
  void (*release)(AHardwareBuffer*) = FindSymbol<decltype(release)>("AHardwareBuffer_release");
  int (*allocate)(const AHardwareBuffer_Desc*, AHardwareBuffer**) = FindSymbol<decltype(allocate)>("AHardwareBuffer_allocate");

  bool Available() const {
    return open_reader && close_reader && window && read && close_image && image_buffer && crop &&
        describe && acquire && release && allocate;
  }
};

const ImageApi& Images() {
  static const ImageApi api;
  return api;
}

struct HardwareBufferStorage final : detail::PlatformVideoStorage {
  HardwareBufferStorage(AHardwareBuffer* buffer, const VideoFormat& format, std::shared_ptr<const void> lease)
      : PlatformVideoStorage(format.width, format.height, format.pixel_format), buffer(buffer), lease(std::move(lease)) {
    Images().acquire(buffer);
  }
  ~HardwareBufferStorage() override { Images().release(buffer); }
  AHardwareBuffer* buffer;
  std::shared_ptr<const void> lease;
};

class CurrentEgl {
public:
  CurrentEgl(EGLDisplay display, EGLSurface surface, EGLContext context) : display_(display) {
    if (!eglBindAPI(EGL_OPENGL_ES_API) || !eglMakeCurrent(display, surface, surface, context)) {
      eglBindAPI(api_);
      throw std::runtime_error("Cannot activate the video EGL context");
    }
  }
  ~CurrentEgl() {
    eglBindAPI(api_);
    if (context_ == EGL_NO_CONTEXT) eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    else eglMakeCurrent(previous_, draw_, read_, context_);
  }
private:
  EGLDisplay display_;
  EGLDisplay previous_ = eglGetCurrentDisplay();
  EGLSurface draw_ = eglGetCurrentSurface(EGL_DRAW), read_ = eglGetCurrentSurface(EGL_READ);
  EGLContext context_ = eglGetCurrentContext();
  EGLenum api_ = eglQueryAPI();
};

class VideoFramebuffer {
public:
  explicit VideoFramebuffer(GLuint color) {
    glGenFramebuffers(1, &name_);
    glBindFramebuffer(GL_FRAMEBUFFER, name_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color, 0);
  }
  ~VideoFramebuffer() { glDeleteFramebuffers(1, &name_); }
  bool Complete() const { return name_ && glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE; }
private:
  GLuint name_ = 0;
};

GLuint VideoShader(GLenum type, const char* source) {
  const auto shader = glCreateShader(type);
  glShaderSource(shader, 1, &source, nullptr);
  glCompileShader(shader);
  GLint ready = GL_FALSE;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &ready);
  if (ready != GL_TRUE) {
    char log[512]{};
    glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
    glDeleteShader(shader);
    throw std::runtime_error(std::string("Cannot compile the GLES video shader: ") + log);
  }
  return shader;
}

GLuint VideoProgram() {
  const auto vertex = VideoShader(GL_VERTEX_SHADER, R"glsl(#version 300 es
    out vec2 point;
    void main() {
      point = vec2(gl_VertexID & 1, gl_VertexID >> 1);
      gl_Position = vec4(2.0 * point - 1.0, 0.0, 1.0);
    }
  )glsl");
  GLuint fragment = 0;
  try {
    fragment = VideoShader(GL_FRAGMENT_SHADER, R"glsl(#version 300 es
      #extension GL_OES_EGL_image_external_essl3 : require
      precision highp float;
      uniform samplerExternalOES image;
      uniform vec2 map[3];
      uniform vec2 half_texel;
      uniform int window;
      in vec2 point;
      out vec4 color;
      void main() {
        vec2 p = vec2(point.x, window == 1 ? 1.0 - point.y : point.y);
        vec2 opposite = map[0] + map[1] + map[2];
        vec2 low = min(map[0], opposite), high = max(map[0], opposite);
        vec2 margin = min(half_texel, (high - low) * 0.5);
        vec2 uv = clamp(map[0] + p.x * map[1] + p.y * map[2], low + margin, high - margin);
        color = vec4(texture(image, uv).rgb, 1.0);
      }
    )glsl");
  } catch (...) {
    glDeleteShader(vertex);
    throw;
  }
  const auto program = glCreateProgram();
  glAttachShader(program, vertex);
  glAttachShader(program, fragment);
  glLinkProgram(program);
  glDeleteShader(vertex);
  glDeleteShader(fragment);
  GLint ready = GL_FALSE;
  glGetProgramiv(program, GL_LINK_STATUS, &ready);
  if (ready != GL_TRUE) {
    glDeleteProgram(program);
    throw std::runtime_error("Cannot link the GLES video program");
  }
  return program;
}

template<class Symbol> Symbol EglProc(const char* name) {
  const auto proc = reinterpret_cast<Symbol>(eglGetProcAddress(name));
  if (!proc) throw std::runtime_error(std::string("Missing EGL video extension: ") + name);
  return proc;
}

CodecResult<VideoFrame> RetainHardwareBuffer(AHardwareBuffer* buffer, VideoFormat format, MediaTimestamp pts,
    std::optional<MediaTimestamp> duration, std::shared_ptr<const void> lease) {
  using R = CodecResult<VideoFrame>;
  if (!buffer || !Images().Available()) return R::Failure(detail::Error(CodecErrorCode::Unsupported, "Expected an Android hardware buffer"));
  AHardwareBuffer_Desc description{};
  Images().describe(buffer, &description);
  if (format.pixel_format != VideoPixelFormat::Opaque || description.layers != 1 ||
      description.width != format.width || description.height != format.height ||
      !(description.usage & AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE))
    return R::Failure(detail::Error(CodecErrorCode::InvalidArgument, "Expected a matching GPU-sampleable Android video image"));
  auto frame = detail::VideoFrameAccess::FromPlatform(format, pts, duration,
      std::make_shared<HardwareBufferStorage>(buffer, format, std::move(lease)));
  if (!frame.Validate()) return R::Failure(detail::Error(CodecErrorCode::InvalidArgument, "Invalid platform video frame metadata"));
  return R::Success(std::move(frame));
}

} // namespace

namespace detail {

AHardwareBuffer* HardwareBuffer(const VideoFrame& frame) noexcept {
  const auto* storage = dynamic_cast<const HardwareBufferStorage*>(VideoFrameAccess::PlatformStorage(frame).get());
  return storage ? storage->buffer : nullptr;
}

CodecResult<std::shared_ptr<AImageReader>> OpenVideoImageReader(const VideoDecoderConfig& config) {
  using R = CodecResult<std::shared_ptr<AImageReader>>;
  const auto& api = Images();
  if (!api.Available()) return R::Failure(Error(CodecErrorCode::Unsupported, "Hardware buffer output requires Android API 26"));
  AImageReader* reader = nullptr;
  // MediaCodec includes the consumer's acquired-image limit in its output buffer count.
  constexpr int slots = 4;
  const auto status = api.open_reader(config.width, config.height, AIMAGE_FORMAT_PRIVATE,
      AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE, slots, &reader);
  if (status != AMEDIA_OK) return R::Failure(Error(CodecErrorCode::Unsupported, "Cannot open the Android decoder image reader", std::to_string(status)));
  return R::Success(std::shared_ptr<AImageReader>(reader, api.close_reader));
}

ANativeWindow* VideoImageReaderWindow(AImageReader* reader) {
  ANativeWindow* window = nullptr;
  if (Images().window(reader, &window) != AMEDIA_OK) return nullptr;
  return window;
}

CodecResult<std::optional<VideoFrame>> ReadVideoImage(const std::shared_ptr<AImageReader>& reader,
    VideoFormat format, MediaTimestamp pts) {
  using R = CodecResult<std::optional<VideoFrame>>;
  const auto& api = Images();
  AImage* image = nullptr;
  const auto status = api.read(reader.get(), &image);
  if (status == AMEDIA_IMGREADER_NO_BUFFER_AVAILABLE || status == AMEDIA_IMGREADER_MAX_IMAGES_ACQUIRED)
    return R::Success(std::nullopt);
  if (status != AMEDIA_OK)
    return R::Failure(Error(CodecErrorCode::PlatformFailure, "Cannot acquire the decoded Android image", std::to_string(status)));
  auto lease = std::shared_ptr<AImage>(image, [keep = reader, close = api.close_image](AImage* value) mutable {
    close(value);
    keep.reset();
  });
  AHardwareBuffer* buffer = nullptr;
  if (api.image_buffer(image, &buffer) != AMEDIA_OK || !buffer)
    return R::Failure(Error(CodecErrorCode::Unsupported, "Android did not expose a decoded hardware buffer"));
  AHardwareBuffer_Desc description{};
  api.describe(buffer, &description);
  AImageCropRect crop{};
  if (api.crop(image, &crop) != AMEDIA_OK || crop.left < 0 || crop.top < 0 ||
      crop.right <= crop.left || crop.bottom <= crop.top)
    return R::Failure(Error(CodecErrorCode::InvalidData, "Invalid Android decoder image crop"));
  format.width = description.width;
  format.height = description.height;
  format.visible = {std::uint32_t(crop.left), std::uint32_t(crop.top), std::uint32_t(crop.right - crop.left),
      std::uint32_t(crop.bottom - crop.top)};
  auto frame = RetainHardwareBuffer(buffer, format, pts, {}, std::move(lease));
  if (!frame.Succeeded()) return R::Failure(frame.Error());
  return R::Success(std::move(frame).Value());
}

struct AndroidVideoRenderer::Context {
  EGLDisplay display = EGL_NO_DISPLAY;
  EGLContext context = EGL_NO_CONTEXT;
  EGLSurface surface = EGL_NO_SURFACE;
  GLuint program = 0;
  GLint mapping = -1, half_texel = -1, window_uniform = -1;
  bool window = false;
  PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC client_buffer = nullptr;
  PFNEGLCREATEIMAGEKHRPROC create_image = nullptr;
  PFNEGLDESTROYIMAGEKHRPROC destroy_image = nullptr;
  PFNGLEGLIMAGETARGETTEXTURE2DOESPROC bind_image = nullptr;
  PFNEGLPRESENTATIONTIMEANDROIDPROC presentation_time = nullptr;

  ~Context() {
    if (context != EGL_NO_CONTEXT) eglDestroyContext(display, context);
    if (surface != EGL_NO_SURFACE) eglDestroySurface(display, surface);
  }

  void Open(ANativeWindow* target) {
    window = target != nullptr;
    display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display == EGL_NO_DISPLAY || !eglInitialize(display, nullptr, nullptr))
      throw std::runtime_error("Cannot initialize EGL for video rendering");
    const EGLint attributes[]{EGL_SURFACE_TYPE, window ? EGL_WINDOW_BIT : EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
        window ? EGL_RECORDABLE_ANDROID : EGL_NONE, EGL_TRUE, EGL_NONE};
    EGLConfig config{};
    EGLint count = 0;
    if (!eglChooseConfig(display, attributes, &config, 1, &count) || !count)
      throw std::runtime_error("No compatible EGL configuration for video rendering");
    const EGLint pbuffer[]{EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
    surface = window ? eglCreateWindowSurface(display, config, target, nullptr) :
        eglCreatePbufferSurface(display, config, pbuffer);
    const EGLint version[]{EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    const auto api = eglQueryAPI();
    if (!eglBindAPI(EGL_OPENGL_ES_API)) throw std::runtime_error("Cannot bind OpenGL ES for video rendering");
    context = eglCreateContext(display, config, EGL_NO_CONTEXT, version);
    eglBindAPI(api);
    if (surface == EGL_NO_SURFACE || context == EGL_NO_CONTEXT)
      throw std::runtime_error("Cannot create the video EGL surface or context");
    CurrentEgl current(display, surface, context);
    client_buffer = EglProc<PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC>("eglGetNativeClientBufferANDROID");
    create_image = EglProc<PFNEGLCREATEIMAGEKHRPROC>("eglCreateImageKHR");
    destroy_image = EglProc<PFNEGLDESTROYIMAGEKHRPROC>("eglDestroyImageKHR");
    bind_image = EglProc<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>("glEGLImageTargetTexture2DOES");
    if (window) presentation_time = EglProc<PFNEGLPRESENTATIONTIMEANDROIDPROC>("eglPresentationTimeANDROID");
    program = VideoProgram();
    mapping = glGetUniformLocation(program, "map");
    half_texel = glGetUniformLocation(program, "half_texel");
    window_uniform = glGetUniformLocation(program, "window");
  }

  struct Texture {
    Texture(const Context& egl, AHardwareBuffer* buffer, GLenum type) : egl(egl) {
      const EGLint attributes[]{EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
      const auto client = egl.client_buffer(buffer);
      if (!client) throw std::runtime_error("Cannot obtain an EGL video buffer");
      image = egl.create_image(egl.display, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID, client, attributes);
      if (image == EGL_NO_IMAGE_KHR) throw std::runtime_error("Cannot import the hardware buffer as an EGL image");
      glGenTextures(1, &name);
      glBindTexture(type, name);
      egl.bind_image(type, image);
      glTexParameteri(type, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(type, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexParameteri(type, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(type, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    ~Texture() {
      glDeleteTextures(1, &name);
      egl.destroy_image(egl.display, image);
    }
    const Context& egl;
    EGLImageKHR image = EGL_NO_IMAGE_KHR;
    GLuint name = 0;
  };

  void Draw(const VideoFrame& frame, unsigned width, unsigned height) const {
    const auto source = HardwareBuffer(frame);
    if (!source) throw std::runtime_error("Expected an Android hardware video buffer");
    glActiveTexture(GL_TEXTURE0);
    const Texture texture(*this, source, GL_TEXTURE_EXTERNAL_OES);
    const auto coordinates = VideoSampleCoordinates(frame.format);
    glViewport(0, 0, width, height);
    glUseProgram(program);
    glUniform2fv(mapping, 3, coordinates.front().data());
    glUniform2f(half_texel, 0.5f / frame.format.width, 0.5f / frame.format.height);
    glUniform1i(window_uniform, window);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    // The image lease can end as soon as this call returns.
    glFinish();
    if (glGetError() != GL_NO_ERROR) throw std::runtime_error("Cannot render the video image");
  }
};

AndroidVideoRenderer::AndroidVideoRenderer(ANativeWindow* window) : context_(std::make_unique<Context>()) {
  context_->Open(window);
}
AndroidVideoRenderer::~AndroidVideoRenderer() = default;

void AndroidVideoRenderer::DrawToSurface(const VideoFrame& frame) {
  const auto ns = frame.pts.Rescale(1000000000);
  if (!ns) throw std::runtime_error("Surface presentation time exceeds the nanosecond clock");
  const auto& egl = *context_;
  CurrentEgl current(egl.display, egl.surface, egl.context);
  egl.Draw(frame, frame.format.width, frame.format.height);
  if (!egl.presentation_time(egl.display, egl.surface, ns->ticks) || !eglSwapBuffers(egl.display, egl.surface))
    throw std::runtime_error("Cannot submit the rendered video to MediaCodec");
}

CodecResult<VideoFrame> AndroidVideoRenderer::DrawToBuffer(const VideoFrame& source, VideoFormat target) {
  using R = CodecResult<VideoFrame>;
  const auto& api = Images();
  if (!api.Available()) return R::Failure(Error(CodecErrorCode::Unsupported, "GPU video transforms require Android API 26"));
  const AHardwareBuffer_Desc description{.width = target.width, .height = target.height, .layers = 1,
      .format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM,
      .usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE | AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER};
  AHardwareBuffer* buffer = nullptr;
  if (api.allocate(&description, &buffer) != 0)
    return R::Failure(Error(CodecErrorCode::ResourceExhausted, "Cannot allocate the scaled Android video buffer"));
  const std::unique_ptr<AHardwareBuffer, decltype(api.release)> owned(buffer, api.release);
  const auto& egl = *context_;
  CurrentEgl current(egl.display, egl.surface, egl.context);
  const Context::Texture output(egl, buffer, GL_TEXTURE_2D);
  const VideoFramebuffer framebuffer(output.name);
  if (!framebuffer.Complete())
    return R::Failure(Error(CodecErrorCode::Unsupported, "The GPU cannot render to the Android video buffer"));
  egl.Draw(source, target.width, target.height);
  return RetainHardwareBuffer(buffer, target, source.pts, source.duration, {});
}

void AndroidVideoRenderer::DrawToTexture(const VideoFrame& frame, huxerui::android::GlTexture& texture) {
  const auto& egl = *context_;
  CurrentEgl current(egl.display, egl.surface, egl.context);
  const auto width = int(frame.format.width), height = int(frame.format.height);
  struct RenderTexture {
    RenderTexture(int width, int height) {
      glGenTextures(1, &name);
      glBindTexture(GL_TEXTURE_2D, name);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    }
    ~RenderTexture() { glDeleteTextures(1, &name); }
    GLuint name = 0;
  } output(width, height);
  const VideoFramebuffer framebuffer(output.name);
  if (!framebuffer.Complete()) throw std::runtime_error("Cannot prepare the Android video texture");
  egl.Draw(frame, width, height);
  using Texture = huxerui::android::GlTexture;
  texture.PublishCurrent({output.name, width, height, -1, Texture::Origin::TopLeft, Texture::Alpha::Opaque});
}

CodecResult<std::shared_ptr<ExternalTexture>> MakePlatformVideoTexture(const VideoFrame& frame) {
  using R = CodecResult<std::shared_ptr<ExternalTexture>>;
  if (!HardwareBuffer(frame)) return R::Failure(Error(CodecErrorCode::Unsupported, "Expected an Android hardware video buffer"));
  auto texture = std::make_shared<huxerui::android::GlTexture>(Size{float(frame.format.width), float(frame.format.height)});
  AndroidVideoRenderer().DrawToTexture(frame, *texture);
  texture->Finish();
  return R::Success(std::move(texture));
}

class GlesVideoFrameTransformer final : public VideoFrameTransformBackend {
public:
  CodecResult<VideoFrame> Transform(const VideoFrame& source, VideoFormat target) override {
    if (!renderer_) renderer_ = std::make_unique<AndroidVideoRenderer>();
    return renderer_->DrawToBuffer(source, target);
  }
private:
  std::unique_ptr<AndroidVideoRenderer> renderer_;
};

std::unique_ptr<VideoFrameTransformBackend> MakeVideoFrameTransformBackend() { return std::make_unique<GlesVideoFrameTransformer>(); }

} // namespace detail
} // namespace huxerui::media
