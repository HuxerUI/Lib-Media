#include "decoded_playback/device.h"

#include <audioclient.h>
#include <audiopolicy.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>
#include <wrl/implements.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>

#define HUXERUI_MEDIA_DISCARD(expression) static_cast<void>(expression)

namespace huxerui::media::detail {
namespace {
using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;

void Require(HRESULT status, const char* operation, PlaybackErrorCode code = PlaybackErrorCode::DeviceFailure) {
  if (FAILED(status)) throw PlaybackError{code, operation, std::to_string(status)};
}

template<class T, class Function> PlaybackResult<T> Guard(Function function) {
  try {
    return PlaybackResult<T>::Success(function());
  } catch (PlaybackError& error) {
    return PlaybackResult<T>::Failure(std::move(error));
  }
}

struct OutputStatus {
  std::atomic_bool active{true}, lost{false};
  std::function<void()> wakeup;
};

class SessionEvents final
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IAudioSessionEvents> {
public:
  explicit SessionEvents(std::shared_ptr<OutputStatus> status) : status_(std::move(status)) {}
  HRESULT STDMETHODCALLTYPE OnDisplayNameChanged(LPCWSTR, LPCGUID) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE OnIconPathChanged(LPCWSTR, LPCGUID) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE OnSimpleVolumeChanged(float, BOOL, LPCGUID) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE OnChannelVolumeChanged(DWORD, float[], DWORD, LPCGUID) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE OnGroupingParamChanged(LPCGUID, LPCGUID) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE OnStateChanged(AudioSessionState) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE OnSessionDisconnected(AudioSessionDisconnectReason) override {
    if (status_->active.load() && !status_->lost.exchange(true) && status_->wakeup) {
      try { status_->wakeup(); } catch (...) { return E_FAIL; }
    }
    return S_OK;
  }
private:
  const std::shared_ptr<OutputStatus> status_;
};

class WindowsPlaybackDevice final : public PlaybackDevice {
public:
  ~WindowsPlaybackDevice() override {
    try { HUXERUI_MEDIA_DISCARD(Close()); } catch (...) {}
  }

