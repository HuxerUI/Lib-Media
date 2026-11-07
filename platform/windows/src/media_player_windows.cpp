#include <huxerui/windows/platform_registry.h>
#include <huxerui/window.h>

#include <d3d11.h>
#include <dxgi1_2.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfmediaengine.h>
#include <wrl.h>
#include <wrl/implements.h>

#include <atomic>
#include <cmath>
#include <limits>
#include <mutex>
#include <iomanip>
#include <sstream>

#include "player/state.h"
#include "player/video_geometry.h"
#include "media_player_windows.h"

namespace huxerui::media::detail {
namespace {

using Microsoft::WRL::ComPtr;
constexpr UINT media_message = WM_APP + 147;
constexpr wchar_t dispatch_class[] = L"HuxerUI.MediaPlayer.Dispatch";
constexpr wchar_t video_class[] = L"HuxerUI.MediaPlayer.Video";

std::wstring Wide(const std::string& text) {
  const int length =
      MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
  if (!length && !text.empty())
    throw std::invalid_argument("HuxerUI media source contains invalid UTF-8");
  std::wstring result(length, L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), length);
  return result;
}

void Require(HRESULT result) {
  if (FAILED(result))
    throw result;
}

struct CallbackTarget {
  std::atomic<HWND> window{nullptr};
};

class EngineNotify final
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
                                          IMFMediaEngineNotify> {
public:
  std::shared_ptr<CallbackTarget> target;
  std::uint64_t cookie = 0;
  HRESULT STDMETHODCALLTYPE EventNotify(DWORD event, DWORD_PTR parameter, DWORD) override {
    if (event == MF_MEDIA_ENGINE_EVENT_NOTIFYSTABLESTATE) {
      SetEvent(reinterpret_cast<HANDLE>(parameter));
      return S_OK;
    }
    if (auto window = target->window.load())
      PostMessageW(window, media_message, static_cast<WPARAM>(cookie), static_cast<LPARAM>(event));
    return S_OK;
  }
};

class WindowsPlayer;

class WindowsPlayer final : public PlayerBackend, public std::enable_shared_from_this<WindowsPlayer> {
public:
  WindowsPlayer();
  ~WindowsPlayer() override;
  void Load(const MediaSource& source, std::uint64_t generation) override;
  void Clear() noexcept override;
  void Play() override;
  void Pause() override;
  void Seek(MediaTime position, std::uint64_t seek_id) override;
  void SetVolume(double volume, bool muted) override;
  void Poll() override;
  void OnEvent(DWORD event);
  void Render(VideoWindow& view);
  void Error(HRESULT result, MediaErrorCode code, bool fatal, const char* operation = "media operation");
  void Send(PlayerEventKind kind, std::uint64_t seek_id = 0);
  PlayerSnapshot Snapshot();

  HWND dispatch_window = nullptr;
  std::uint64_t cookie = 0;
  std::weak_ptr<VideoWindow> video;

private:
  void CreateEngine();
  std::shared_ptr<CallbackTarget> callback_target_ = std::make_shared<CallbackTarget>();
  std::optional<MediaSource> source_;
  std::uint64_t generation_ = 0;
  std::uint64_t seek_id_ = 0;
  bool started_ = false;
  bool com_initialized_ = false;
  bool ready_ = false;
  bool buffering_ = false;
  bool output_failed_ = false;
  double volume_ = 1;
  bool muted_ = false;
  ComPtr<IMFMediaEngine> engine_;
  ComPtr<ID3D11Device> device_;
  ComPtr<ID3D11DeviceContext> context_;
  ComPtr<IMFDXGIDeviceManager> manager_;
};

LRESULT CALLBACK DispatchProcedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  auto* player = reinterpret_cast<WindowsPlayer*>(GetWindowLongPtrW(window, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    player = static_cast<WindowsPlayer*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(player));
  }
  if (message == media_message && player && player->cookie == wparam) {
    player->OnEvent(static_cast<DWORD>(lparam));
    return 0;
  }
  return DefWindowProcW(window, message, wparam, lparam);
}

