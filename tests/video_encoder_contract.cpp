#include "codec/encoder/session.h"
#include "codec/decoder/format.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <thread>

using namespace huxerui;
using namespace huxerui::media;
using namespace huxerui::media::detail;
namespace {
void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class F> void Eventually(F work, const char* message) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!work()) {
    if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error(message);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}
struct Bits {
  std::vector<bool> bits;
  void Write(unsigned value, unsigned count) { for (unsigned i = count; i; --i) bits.push_back((value >> (i - 1)) & 1); }
  void Ue(unsigned value) {
    const auto code = value + 1; unsigned width = 0;
    for (auto n = code; n; n >>= 1) ++width;
    Write(0, width - 1); Write(code, width);
  }
  Bytes Nal(unsigned header) {
    Write(1, 1); while (bits.size() % 8) Write(0, 1);
    Bytes result{std::byte(header)}; unsigned zeros = 0;
    for (std::size_t i = 0; i < bits.size(); i += 8) {
      unsigned value = 0;
      for (unsigned j = 0; j < 8; ++j) value = (value << 1) | bits[i + j];
      if (zeros >= 2 && value <= 3) { result.push_back(std::byte{3}); zeros = 0; }
      result.push_back(std::byte(value)); zeros = value == 0 ? zeros + 1 : 0;
    }
    return result;
  }
};
VideoEncoderConfig Config() {
  VideoEncoderConfig config;
  config.input = {.width = 16, .height = 16, .visible = {0, 0, 16, 16}, .pixel_format = VideoPixelFormat::NV12};
  config.profile = H264Profile::Main; config.max_b_frames = 2; config.max_packet_bytes = 1024;
  config.limits = {4, 4 * 384, 4, 4 * 1024};
  return config;
}
VideoEncodeFormat Format(const VideoEncoderConfig& config, bool allow_gaps = false) {
  Bits bits;
  bits.Write(config.profile == H264Profile::Baseline ? 66 : config.profile == H264Profile::Main ? 77 : 100, 8);
  bits.Write(0, 8); bits.Write(31, 8); bits.Ue(0);
  if (config.profile == H264Profile::High) { bits.Ue(1); bits.Ue(0); bits.Ue(0); bits.Write(0, 2); }
  bits.Ue(0); bits.Ue(0); bits.Ue(0); bits.Ue(2); bits.Write(allow_gaps, 1);
  bits.Ue(0); bits.Ue(0); bits.Write(1, 1); bits.Write(1, 1); bits.Write(0, 1); bits.Write(0, 1);
  const auto sps = bits.Nal(0x67);
  Bytes data{std::byte{0}, std::byte{0}, std::byte{0}, std::byte{1}};
  data.insert(data.end(), sps.begin(), sps.end());
  data.insert(data.end(), {std::byte{0}, std::byte{0}, std::byte{0}, std::byte{1}, std::byte{0x68}, std::byte{0xce}, std::byte{0x3c}, std::byte{0x80}});
  auto result = ReadVideoEncodeFormat(data, config);
  Check(result.Succeeded(), "Cannot create test H.264 parameter sets");
  return std::move(result).Value();
}
VideoFrame Frame(const VideoEncoderConfig& config, unsigned index) {
  auto bytes = std::make_shared<Bytes>(18 * 24, std::byte{91});
  BufferReference owner(*bytes, bytes);
  return {config.input, {30000 + std::int64_t(index) * 1001, 30000}, MediaTimestamp{1001, 30000},
      {{owner.Slice(0, 18 * 16), 18, 1}, {owner.Slice(18 * 16, 18 * 8), 18, 2}}};
}
struct Control {
  std::mutex mutex; std::condition_variable changed;
  bool block_open = false, entered_open = false, block_exit = false, entered_exit = false;
  std::atomic<bool> fail_close{false}, wrong_pts{false}, oversized{false}, refuse_once{false};
  std::atomic<bool> exited{false}, cleanup_after_exit{false}, cleanup_ok{false};
  std::atomic<unsigned> opened{0}, submitted{0}, closed{0}, cleaned{0}, requests{0}, wakes{0}, flushes{0};
};
struct Unblock {
  std::shared_ptr<Control> control;
  ~Unblock() {
    std::lock_guard lock(control->mutex);
    control->block_open = control->block_exit = false; control->changed.notify_all();
  }
};
struct Exit {
  std::shared_ptr<Control> control;
  ~Exit() {
    if (!control) return;
    std::unique_lock lock(control->mutex);
    control->entered_exit = true; control->changed.notify_all();
    control->changed.wait(lock, [&] { return !control->block_exit; }); control->exited = true;
  }
};
class Fake final : public VideoEncoderBackend {
public:
  explicit Fake(std::shared_ptr<Control> control) : control_(std::move(control)) {}
  CodecResult<void> Open(const VideoEncoderConfig& config) override {
    static thread_local Exit exit;
    exit.control = control_; config_ = config;
    std::unique_lock lock(control_->mutex);
    control_->entered_open = true; control_->changed.notify_all();
    control_->changed.wait(lock, [&] { return !control_->block_open; }); ++control_->opened;
    return CodecResult<void>::Success();
  }
  CodecResult<SubmitStatus> Submit(const VideoFrame& frame) override {
    if (control_->refuse_once && !control_->flushes) return CodecResult<SubmitStatus>::Success(SubmitStatus::WouldBlock);
    Check(frame.Validate() && frame.planes[0].row_stride == 16 && frame.planes[0].data.AsBytes()[0] == std::byte{91},
        "Input copy or stride normalization failed");
    pending_.push_back(frame.pts.ticks); ++control_->submitted;
    return CodecResult<SubmitStatus>::Success(SubmitStatus::Accepted);
  }
  CodecResult<std::optional<VideoEncoderBackendOutput>> Poll() override {
    using R = CodecResult<std::optional<VideoEncoderBackendOutput>>;
    if (!described_) { described_ = true; return R::Success(Format(config_)); }
    if (!ready_.empty()) {
      auto [pts, b] = ready_.front(); ready_.pop_front(); return Packet(pts, b, false);
    }
    if (!first_ && !pending_.empty()) {
      first_ = true; const auto pts = pending_.front(); pending_.pop_front(); return Packet(pts, false, true);
    }
    if (pending_.size() >= 3 || (finished_ && !pending_.empty())) {
      const auto count = std::min<std::size_t>(3, pending_.size());
      const auto anchor = pending_[count - 1];
      for (std::size_t i = 0; i + 1 < count; ++i) ready_.emplace_back(pending_[i], true);
      for (std::size_t i = 0; i < count; ++i) pending_.pop_front();
      return Packet(anchor, false, false);
    }
    if (finished_ && !eos_) { eos_ = true; return R::Success(VideoEncoderEnd{}); }
    return R::Success(std::nullopt);
  }
  CodecResult<void> RequestKeyFrame() override { ++control_->requests; return CodecResult<void>::Success(); }
  CodecResult<void> FlushPending() override { ++control_->flushes; return CodecResult<void>::Success(); }
  CodecResult<SubmitStatus> Finish() override { finished_ = true; return CodecResult<SubmitStatus>::Success(SubmitStatus::Accepted); }
  CodecResult<void> Close() override {
    ++control_->closed;
    return control_->fail_close ? CodecResult<void>::Failure(Error(CodecErrorCode::PlatformFailure, "Injected video cleanup failure")) : CodecResult<void>::Success();
  }
private:
  CodecResult<std::optional<VideoEncoderBackendOutput>> Packet(std::int64_t pts, bool b, bool key) {
    Bits bits; bits.Ue(0); bits.Ue(key ? 2 : b ? 1 : 0); bits.Ue(0);
    const auto nal = bits.Nal(key ? 0x65 : 0x41);
    Bytes data{std::byte{0}, std::byte{0}, std::byte{0}, std::byte(nal.size())};
    data.insert(data.end(), nal.begin(), nal.end());
    if (control_->oversized) data.resize(config_.max_packet_bytes + 1);
    return CodecResult<std::optional<VideoEncoderBackendOutput>>::Success(EncodedPacket{Own(std::move(data)),
        {pts + (control_->wrong_pts ? 17 : 0), 1000000}, MediaTimestamp{-66733 + std::int64_t(emitted_++) * 33367, 1000000},
        std::nullopt, false});
  }
  std::shared_ptr<Control> control_;
  VideoEncoderConfig config_;
  std::deque<std::int64_t> pending_;
  std::deque<std::pair<std::int64_t, bool>> ready_;
  unsigned emitted_ = 0;
  bool described_ = false, first_ = false, finished_ = false, eos_ = false;
};
std::shared_ptr<VideoEncoderState> Create(VideoEncoderConfig config, const std::shared_ptr<Control>& control) {
  config.wakeup = [control] { ++control->wakes; };
  config.cleanup_completed = [control](auto result) {
    control->cleanup_after_exit = control->exited.load(); control->cleanup_ok = result.Succeeded(); ++control->cleaned;
  };
  auto created = VideoEncoderState::Create(config, [control] { return std::make_unique<Fake>(control); });
  Check(created.Succeeded(), "Cannot create fake video encoder");
  return std::move(created).Value();
}
void Ready(const std::shared_ptr<VideoEncoderState>& state, const std::shared_ptr<Control>& control, unsigned opens = 1) {
  Eventually([&] { auto read = state->Read(); Check(read.Succeeded(), "Video startup failed"); return control->opened >= opens && control->wakes > 0; },
      "Video startup stalled");
}
void Submit(const std::shared_ptr<VideoEncoderState>& state, const VideoFrame& frame) {
  Eventually([&] { auto value = state->Submit(frame); Check(value.Succeeded(), "Valid video was rejected"); return value.Value() == SubmitStatus::Accepted; },
      "Video submit stalled");
}
void Close(const std::shared_ptr<VideoEncoderState>& state, const std::shared_ptr<Control>& control) {
  state->Close(); state->Close();
  Eventually([&] { return control->cleaned == 1; }, "Video cleanup stalled");
  Check(control->cleanup_after_exit, "Video cleanup preceded thread-local destruction");
}
void ReorderingAndTiming() {
  auto config = Config(); auto control = std::make_shared<Control>(); control->refuse_once = true;
  auto state = Create(config, control); Ready(state, control);
  auto invalid = Frame(config, 0); invalid.format.width = 0;
  Check(!state->Submit(invalid).Succeeded(), "Invalid video accepted");
  for (unsigned i = 0; i < 4; ++i) {
    auto frame = Frame(config, i); Submit(state, frame);
    const auto pixels = frame.planes[0].data.AsBytes();
    std::fill(const_cast<std::byte*>(pixels.data()), const_cast<std::byte*>(pixels.data()) + pixels.size(), std::byte{0});
  }
  Check(!state->Submit(Frame(config, 3)).Succeeded(), "Duplicate PTS was accepted");
  Check(state->RequestKeyFrame().Succeeded() && state->Finish().Succeeded(), "Video controls failed");
  const unsigned order[]{0, 3, 1, 2}; unsigned pictures = 0; bool ended = false;
  Eventually([&] {
    auto value = state->Read(); Check(value.Succeeded(), "Reordered video failed");
    if (value.Value()) {
      if (auto* packet = std::get_if<EncodedPacket>(&*value.Value())) {
        Check(pictures < 4 && packet->pts == Frame(config, order[pictures]).pts && packet->duration == MediaTimestamp{1001, 30000},
            "Output metadata was matched by FIFO instead of presentation identity");
        Check(packet->dts == MediaTimestamp{1000000 - 66733 + std::int64_t(pictures) * 33367, 1000000}, "Reported DTS was fabricated or shifted incorrectly");
        Check(packet->key_frame == (pictures == 0), "Random access was not derived from IDR syntax"); ++pictures;
      } else if (std::holds_alternative<EndOfStream>(*value.Value())) ended = true;
    }
    return ended;
  }, "Reordered video drain stalled");
  Check(pictures == 4 && control->submitted == 4 && control->requests == 1 && control->flushes > 0, "Wrong video input/output/control count");
  Close(state, control); Check(control->cleanup_ok, "Video cleanup failed");
}
void RetentionAndReset() {
  auto config = Config(); auto control = std::make_shared<Control>(); auto state = Create(config, control); Ready(state, control);
  for (unsigned i = 0; i < 8; ++i) Submit(state, Frame(config, i));
  std::vector<BufferReference> held;
  Eventually([&] {
    auto value = state->Read(); Check(value.Succeeded(), "Video retention failed");
    if (value.Value()) if (auto* packet = std::get_if<EncodedPacket>(&*value.Value())) held.push_back(packet->data.Slice(0, 1));
    return held.size() == 4;
  }, "Video output window was not filled");
  const auto submitted = control->submitted.load();
  Check(submitted == 4 && state->Reset().Succeeded(), "Video output was not bounded");
  Ready(state, control, 2); Submit(state, Frame(config, 0));
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  Check(control->submitted == submitted, "Video reset replenished retained output credit");
  held.erase(held.begin());
  Eventually([&] { return control->submitted > submitted; }, "Video slice release did not wake producer");
  const auto byte = held.front().AsBytes()[0]; Close(state, control);
  Check(held.front().AsBytes()[0] == byte, "Video slice changed during cleanup");
}
void StartupAndFailure() {
  auto config = Config(); auto blocked = std::make_shared<Control>(); blocked->block_open = true; Unblock unblock{blocked};
  auto first = Create(config, blocked);
  Eventually([&] { std::lock_guard lock(blocked->mutex); return blocked->entered_open; }, "Blocked video initialization did not begin");
  Check(first->Submit(Frame(config, 0)).Value() == SubmitStatus::WouldBlock, "Starting video accepted input");
  auto other_control = std::make_shared<Control>(); auto other = Create(config, other_control); Ready(other, other_control); Close(other, other_control);
  first->Close(); Check(first->Read().Error().code == CodecErrorCode::Closed, "Video close barrier was delayed");
  { std::lock_guard lock(blocked->mutex); blocked->block_open = false; blocked->changed.notify_all(); }
  Eventually([&] { return blocked->cleaned == 1; }, "Retired video initialization did not clean up");
  Check(blocked->wakes == 0, "Retired initialization admitted a video wakeup");
  for (bool wrong_pts : {false, true}) {
    auto control = std::make_shared<Control>(); Unblock release{control};
    auto state = Create(config, control); Ready(state, control);
    { std::lock_guard lock(control->mutex); control->block_exit = true; }
    control->fail_close = true; control->wrong_pts = wrong_pts; control->oversized = !wrong_pts;
    Submit(state, Frame(config, 0));
    Eventually([&] {
      auto value = state->Read();
      return !value.Succeeded() && value.Error().code == (wrong_pts ? CodecErrorCode::InvalidData : CodecErrorCode::ResourceExhausted);
    }, "Invalid video output was accepted");
    Eventually([&] { std::lock_guard lock(control->mutex); return control->entered_exit; }, "Video worker did not exit");
    Check(control->cleaned == 0 && !state->Reset().Succeeded(), "Video cleanup raced thread exit or allowed failed reset");
    { std::lock_guard lock(control->mutex); control->block_exit = false; control->changed.notify_all(); }
    Eventually([&] { return control->cleaned == 1; }, "Video failure cleanup stalled");
    Check(!control->cleanup_ok && control->cleanup_after_exit, "Video operational/cleanup failure contract changed");
    state->Close();
  }
}
void WakeupClose() {
  auto config = Config(); auto control = std::make_shared<Control>(); control->block_open = true; Unblock unblock{control};
  auto slot = std::make_shared<std::weak_ptr<VideoEncoderState>>();
  config.wakeup = [slot] { if (auto state = slot->lock()) state->Close(); };
  config.cleanup_completed = [control](auto result) { control->cleanup_ok = result.Succeeded(); ++control->cleaned; };
  auto created = VideoEncoderState::Create(config, [control] { return std::make_unique<Fake>(control); });
  Check(created.Succeeded(), "Cannot create callback-close video encoder");
  auto state = std::move(created).Value(); *slot = state;
  { std::lock_guard lock(control->mutex); control->block_open = false; control->changed.notify_all(); }
  Eventually([&] { return control->cleaned == 1; }, "Close from video wakeup deadlocked");
  Check(control->cleanup_ok, "Callback-close video cleanup failed");
}
void Validation() {
  auto config = Config(); Check(ValidateVideoEncoderConfig(config).Value() == 384, "Packed video accounting changed");
  config.profile = H264Profile::Baseline;
  Check(ValidateVideoEncoderConfig(config).Error().code == CodecErrorCode::InvalidArgument, "Baseline accepted B frames");
  config = Config(); config.limits.output_items = 3;
  Check(!ValidateVideoEncoderConfig(config).Succeeded(), "Impossible video reorder window accepted");
  config = Config(); config.input.height = 15; config.input.visible.height = 15;
  Check(!ValidateVideoEncoderConfig(config).Succeeded(), "Odd video geometry accepted");
  Bytes prefixed{std::byte{0}, std::byte{0}, std::byte{0}, std::byte{5}, std::byte{0x6e},
      std::byte{0xc0}, std::byte{0x80}, std::byte{0x0f}, std::byte{0x20},
      std::byte{0}, std::byte{0}, std::byte{0}, std::byte{2}, std::byte{0x65}, std::byte{0xb8}};
  auto normalized = NormalizeVideoEncoderPacket(prefixed, 4, 1024);
  Check(normalized.Succeeded() && normalized.Value().size() == 6 && normalized.Value()[4] == std::byte{0x65},
      "Optional base-layer prefix changed the AVC picture");
  prefixed[6] = std::byte{0x90};
  Check(!NormalizeVideoEncoderPacket(prefixed, 4, 1024).Succeeded(), "An extension-layer prefix was silently stripped");
  for (auto profile : {H264Profile::Baseline, H264Profile::Main, H264Profile::High}) {
    config = Config(); config.profile = profile; if (profile == H264Profile::Baseline) config.max_b_frames = 0;
    const auto format = Format(config); Check(format.width == 16 && format.height == 16, "SPS geometry changed");
    auto bad = format.initialization_data; bad[1] = std::byte{244};
    Check(!ReadVideoEncodeFormat(bad, config).Succeeded(), "Unsupported H.264 profile accepted");
    const auto declared = Format(config, true);
    VideoDecoderConfig decoder;
    decoder.width = declared.width; decoder.height = declared.height;
    decoder.initialization_data = declared.initialization_data;
    Check(ValidateConfig(decoder).Succeeded(), "A gaps-allowed declaration rejected a continuous stream");
  }
}
}
int main() {
  try {
    Validation(); ReorderingAndTiming(); RetentionAndReset(); StartupAndFailure(); WakeupClose();
    std::cout << "H.264 queue, reorder identity, DTS, retained storage, reset, isolation, and cleanup contracts passed\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