  PlaybackResult<bool> Open(const AudioFormat& format, std::size_t capacity, std::function<void()> output_lost) override {
    return Guard<bool>([&] {
      rate_ = format.sample_rate;
      channels_ = format.ChannelCount();
      if (!rate_ || !channels_ || rate_ > MAXDWORD / (channels_ * sizeof(std::int16_t)))
        throw PlaybackError{PlaybackErrorCode::Unsupported, "The PCM format exceeds WASAPI limits", {}};
      Require(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "Cannot initialize COM for PCM output");
      com_ = true;
      ComPtr<IMMDeviceEnumerator> devices;
      Require(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&devices)),
              "Cannot enumerate Windows audio devices", PlaybackErrorCode::Unsupported);
      Require(devices->GetDefaultAudioEndpoint(eRender, eConsole, &endpoint_),
              "No default Windows audio output is available", PlaybackErrorCode::Unsupported);
      Require(endpoint_->Activate(__uuidof(IAudioClient), CLSCTX_INPROC_SERVER, nullptr,
                                  reinterpret_cast<void**>(client_.GetAddressOf())), "Cannot open the Windows audio client");

      WAVEFORMATEX pcm{};
      pcm.wFormatTag = WAVE_FORMAT_PCM;
      pcm.nChannels = WORD(channels_);
      pcm.nSamplesPerSec = rate_;
      pcm.wBitsPerSample = 16;
      pcm.nBlockAlign = WORD(channels_ * sizeof(std::int16_t));
      pcm.nAvgBytesPerSec = rate_ * pcm.nBlockAlign;
      const auto frames = std::min(capacity, std::size_t(std::max(1u, rate_ / 10)));
      const auto duration = REFERENCE_TIME((std::uint64_t(frames) * 10000000 + rate_ - 1) / rate_);
      Require(client_->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                  AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY |
                                      AUDCLNT_STREAMFLAGS_NOPERSIST,
                                  duration, 0, &pcm, nullptr), "WASAPI rejected the PCM format", PlaybackErrorCode::Unsupported);
      initialized_ = true;
      Require(client_->GetBufferSize(&capacity_), "Cannot query the WASAPI buffer size");
      Require(client_->GetService(IID_PPV_ARGS(&render_)), "Cannot open the WASAPI render interface");
      Require(client_->GetService(IID_PPV_ARGS(&clock_)), "Cannot open the WASAPI playback clock");
      Require(clock_->GetFrequency(&frequency_), "Cannot query the WASAPI clock frequency");
      if (!capacity_ || !frequency_ || frequency_ > UINT32_MAX)
        throw PlaybackError{PlaybackErrorCode::Unsupported, "WASAPI reported unsupported buffer or clock bounds", {}};
      status_ = std::make_shared<OutputStatus>();
      status_->wakeup = std::move(output_lost);
      events_ = Microsoft::WRL::Make<SessionEvents>(status_);
      if (!events_) throw PlaybackError{PlaybackErrorCode::ResourceExhausted, "Cannot allocate WASAPI session events", {}};
      Require(client_->GetService(IID_PPV_ARGS(&session_)), "Cannot observe the WASAPI audio session");
      Require(session_->RegisterAudioSessionNotification(events_.Get()), "Cannot register WASAPI session events");
      registered_ = true;
      return true;
    });
  }

  PlaybackResult<std::size_t> Write(std::span<const std::int16_t> samples) override {
    return Guard<std::size_t>([&] {
      if (!accepts_media_) return std::size_t(0);
      const auto padding = Padding();
      // A stream clock continues across an underrun. Never append media after that gap.
      if (started_ && padding == 0) {
        accepts_media_ = false;
        return std::size_t(0);
      }
      const auto frames = std::min(samples.size() / channels_, std::size_t(capacity_ - padding));
      if (!frames) return std::size_t(0);
      if (written_ > UINT64_MAX - frames)
        throw PlaybackError{PlaybackErrorCode::DeviceFailure, "WASAPI media count overflow", {}};
      BYTE* target = nullptr;
      Require(render_->GetBuffer(UINT32(frames), &target), "Cannot acquire the WASAPI render buffer");
      std::memcpy(target, samples.data(), frames * channels_ * sizeof(std::int16_t));
      Require(render_->ReleaseBuffer(UINT32(frames), 0), "Cannot submit the WASAPI render buffer");
      written_ += frames;
      return frames;
    });
  }

  PlaybackResult<bool> Start() override {
    return Guard<bool>([&] {
      Require(client_->Start(), "Cannot start WASAPI output");
      started_ = running_ = true;
      progressed_ = Clock::now();
      return true;
    });
  }

  PlaybackResult<bool> Pause() override {
    return Guard<bool>([&] {
      Require(client_->Stop(), "Cannot pause WASAPI output");
      running_ = false;
      return true;
    });
  }

  PlaybackResult<PlaybackProgress> Observe(bool) override {
    return Guard<PlaybackProgress>([&] {
      const auto padding = Padding();
      UINT64 position = 0;
      const auto status = clock_->GetPosition(&position, nullptr);
      Require(status, "Cannot observe the WASAPI playback clock");
      if (status == S_OK) {
        if (position < clock_position_)
          throw PlaybackError{PlaybackErrorCode::DeviceFailure, "The WASAPI playback clock regressed", {}};
        clock_position_ = position;
        if (running_) {
          const auto whole = position / frequency_;
          if (whole > UINT64_MAX / rate_)
            throw PlaybackError{PlaybackErrorCode::DeviceFailure, "The WASAPI playback clock overflowed", {}};
          const auto fraction = position % frequency_ * rate_ / frequency_;
          const auto count = whole * rate_;
          if (fraction > UINT64_MAX - count)
            throw PlaybackError{PlaybackErrorCode::DeviceFailure, "The WASAPI playback clock overflowed", {}};
          const auto presented = std::min(written_, count + fraction);
          if (presented > presented_) {
            presented_ = presented;
            progressed_ = Clock::now();
          }
        }
      }
      if (started_ && (padding == 0 || presented_ == written_)) accepts_media_ = false;
      if (running_ && presented_ < written_ && Clock::now() - progressed_ >= std::chrono::seconds(5))
        throw PlaybackError{PlaybackErrorCode::DeviceFailure, "The WASAPI playback clock stopped confirming PCM", {}};
      return PlaybackProgress{presented_, accepts_media_};
    });
  }

  PlaybackResult<bool> Close() override {
    HRESULT failure = S_OK;
    if (status_) status_->active = false;
    if (std::exchange(registered_, false)) failure = session_->UnregisterAudioSessionNotification(events_.Get());
    if (initialized_) {
      const auto stopped = client_->Stop();
      if (SUCCEEDED(failure)) failure = stopped;
      const auto reset = client_->Reset();
      if (SUCCEEDED(failure)) failure = reset;
    }
    session_.Reset();
    events_.Reset();
    status_.reset();
    clock_.Reset();
    render_.Reset();
    client_.Reset();
    endpoint_.Reset();
    initialized_ = running_ = started_ = false;
    if (std::exchange(com_, false)) CoUninitialize();
    if (FAILED(failure)) return PlaybackFailure<bool>(PlaybackErrorCode::DeviceFailure,
                                                     "Cannot stop and reset WASAPI output", std::to_string(failure));
    return PlaybackResult<bool>::Success(true);
  }

private:
  UINT32 Padding() {
    if (status_ && status_->lost.load())
      throw PlaybackError{PlaybackErrorCode::DeviceFailure, "The WASAPI audio session was disconnected", {}};
    DWORD state = 0;
    Require(endpoint_->GetState(&state), "Cannot query the Windows audio endpoint");
    if (!(state & DEVICE_STATE_ACTIVE))
      throw PlaybackError{PlaybackErrorCode::DeviceFailure, "The Windows audio endpoint is no longer active", {}};
    UINT32 padding = 0;
    Require(client_->GetCurrentPadding(&padding), "Cannot query WASAPI output capacity");
    if (padding > capacity_)
      throw PlaybackError{PlaybackErrorCode::DeviceFailure, "WASAPI reported invalid output capacity", {}};
    return padding;
  }

  ComPtr<IMMDevice> endpoint_;
  ComPtr<IAudioClient> client_;
  ComPtr<IAudioRenderClient> render_;
  ComPtr<IAudioClock> clock_;
  ComPtr<IAudioSessionControl> session_;
  ComPtr<SessionEvents> events_;
  std::shared_ptr<OutputStatus> status_;
  UINT32 rate_ = 0, channels_ = 0, capacity_ = 0;
  UINT64 frequency_ = 0, clock_position_ = 0, written_ = 0, presented_ = 0;
  bool com_ = false, initialized_ = false, running_ = false, started_ = false, accepts_media_ = true, registered_ = false;
  Clock::time_point progressed_{};
};
} // namespace

std::unique_ptr<PlaybackDevice> MakePlaybackDevice() { return std::make_unique<WindowsPlaybackDevice>(); }
} // namespace huxerui::media::detail

#undef HUXERUI_MEDIA_DISCARD