LRESULT CALLBACK VideoProcedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  auto* video = reinterpret_cast<VideoWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    video = static_cast<VideoWindow*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(video));
  }
  if (video && (message == WM_TIMER || message == WM_SIZE || message == WM_PAINT)) {
    if (message != WM_TIMER)
      video->dirty = true;
    if (message == WM_PAINT) {
      PAINTSTRUCT paint;
      HDC dc = BeginPaint(window, &paint);
      FillRect(dc, &paint.rcPaint, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
      EndPaint(window, &paint);
    }
    if (auto player = video->player.lock())
      std::static_pointer_cast<WindowsPlayer>(player)->Render(*video);
    return 0;
  }
  if (message == WM_ERASEBKGND)
    return 1;
  return DefWindowProcW(window, message, wparam, lparam);
}

void RegisterWindows() {
  static std::once_flag registration;
  std::call_once(registration, [] {
    WNDCLASSW dispatch{};
    dispatch.hInstance = GetModuleHandleW(nullptr);
    dispatch.lpfnWndProc = DispatchProcedure;
    dispatch.lpszClassName = dispatch_class;
    if (!RegisterClassW(&dispatch) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
      throw std::runtime_error("HuxerUI media dispatch registration failed");
    dispatch.lpfnWndProc = VideoProcedure;
    dispatch.lpszClassName = video_class;
    dispatch.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    if (!RegisterClassW(&dispatch) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
      throw std::runtime_error("HuxerUI video registration failed");
  });
}

WindowsPlayer::WindowsPlayer() {
  RegisterWindows();
  dispatch_window =
      CreateWindowExW(0, dispatch_class, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), this);
  if (!dispatch_window)
    throw std::runtime_error("HuxerUI media dispatch window creation failed");
  callback_target_->window = dispatch_window;
}

WindowsPlayer::~WindowsPlayer() {
  callback_target_->window = nullptr;
  Clear();
  DestroyWindow(dispatch_window);
  if (started_)
    MFShutdown();
  if (com_initialized_)
    CoUninitialize();
}

void WindowsPlayer::CreateEngine() {
  if (!started_) {
    if (!com_initialized_) {
      HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
      if (result != RPC_E_CHANGED_MODE) {
        Require(result);
        com_initialized_ = true;
      }
    }
    Require(MFStartup(MF_VERSION));
    started_ = true;
  }
  if (!device_) {
    Require(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                              D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT, nullptr, 0,
                              D3D11_SDK_VERSION, &device_, nullptr, &context_));
    ComPtr<ID3D10Multithread> multithread;
    Require(device_.As(&multithread));
    multithread->SetMultithreadProtected(TRUE);
    UINT token = 0;
    Require(MFCreateDXGIDeviceManager(&token, &manager_));
    Require(manager_->ResetDevice(device_.Get(), token));
  }
  ComPtr<IMFAttributes> attributes;
  Require(MFCreateAttributes(&attributes, 3));
  auto notify = Microsoft::WRL::Make<EngineNotify>();
  notify->target = callback_target_;
  notify->cookie = cookie;
  Require(attributes->SetUnknown(MF_MEDIA_ENGINE_CALLBACK, notify.Get()));
  Require(attributes->SetUnknown(MF_MEDIA_ENGINE_DXGI_MANAGER, manager_.Get()));
  Require(attributes->SetUINT32(MF_MEDIA_ENGINE_VIDEO_OUTPUT_FORMAT, DXGI_FORMAT_B8G8R8A8_UNORM));
  ComPtr<IMFMediaEngineClassFactory> factory;
  Require(CoCreateInstance(CLSID_MFMediaEngineClassFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)));
  Require(factory->CreateInstance(0, attributes.Get(), &engine_));
  Require(engine_->SetVolume(volume_));
  Require(engine_->SetMuted(muted_));
}

