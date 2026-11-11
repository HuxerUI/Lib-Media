#include "codec/decoder/session.h"
#include "codec/decoder/copy.h"

#ifdef _WIN32
#include "media_codec_transform_windows.h"
#endif

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <thread>

namespace {
thread_local int allocations_until_failure = -1;
}

void* operator new(std::size_t size) {
  if (allocations_until_failure >= 0 && allocations_until_failure-- == 0) throw std::bad_alloc();
  if (auto* memory = std::malloc(size ? size : 1)) return memory;
  throw std::bad_alloc();
}
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }

using namespace huxerui;
using namespace huxerui::media;
using namespace huxerui::media::detail;
using State = DecoderState<AudioDecodeOutput>;
namespace {
void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
#ifdef _WIN32
void WindowsVideoColor() {
  Check(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)), "Could not initialize COM");
  struct Uninitialize { ~Uninitialize() { CoUninitialize(); } } uninitialize;
  Check(SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE)), "Could not initialize Media Foundation");
  struct Shutdown { ~Shutdown() { MFShutdown(); } } shutdown;
  Microsoft::WRL::ComPtr<IMFMediaType> type;
  Check(SUCCEEDED(MFCreateMediaType(&type)), "Could not create a Media Foundation output type");
  VideoFormat format;
  format.pixel_format = VideoPixelFormat::P010;
  ReadMediaFoundationVideoColor(type.Get(), format);
  Check(format.primaries == ColorPrimaries::Unknown && format.transfer == ColorTransfer::Unknown &&
        format.matrix == ColorMatrix::Unknown && format.range == ColorRange::Unknown, "Missing MF color attributes were inferred");
  Check(SUCCEEDED(type->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT2020)) &&
        SUCCEEDED(type->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235)), "Could not set MF HDR color attributes");
  for (const auto transfer : {MFVideoTransFunc_2084, MFVideoTransFunc_HLG}) {
    const auto pq = transfer == MFVideoTransFunc_2084;
    Check(SUCCEEDED(type->SetUINT32(MF_MT_TRANSFER_FUNCTION, transfer)) &&
          SUCCEEDED(type->SetUINT32(MF_MT_YUV_MATRIX, pq ? MFVideoTransferMatrix_BT2020_10 : MFVideoTransferMatrix_BT2020_12)),
          "Could not set MF HDR transfer and matrix");
    ReadMediaFoundationVideoColor(type.Get(), format);
    Check(format.primaries == ColorPrimaries::BT2020 && format.matrix == ColorMatrix::BT2020 &&
          format.transfer == (pq ? ColorTransfer::PQ : ColorTransfer::HLG) &&
          format.range == ColorRange::Limited && format.pixel_format == VideoPixelFormat::P010,
          "Media Foundation HDR output lost color signaling");
  }
}
#endif
template<class Predicate> void Eventually(Predicate predicate, const char* message) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) return;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  throw std::runtime_error(message);
}
struct Control {
  std::mutex mutex;
  std::condition_variable changed;
  bool block_open = false, open_entered = false, fail_poll = false, fail_close = false;
  bool block_close = false, close_entered = false;
  bool block_worker_exit = false, exit_entered = false;
  std::atomic<int> opened{0}, closed{0}, submitted{0}, cleaned{0}, wakeups{0};
  std::atomic<bool> cleanup_failed{false}, worker_exited{false}, cleanup_after_exit{false};
  std::atomic<unsigned> byte{0};
  std::uint32_t presentation_timescale = 1000000;
  std::weak_ptr<State> state;
};
#ifndef __EMSCRIPTEN__
struct WorkerExit {
  std::shared_ptr<Control> control;
  ~WorkerExit() {
    std::unique_lock lock(control->mutex);
    control->exit_entered = true;
    control->changed.notify_all();
    control->changed.wait(lock, [&] { return !control->block_worker_exit; });
    control->worker_exited = true;
  }
};
#endif
class Fake final : public DecoderBackend {
public:
  explicit Fake(std::shared_ptr<Control> control) : control_(std::move(control)) {}
  CodecResult<void> Open(const DecoderConfig& config) override {
#ifndef __EMSCRIPTEN__
    static thread_local WorkerExit worker_exit;
    worker_exit.control = control_;
#endif
    format_ = std::get<AudioDecoderConfig>(config).output;
    std::unique_lock lock(control_->mutex);
    control_->open_entered = true;
    control_->changed.notify_all();
    control_->changed.wait(lock, [&] { return !control_->block_open; });
    ++control_->opened;
    return CodecResult<void>::Success();
  }
  CodecResult<SubmitStatus> Submit(const EncodedPacket& packet) override {
    unsigned empty = 0;
    control_->byte.compare_exchange_strong(empty, std::to_integer<unsigned>(packet.data.AsBytes()[0]));
    pending_.push_back(packet.pts.Rescale(PresentationTimescale()).value());
    ++control_->submitted;
    return CodecResult<SubmitStatus>::Success(SubmitStatus::Accepted);
  }
  std::uint32_t PresentationTimescale() const noexcept override { return control_->presentation_timescale; }
  CodecResult<std::optional<BackendOutput>> Poll() override {
    using R = CodecResult<std::optional<BackendOutput>>;
    {
      std::lock_guard lock(control_->mutex);
      if (control_->fail_poll) return R::Failure(Error(CodecErrorCode::InvalidData, "Injected operational failure"));
    }
    if (!pending_.empty()) {
      AudioFrame frame{format_, pending_.front(), 1024, {Own(Bytes(1024 * format_.ChannelCount() * format_.BytesPerSample(), std::byte{42}))}};
      pending_.pop_front();
      return R::Success(BackendOutput{DecodedFrame{std::move(frame)}});
    }
    if (finished_ && !eos_) { eos_ = true; return R::Success(BackendOutput{EndOfStream{}}); }
    return R::Success(std::nullopt);
  }
  CodecResult<SubmitStatus> Finish() override { finished_ = true; return CodecResult<SubmitStatus>::Success(SubmitStatus::Accepted); }
  CodecResult<void> Close() override {
    std::unique_lock lock(control_->mutex);
    control_->close_entered = true;
    control_->changed.notify_all();
    control_->changed.wait(lock, [&] { return !control_->block_close; });
    ++control_->closed;
    pending_.clear();
    if (control_->fail_close) return CodecResult<void>::Failure(Error(CodecErrorCode::PlatformFailure, "Injected cleanup failure"));
    return CodecResult<void>::Success();
  }
private:
  std::shared_ptr<Control> control_;
  AudioFormat format_;
  std::deque<MediaTimestamp> pending_;
  bool finished_ = false, eos_ = false;
};
AudioDecoderConfig Config() {
  AudioDecoderConfig config;
  config.initialization_data = {std::byte{0x11}, std::byte{0x90}};
  config.output.sample_rate = 48000;
  config.limits = {2, 2, 4, 32768};
  return config;
}
std::shared_ptr<State> Create(const std::shared_ptr<Control>& control, bool close_in_wakeup = false) {
  auto config = Config();
  config.wakeup = [control, close_in_wakeup] {
    ++control->wakeups;
    if (close_in_wakeup) { if (auto state = control->state.lock()) state->Close(); }
  };
  config.cleanup_completed = [control](CodecResult<void> result) {
    control->cleanup_failed = !result.Succeeded();
    control->cleanup_after_exit = control->worker_exited.load();
    ++control->cleaned;
  };
  auto created = State::Create(config, [control] { return std::make_unique<Fake>(control); });
  Check(created.Succeeded(), "Fake session creation failed");
  return std::move(created).Value();
}
EncodedPacket Packet(int position, unsigned byte = 5) {
  return {Own(Bytes{std::byte(byte)}), {position, 48000}, {}, {}, true};
}
void Ready(const std::shared_ptr<Control>& control) {
  Eventually([&] { return control->wakeups > 0; }, "Session did not initialize");
}
AudioFrame ReadFrame(const std::shared_ptr<State>& state) {
  std::optional<AudioFrame> frame;
  Eventually([&] {
    auto read = state->Read();
    Check(read.Succeeded(), "Decoding failed before producing a frame");
    if (read.Value()) {
      if (auto* output = std::get_if<AudioFrame>(&*read.Value())) frame = std::move(*output);
    }
    return frame.has_value();
  }, "Decoder did not produce a frame");
  return std::move(*frame);
}
void AllocationFailureReturnsCredit() {
  auto control = std::make_shared<Control>();
  auto state = Create(control); Ready(control);
  auto packet = Packet(0);
  packet.data = Own(Bytes(Config().limits.input_bytes, std::byte{5}));
  for (int count : {0, 1}) {
    allocations_until_failure = count;
    auto result = state->Submit(packet);
    allocations_until_failure = -1;
    Check(!result.Succeeded() && result.Error().code == CodecErrorCode::ResourceExhausted,
          "Input allocation failure did not return ResourceExhausted");
  }
  auto result = state->Submit(packet);
  Check(result.Succeeded() && result.Value() == SubmitStatus::Accepted, "Allocation failure leaked input credit");
  Check(ReadFrame(state).Validate(), "Decoder did not recover after the allocation failure");
  state->Close();
  Eventually([&] { return control->cleaned == 1; }, "Cleanup did not complete after allocation failure");
}
void PlatformTimestampRounding() {
  auto control = std::make_shared<Control>();
  control->presentation_timescale = 10000000;
  auto state = Create(control); Ready(control);
  auto packet = Packet(0);
  packet.pts = {12288, 44100};
  auto result = state->Submit(packet);
  Check(result.Succeeded() && result.Value() == SubmitStatus::Accepted, "Timestamp test input not accepted");
  const auto frame = ReadFrame(state);
  Check(frame.pts.ticks == packet.pts.ticks && frame.pts.timescale == packet.pts.timescale,
        "Media Foundation 100 ns output did not preserve the original PTS");
  state->Close();
  Eventually([&] { return control->cleaned == 1; }, "Timestamp test did not clean up");
}
#ifndef __EMSCRIPTEN__
void IndependentWorkerCleanup() {
  auto blocked = std::make_shared<Control>();
  blocked->block_close = blocked->block_worker_exit = true;
  struct ReleaseGates {
    Control& control;
    ~ReleaseGates() {
      std::lock_guard lock(control.mutex);
      control.block_close = control.block_worker_exit = false;
      control.changed.notify_all();
    }
  } release{*blocked};
  auto first = Create(blocked); Ready(blocked);
  first->Close();
  {
    std::unique_lock lock(blocked->mutex);
    Check(blocked->changed.wait_for(lock, std::chrono::seconds(3), [&] { return blocked->close_entered; }),
          "First session did not start platform cleanup");
  }

  auto independent = std::make_shared<Control>();
  auto second = Create(independent); Ready(independent);
  auto result = second->Submit(Packet(0));
  Check(result.Succeeded() && result.Value() == SubmitStatus::Accepted, "Second session could not accept input");
  Check(ReadFrame(second).Validate(), "Second session could not decode during platform cleanup");
  auto probed = std::make_shared<std::atomic<bool>>(false);
  AudioDecoder::Probe({}, [probed](CodecResult<void>) { *probed = true; });
  Eventually([&] { return probed->load(); }, "Probe waited for unrelated platform cleanup");
  Check(blocked->cleaned == 0, "Cleanup completed during a blocked platform Close");

  const auto close_second = [&] {
    second->Close();
    Eventually([&] { return independent->cleaned == 1; }, "Second session's cleanup waited for an unrelated worker");
    Check(independent->cleanup_after_exit, "Cleanup was reported before the worker exited");
  };
#ifdef _WIN32
  // Windows serializes TLS destructors under its loader lock. Check independent release before entering TLS teardown.
  close_second();
#endif

  {
    std::unique_lock lock(blocked->mutex);
    blocked->block_close = false;
    blocked->changed.notify_all();
    Check(blocked->changed.wait_for(lock, std::chrono::seconds(3), [&] { return blocked->exit_entered; }),
          "Closed session did not begin releasing worker resources");
  }
#ifndef _WIN32
  close_second();
#endif
  Check(blocked->cleaned == 0, "Cleanup completed during a blocked worker exit");
  {
    std::lock_guard lock(blocked->mutex);
    blocked->block_worker_exit = false;
    blocked->changed.notify_all();
  }
  Eventually([&] { return blocked->cleaned == 1; }, "First session did not clean up after its worker exited");
  Check(blocked->cleanup_after_exit, "First session reported cleanup before worker exit");
}
#endif
void RetainedBudgetAndCopy() {
  auto control = std::make_shared<Control>();
  auto state = Create(control); Ready(control);
  auto original = std::make_shared<Bytes>(1, std::byte{7});
  auto packet = Packet(0); packet.data = BufferReference(*original, original);
  auto accepted = state->Submit(packet);
  Check(accepted.Succeeded() && accepted.Value() == SubmitStatus::Accepted, "Packet not accepted");
  (*original)[0] = std::byte{9};
  Check(state->Submit(Packet(1024)).Succeeded(), "Second submission failed");
  std::vector<AudioFrame> held;
  int submitted = 2;
  Eventually([&] {
    if (submitted < 4) {
      const auto accepted = state->Submit(Packet(submitted * 1024));
      Check(accepted.Succeeded(), "Cannot fill output pool");
      if (accepted.Value() == SubmitStatus::Accepted) ++submitted;
    }
    auto result = state->Read();
    Check(result.Succeeded(), "Read failed");
    if (result.Value()) if (auto* frame = std::get_if<AudioFrame>(&*result.Value())) held.push_back(*frame);
    return held.size() == 4;
  }, "Did not produce frames");
  Check(control->submitted == 4, "Unexpected platform input count");
  Check(control->byte == 7, "Accepted input did not preserve its copied bytes");
  { std::lock_guard lock(control->mutex); control->block_close = true; }
  Check(state->Reset().Succeeded(), "Reset failed");
  {
    std::unique_lock lock(control->mutex);
    Check(control->changed.wait_for(lock, std::chrono::seconds(3), [&] { return control->close_entered; }), "Reset did not begin cleanup");
  }
  Check(state->Reset().Succeeded(), "Pending Reset did not coalesce");
  auto empty = state->Read();
  Check(empty.Succeeded() && !empty.Value(), "Old output crossed the reset barrier");
  { std::lock_guard lock(control->mutex); control->block_close = false; control->changed.notify_all(); }
  Eventually([&] { return control->opened == 2; }, "Platform reset did not complete");
  Eventually([&] {
    auto result = state->Submit(Packet(0));
    Check(result.Succeeded(), "Post-reset submission failed");
    return result.Value() == SubmitStatus::Accepted;
  }, "Reset did not reopen input");
  Check(state->Submit(Packet(1024)).Succeeded(), "Post-reset queue failed");
  auto full = state->Submit(Packet(2048));
  Check(full.Succeeded() && full.Value() == SubmitStatus::WouldBlock, "Input queue ignored held output credit");
  Check(control->submitted == 4, "Reset replenished retained output budget");
  held.pop_back();
  Eventually([&] { return control->submitted == 5; }, "Releasing output did not restore credit");
  state->Close();
  Eventually([&] { return control->cleaned == 1; }, "Close waited for retained frames");
  Check(held.front().Validate() && held.front().planes[0].AsBytes()[0] == std::byte{42}, "Close invalidated output bytes");
  Check(!state->Read().Succeeded() && !state->Finish().Succeeded() && !state->Reset().Succeeded(), "Closed state accepted operations");
  state->Close(); Check(control->cleaned == 1, "Repeated cleanup callback");
}
void StartingAndCallbackClose() {
  auto control = std::make_shared<Control>();
  control->block_open = true;
  auto state = Create(control);
  {
    std::unique_lock lock(control->mutex);
    Check(control->changed.wait_for(lock, std::chrono::seconds(3), [&] { return control->open_entered; }), "Open did not enter");
  }
  Check(state->Submit(Packet(0)).Value() == SubmitStatus::WouldBlock, "Starting accepted input");
  Check(!state->Reset().Succeeded() && !state->Finish().Succeeded(), "Starting accepted a control command");
  state->Close();
  Check(!state->Read().Succeeded() && control->cleaned == 0, "Logical close waited for/opened cleanup prematurely");
  { std::lock_guard lock(control->mutex); control->block_open = false; control->changed.notify_all(); }
  Eventually([&] { return control->cleaned == 1; }, "Canceled initialization did not clean up");
  Check(control->wakeups == 0 && control->closed == 1, "Late initialization resurrected session");

  auto callback = std::make_shared<Control>();
  callback->block_open = true;
  auto next = Create(callback, true);
  callback->state = next;
  { std::lock_guard lock(callback->mutex); callback->block_open = false; callback->changed.notify_all(); }
  Eventually([&] { return callback->cleaned == 1; }, "Close in wakeup deadlocked");
  Check(callback->wakeups == 1 && callback->closed == 1, "Close in wakeup delivered extra callbacks");
}
void CleanupFailureAndPending() {
  auto control = std::make_shared<Control>();
  control->block_close = true; control->fail_close = true;
  auto state = Create(control); Ready(control);
  { std::lock_guard lock(control->mutex); control->fail_poll = true; }
  Check(state->Submit(Packet(0)).Succeeded(), "Cannot trigger processing failure");
  {
    std::unique_lock lock(control->mutex);
    Check(control->changed.wait_for(lock, std::chrono::seconds(3), [&] { return control->close_entered; }), "Fatal failure did not start cleanup");
  }
  auto failed = state->Read();
  Check(!failed.Succeeded() && failed.Error().code == CodecErrorCode::InvalidData, "Operational error was lost");
  Check(control->cleaned == 0, "Stuck platform release was reported complete");
  state->Close();
  { std::lock_guard lock(control->mutex); control->block_close = false; control->changed.notify_all(); }
  Eventually([&] { return control->cleaned == 1; }, "Cleanup failure not delivered");
  Check(control->cleanup_failed, "Release failure was disguised as successful cleanup");
}
void ConcurrentBarriers() {
  auto control = std::make_shared<Control>();
  auto state = Create(control); Ready(control);
  std::atomic<bool> stop{false}, unexpected{false};
  std::atomic<int> accepted{0};
  std::thread producer([&] {
    int position = 0;
    while (!stop) {
      auto result = state->Submit(Packet(position));
      if (!result.Succeeded() && result.Error().code != CodecErrorCode::Closed) unexpected = true;
      if (result.Succeeded() && result.Value() == SubmitStatus::Accepted) { ++accepted; position += 1024; }
      else std::this_thread::yield();
    }
  });
  std::thread consumer([&] {
    while (!stop) {
      auto result = state->Read();
      if (!result.Succeeded() && result.Error().code != CodecErrorCode::Closed) unexpected = true;
      std::this_thread::yield();
    }
  });
  for (unsigned i = 0; i < 100; ++i) {
    auto result = state->Reset();
    if (!result.Succeeded()) unexpected = true;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  state->Close(); stop = true;
  producer.join(); consumer.join();
  Eventually([&] { return control->cleaned == 1; }, "Concurrent barrier cleanup did not complete");
  Check(!unexpected && accepted > 0, "Concurrent queue/control calls violated lifecycle semantics");
}
void Values() {
  const auto maximum = std::numeric_limits<std::int64_t>::max();
  const auto minimum = std::numeric_limits<std::int64_t>::min();
  Check(MediaTimestamp{-1, 2}.Rescale(1)->ticks == -1 && MediaTimestamp{1, 2}.Rescale(1)->ticks == 1, "Rounding tie rule failed");
  Check(MediaTimestamp{minimum, 1}.Rescale(1)->ticks == minimum, "Minimum timestamp overflow");
  Check(!MediaTimestamp{maximum, 1}.Rescale(2), "Overflow was accepted");
  Check(MediaTimestamp{1, 2} == MediaTimestamp{2, 4}, "Timestamp equality ignored rational values");
  Check(!MediaTimestamp{1, 0}.Compare({0, 1}), "Zero timescale accepted");
  Check(MediaTimestamp{maximum, 3}.Compare({maximum - 1, 3}) == 1, "Exact comparison lost precision");
  auto config = Config();
  config.limits.output_bytes = 1;
  Check(!ValidateConfig(config).Succeeded(), "Impossible output budget accepted");
  VideoFormat format{3, 3, {0, 0, 3, 3}};
  VideoFrame frame{format, {}, {}, {{Own(Bytes(9)), 3, 1}, {Own(Bytes(8)), 4, 2}}};
  Check(frame.Validate(), "Odd-sized NV12 rejected");
  frame.planes[1].row_stride = std::numeric_limits<std::size_t>::max();
  Check(!frame.Validate(), "Plane address overflow accepted");
  AudioFrame audio{Config().output, {}, 1024, {Own(Bytes(8191))}};
  Check(!audio.Validate(), "Truncated PCM accepted");
  auto malformed = ParseAvc(Bytes{std::byte{1}});
  Check(!malformed.Succeeded(), "Malformed avcC accepted");
}

void VpxPackets() {
  const auto validate = [](CodecId codec, Bytes bytes, bool needs_key = false, bool key = false) {
    const EncodedPacket packet{Own(std::move(bytes)), {0, 1}, {}, {}, key};
    return ValidatePacket(packet, VideoDecoderConfig{.codec = codec}, needs_key);
  };
  const Bytes vp8_key{std::byte{0x10}, {}, {}, std::byte{0x9d}, std::byte{1}, std::byte{0x2a},
                     std::byte{64}, {}, std::byte{48}, {}};
  Check(validate(CodecId::Vp8, vp8_key, true, true).Value() &&
        !validate(CodecId::Vp8, vp8_key, true).Succeeded(), "VP8 startup did not require a marked key frame");
  Check(!validate(CodecId::Vp8, {std::byte{1}, {}, {}}).Value() &&
        !validate(CodecId::Vp8, {std::byte{1}, {}, {}}, true, true).Succeeded(),
        "VP8 reference-only frames requested output or were accepted as a key frame");
  const Bytes vp9_key{std::byte{0x82}, std::byte{0x49}, std::byte{0x83}, std::byte{0x42}};
  Check(validate(CodecId::Vp9, vp9_key, true, true).Value() &&
        !validate(CodecId::Vp9, vp9_key, true).Succeeded(), "VP9 startup did not require a marked key frame");
  Check(!validate(CodecId::Vp9, {std::byte{0x84}}).Value() &&
        validate(CodecId::Vp9, {std::byte{0x88}}).Value(),
        "VP9 reference-only and show-existing frames reported the wrong output count");
  const auto marker = std::byte{0xc1};
  Bytes superframe{std::byte{0x84}, std::byte{0x86}, marker, std::byte{1}, std::byte{1}, marker};
  Check(validate(CodecId::Vp9, superframe).Value(), "VP9 superframe lost its visible frame");
  superframe[0] = std::byte{0x86};
  Check(!validate(CodecId::Vp9, superframe).Succeeded(), "VP9 superframe accepted two visible frames");
  superframe[4] = std::byte{4};
  Check(!validate(CodecId::Vp9, superframe).Succeeded(), "VP9 superframe accepted an out-of-range index");
  const Bytes features{std::byte{1}, std::byte{1}, {}, std::byte{2}, std::byte{1}, std::byte{31}};
  Check(Vp9Level(features).Value() == 31 && !Vp9Level(std::span(features).first(5)).Succeeded(),
        "VP9 codec feature validation lost the level or accepted truncation");
}
} // namespace
int main() {
  try {
#ifdef _WIN32
    WindowsVideoColor();
#endif
    Values(); VpxPackets(); AllocationFailureReturnsCredit(); PlatformTimestampRounding();
#ifndef __EMSCRIPTEN__
    IndependentWorkerCleanup();
#endif
    RetainedBudgetAndCopy(); StartingAndCallbackClose(); CleanupFailureAndPending(); ConcurrentBarriers();
    std::cout << "Codec value, budget, generation, callback, and cleanup contracts passed\n";
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