void WindowsPlayer::Load(const MediaSource& source, std::uint64_t generation) {
  Clear();
  generation_ = generation;
  source_ = source;
  static std::atomic<std::uint64_t> next_cookie{1};
  cookie = next_cookie++;
  try {
    std::string location;
    const auto& input = PlayerAccess::Source(source);
    if (auto http = std::get_if<MediaHttpSource>(&input)) {
      if (!http->headers.empty()) {
        Error(E_NOTIMPL, MediaErrorCode::Unsupported, true);
        return;
      }
      location = http->url;
    } else {
      std::optional<File> file;
      if (auto local = std::get_if<File>(&input))
        file = *local;
      else
        file = std::get<FileReference>(input).AsFile();
      if (!file) {
        Error(E_NOTIMPL, MediaErrorCode::Unsupported, true);
        return;
      }
      location = file->ToUri().ToString();
    }
    CreateEngine();
    const auto wide = Wide(location);
    BSTR url = SysAllocStringLen(wide.data(), static_cast<UINT>(wide.size()));
    if (!url) {
      Error(E_OUTOFMEMORY, MediaErrorCode::Output, true);
      return;
    }
    const HRESULT result = engine_->SetSource(url);
    SysFreeString(url);
    Require(result);
    Require(engine_->Load());
  } catch (HRESULT result) {
    Error(result, MediaErrorCode::Output, true);
  } catch (const std::exception&) {
    Error(E_INVALIDARG, MediaErrorCode::InvalidSource, true);
  }
}

void WindowsPlayer::Clear() noexcept {
  cookie = 0;
  ready_ = false;
  buffering_ = false;
  seek_id_ = 0;
  output_failed_ = false;
  if (engine_) {
    engine_->Shutdown();
    engine_.Reset();
  }
  source_.reset();
  if (auto view = video.lock()) {
    view->swapchain.Reset();
    view->width = view->height = 0;
    InvalidateRect(view->window, nullptr, TRUE);
  }
}

void WindowsPlayer::Error(HRESULT result, MediaErrorCode code, bool fatal, const char* operation) {
  if (!emit)
    return;
  if (result == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) || result == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND))
    code = MediaErrorCode::NotFound;
  if (result == E_ACCESSDENIED)
    code = MediaErrorCode::PermissionDenied;
  if (result == MF_E_UNSUPPORTED_BYTESTREAM_TYPE)
    code = MediaErrorCode::Unsupported;
  std::ostringstream platform_code;
  platform_code << "0x" << std::uppercase << std::hex << std::setw(8) << std::setfill('0')
                << static_cast<std::uint32_t>(result);
  emit({.generation = generation_,
        .kind = PlayerEventKind::Error,
        .seek_id = seek_id_,
        .error = {code, std::string("HuxerUI Windows ") + operation + " failed", platform_code.str(), fatal}});
}

PlayerSnapshot WindowsPlayer::Snapshot() {
  PlayerSnapshot result;
  if (!engine_)
    return result;
  const double position = engine_->GetCurrentTime();
  const double duration = engine_->GetDuration();
  if (std::isfinite(position) && position >= 0)
    result.progress.position = MediaTime(position);
  if (std::isfinite(duration) && duration >= 0)
    result.progress.duration = MediaTime(duration);
  ComPtr<IMFMediaTimeRange> buffered;
  if (SUCCEEDED(engine_->GetBuffered(&buffered)) && buffered) {
    result.progress.buffered_ranges.emplace();
    for (DWORD index = 0; index < buffered->GetLength(); ++index) {
      double start = 0, end = 0;
      if (SUCCEEDED(buffered->GetStart(index, &start)) && SUCCEEDED(buffered->GetEnd(index, &end))) {
        result.progress.buffered_ranges->push_back({MediaTime(start), MediaTime(end)});
      }
    }
  }
  ComPtr<IMFMediaTimeRange> seekable;
  if (ready_ && SUCCEEDED(engine_->GetSeekable(&seekable))) {
    result.seekability = seekable && seekable->GetLength() && result.progress.duration ? MediaSeekability::Seekable
                                                                                       : MediaSeekability::NotSeekable;
  }
  if (ready_) {
    result.has_audio = engine_->HasAudio() != FALSE;
    result.has_video = engine_->HasVideo() != FALSE;
    DWORD width = 0, height = 0;
    if (SUCCEEDED(engine_->GetNativeVideoSize(&width, &height)) && width && height)
      result.video_size = Size{static_cast<float>(width), static_cast<float>(height)};
  }
  result.buffering = buffering_;
  return result;
}

void WindowsPlayer::Send(PlayerEventKind kind, std::uint64_t seek_id) {
  if (emit)
    emit({.generation = generation_, .kind = kind, .snapshot = Snapshot(), .seek_id = seek_id});
}

void WindowsPlayer::OnEvent(DWORD event) {
  if (!engine_)
    return;
  switch (event) {
  case MF_MEDIA_ENGINE_EVENT_LOADEDMETADATA:
    ready_ = true;
    Send(PlayerEventKind::Ready);
    break;
  case MF_MEDIA_ENGINE_EVENT_PLAYING:
    buffering_ = false;
    Send(PlayerEventKind::Playing);
    break;
  case MF_MEDIA_ENGINE_EVENT_PAUSE:
    Send(PlayerEventKind::Paused);
    break;
  case MF_MEDIA_ENGINE_EVENT_WAITING:
    buffering_ = true;
    Send(PlayerEventKind::Update);
    break;
  case MF_MEDIA_ENGINE_EVENT_CANPLAY:
    buffering_ = false;
    Send(PlayerEventKind::Update);
    break;
  case MF_MEDIA_ENGINE_EVENT_SEEKED: {
    const auto seek_id = std::exchange(seek_id_, 0);
    if (seek_id)
      Send(PlayerEventKind::Seeked, seek_id);
    break;
  }
  case MF_MEDIA_ENGINE_EVENT_ENDED:
    Send(PlayerEventKind::Ended);
    break;
  case MF_MEDIA_ENGINE_EVENT_ERROR: {
    ComPtr<IMFMediaError> error;
    engine_->GetError(&error);
    MediaErrorCode code = MediaErrorCode::Unknown;
    if (error) {
      switch (error->GetErrorCode()) {
      case MF_MEDIA_ENGINE_ERR_NETWORK:
        code = MediaErrorCode::Network;
        break;
      case MF_MEDIA_ENGINE_ERR_DECODE:
        code = MediaErrorCode::Decode;
        break;
      case MF_MEDIA_ENGINE_ERR_SRC_NOT_SUPPORTED:
        code = MediaErrorCode::Unsupported;
        break;
      }
    }
    Error(error ? error->GetExtendedErrorCode() : E_FAIL, code, true);
    break;
  }
  }
}

void WindowsPlayer::Play() {
  if (engine_) {
    const auto result = engine_->Play();
    if (FAILED(result))
      Error(result, MediaErrorCode::PlaybackNotAllowed, false);
  }
}
void WindowsPlayer::Pause() {
  if (engine_) {
    const auto result = engine_->Pause();
    if (FAILED(result))
      Error(result, MediaErrorCode::Output, false);
  }
}
void WindowsPlayer::Seek(MediaTime position, std::uint64_t seek_id) {
  seek_id_ = seek_id;
  if (engine_) {
    const auto result = engine_->SetCurrentTime(position.count());
    if (FAILED(result))
      Error(result, MediaErrorCode::SeekFailed, false);
  }
}
void WindowsPlayer::SetVolume(double volume, bool muted) {
  volume_ = volume;
  muted_ = muted;
  if (engine_) {
    auto result = engine_->SetVolume(volume);
    if (SUCCEEDED(result))
      result = engine_->SetMuted(muted);
    if (FAILED(result))
      Error(result, MediaErrorCode::Output, false);
  }
}
void WindowsPlayer::Poll() {
  if (ready_)
    Send(PlayerEventKind::Update);
}

void WindowsPlayer::Render(VideoWindow& view) {
  if (!engine_ || !ready_ || !engine_->HasVideo() || output_failed_ || !IsWindowVisible(view.window))
    return;
  if (engine_->GetReadyState() < MF_MEDIA_ENGINE_READY_HAVE_CURRENT_DATA)
    return;
  const char* operation = "video output setup";
  try {
    RECT bounds{};
    GetClientRect(view.window, &bounds);
    const UINT width = static_cast<UINT>(bounds.right);
    const UINT height = static_cast<UINT>(bounds.bottom);
    if (!width || !height)
      return;
    if (!view.swapchain) {
      ComPtr<IDXGIDevice> dxgi;
      Require(device_.As(&dxgi));
      ComPtr<IDXGIAdapter> adapter;
      Require(dxgi->GetAdapter(&adapter));
      ComPtr<IDXGIFactory2> factory;
      Require(adapter->GetParent(IID_PPV_ARGS(&factory)));
      DXGI_SWAP_CHAIN_DESC1 description{};
      description.Width = width;
      description.Height = height;
      description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
      description.SampleDesc.Count = 1;
      description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
      description.BufferCount = 2;
      description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
      Require(
          factory->CreateSwapChainForHwnd(device_.Get(), view.window, &description, nullptr, nullptr, &view.swapchain));
      view.width = width;
      view.height = height;
    } else if (view.width != width || view.height != height) {
      Require(view.swapchain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0));
      view.width = width;
      view.height = height;
    }
    LONGLONG presentation = 0;
    const auto tick = engine_->OnVideoStreamTick(&presentation);
    if (tick != S_OK && !view.dirty)
      return;
    ComPtr<ID3D11Texture2D> buffer;
    Require(view.swapchain->GetBuffer(0, IID_PPV_ARGS(&buffer)));
    DWORD source_width = 0, source_height = 0;
    Require(engine_->GetNativeVideoSize(&source_width, &source_height));
    const auto rectangles = FitWindowsVideo({static_cast<float>(source_width), static_cast<float>(source_height)},
                                            {static_cast<float>(width), static_cast<float>(height)}, view.properties);
    if (!rectangles)
      return;
    MFARGB black{0, 0, 0, 255};
    operation = "TransferVideoFrame";
    Require(engine_->TransferVideoFrame(buffer.Get(), &rectangles->source, &rectangles->destination, &black));
    operation = "Present";
    Require(view.swapchain->Present(0, 0));
    view.dirty = false;
  } catch (HRESULT result) {
    output_failed_ = true;
    Error(result, MediaErrorCode::Output, true, operation);
  }
}

} // namespace

std::shared_ptr<PlayerBackend> CreateWindowsPlayer() {
  return std::make_shared<WindowsPlayer>();
}

windows::PlatformViewFactory<VideoSurfaceProperties, VideoWindow, MediaPlayer> CreateWindowsVideoFactory() {
  return {
      .create =
          [](HWND parent, const VideoSurfaceProperties& properties, PlatformEventEmitter) {
            RegisterWindows();
            auto view = std::make_shared<VideoWindow>();
            view->properties = properties;
            view->window = CreateWindowExW(0, video_class, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 0, 0, 1, 1,
                                           parent, nullptr, GetModuleHandleW(nullptr), view.get());
            if (!view->window)
              throw std::runtime_error("HuxerUI video window creation failed");
            SetTimer(view->window, 1, 16, nullptr);
            return view;
          },
      .view = [](const std::shared_ptr<VideoWindow>& view) { return view->window; },
      .update =
          [](VideoWindow& view, const VideoSurfaceProperties& properties) {
            view.properties = properties;
            view.dirty = true;
          },
      .dispose =
          [](VideoWindow& view) {
            KillTimer(view.window, 1);
            view.swapchain.Reset();
            DestroyWindow(view.window);
            view.window = nullptr;
          },
      .connect =
          [](VideoWindow& view, const MediaPlayer& player) {
            PlayerAccess::Bind(player, [&view](const std::shared_ptr<PlayerBackend>& backend) {
              if (auto old = view.player.lock())
                std::static_pointer_cast<WindowsPlayer>(old)->video.reset();
              view.player.reset();
              view.swapchain.Reset();
              if (backend) {
                auto current = std::static_pointer_cast<WindowsPlayer>(backend);
                view.player = current;
                current->video = view.shared_from_this();
              }
              InvalidateRect(view.window, nullptr, TRUE);
            });
          },
      .disconnect = [](VideoWindow&, const MediaPlayer& player) { PlayerAccess::Unbind(player); },
  };
}

void InstallPlatform(ApplicationContext& context) {
  context.RegisterPlatformModule<std::shared_ptr<PlayerBackend>>(
      player_type, [](UiWindow&) -> std::shared_ptr<PlayerBackend> { return CreateWindowsPlayer(); });
  context.RegisterPlatformView<VideoSurfaceProperties, MediaPlayer>(video_type, CreateWindowsVideoFactory());
}

} // namespace huxerui::media::detail
