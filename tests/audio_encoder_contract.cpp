#include "codec/encoder/session.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <iostream>
#include <thread>

using namespace huxerui;
using namespace huxerui::media;
using namespace huxerui::media::detail;
namespace {
void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class F> void Eventually(F work, const char* message) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
  while (!work()) {
    if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error(message);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}
struct Control {
  std::mutex mutex;
  std::condition_variable changed;
  bool block_open = false, entered_open = false, block_exit = false, entered_exit = false;
  std::atomic<bool> fail_poll{false}, fail_close{false}, invalid_packet{false}, exited{false}, cleanup_after_exit{false};
  std::atomic<int> opened{0}, submitted{0}, closed{0}, cleaned{0}, wakes{0};
  std::atomic<bool> cleanup_ok{false};
  std::vector<std::int16_t> pcm;
  std::uint32_t delay = 0;
};
struct Unblock {
  std::shared_ptr<Control> control;
  ~Unblock() {
    std::lock_guard lock(control->mutex);
    control->block_open = control->block_exit = false;
    control->changed.notify_all();
  }
};
struct WorkerExit {
  std::shared_ptr<Control> control;
  ~WorkerExit() {
    if (!control) return;
    std::unique_lock lock(control->mutex);
    control->entered_exit = true; control->changed.notify_all();
    control->changed.wait(lock, [&] { return !control->block_exit; });
    control->exited = true;
  }
};
class Fake final : public AudioEncoderBackend {
public:
  explicit Fake(std::shared_ptr<Control> control) : control_(std::move(control)) {}
  CodecResult<void> Open(const AudioEncoderConfig& config) override {
    static thread_local WorkerExit exit;
    exit.control = control_;
    config_ = config;
    std::unique_lock lock(control_->mutex);
    control_->entered_open = true; control_->changed.notify_all();
    control_->changed.wait(lock, [&] { return !control_->block_open; });
    ++control_->opened;
    return CodecResult<void>::Success();
  }
  CodecResult<SubmitStatus> Submit(std::span<const std::byte> pcm, std::uint32_t count, std::int64_t sample) override {
    Check(count == 1024 && pcm.size() == count * config_.input.ChannelCount() * 2, "Unexpected platform PCM block");
    {
      std::lock_guard lock(control_->mutex);
      const auto offset = control_->pcm.size(); control_->pcm.resize(offset + pcm.size() / 2);
      std::memcpy(control_->pcm.data() + offset, pcm.data(), pcm.size());
    }
    if (!started_) { pending_.push_back(-1024); started_ = true; }
    pending_.push_back(sample);
    ++control_->submitted;
    return CodecResult<SubmitStatus>::Success(SubmitStatus::Accepted);
  }
  CodecResult<std::optional<AudioEncoderBackendOutput>> Poll() override {
    using R = CodecResult<std::optional<AudioEncoderBackendOutput>>;
    if (control_->fail_poll) return R::Failure(Error(CodecErrorCode::InvalidData, "Injected encoder error"));
    if (!described_) { described_ = true; return R::Success(DescribeAac(config_)); }
    if (!pending_.empty() && (finished_ || pending_.size() > control_->delay)) {
      EncodedPacket packet{Own(Bytes(control_->invalid_packet ? 65537 : 17, std::byte{37})),
          {pending_.front(), config_.input.sample_rate}, std::nullopt, std::nullopt, true};
      pending_.pop_front();
      return R::Success(std::move(packet));
    }
    if (finished_ && !eos_) { eos_ = true; return R::Success(AudioEncoderEnd{1024, 0}); }
    return R::Success(std::nullopt);
  }
  CodecResult<SubmitStatus> Finish() override { finished_ = true; return CodecResult<SubmitStatus>::Success(SubmitStatus::Accepted); }
  CodecResult<void> Close() override {
    ++control_->closed;
    return control_->fail_close ? CodecResult<void>::Failure(Error(CodecErrorCode::PlatformFailure, "Injected cleanup error")) : CodecResult<void>::Success();
  }
private:
  std::shared_ptr<Control> control_;
  AudioEncoderConfig config_;
  std::deque<std::int64_t> pending_;
  bool started_ = false, described_ = false, finished_ = false, eos_ = false;
};
AudioEncoderConfig Config() {
  AudioEncoderConfig config;
  config.input = {48000, AudioSampleFormat::S16, AudioPacking::Interleaved, AudioChannelLayout::Mono};
  return config;
}
AudioFrame Frame(const AudioEncoderConfig& config, std::uint32_t count, std::int64_t pts, std::int16_t value = 73) {
  auto pcm = std::make_shared<std::vector<std::int16_t>>(std::size_t(count) * config.input.ChannelCount(), value);
  return {config.input, {pts, config.input.sample_rate}, count, {BufferReference(std::as_bytes(std::span(*pcm)), pcm)}};
}
std::shared_ptr<AudioEncoderState> Create(AudioEncoderConfig config, const std::shared_ptr<Control>& control) {
  config.wakeup = [control] { ++control->wakes; };
  config.cleanup_completed = [control](auto result) {
    control->cleanup_after_exit = control->exited.load(); control->cleanup_ok = result.Succeeded(); ++control->cleaned;
  };
  auto result = AudioEncoderState::Create(config, [control] { return std::make_unique<Fake>(control); });
  Check(result.Succeeded(), "Cannot create fake encoder");
  return std::move(result).Value();
}
void Ready(const std::shared_ptr<AudioEncoderState>& state, const std::shared_ptr<Control>& control, int opens = 1) {
  Eventually([&] { auto read = state->Read(); Check(read.Succeeded(), "Encoder startup failed"); return control->opened >= opens && control->wakes > 0; }, "Encoder did not become ready");
}
void Submit(const std::shared_ptr<AudioEncoderState>& state, const AudioFrame& frame) {
  Eventually([&] { auto result = state->Submit(frame); Check(result.Succeeded(), "Valid PCM was rejected"); return result.Value() == SubmitStatus::Accepted; }, "PCM submit stalled");
}
void Close(const std::shared_ptr<AudioEncoderState>& state, const std::shared_ptr<Control>& control) {
  state->Close(); state->Close();
  Eventually([&] { return control->cleaned.load() == 1; }, "Cleanup stalled");
  Check(control->cleanup_after_exit, "Cleanup preceded worker/TLS exit");
}
void TinyInputsAndTiming() {
  auto config = Config(); config.limits.input_items = 1; config.limits.input_bytes = 2;
  auto control = std::make_shared<Control>(); auto state = Create(config, control); Ready(state, control);
  auto invalid = Frame(config, 1, 0); invalid.sample_count = 0;
  Check(!state->Submit(invalid).Succeeded(), "Invalid PCM accepted");
  const auto first = Frame(config, 1, 48000);
  Submit(state, first);
  auto gap = Frame(config, 1, 48002);
  Check(!state->Submit(gap).Succeeded(), "Timestamp gap accepted");
  for (int i = 1; i < 1537; ++i) {
    auto frame = Frame(config, 1, 48000 + i);
    frame.pts = {(48000 + i) * 1000LL + 400, 48000000};
    Submit(state, frame);
  }
  Check(state->Finish().Succeeded(), "Finish failed");
  bool ended = false; int packets = 0, summaries = 0;
  Eventually([&] {
    auto read = state->Read(); Check(read.Succeeded(), "Tiny input encoding failed");
    if (read.Value()) {
      if (auto* packet = std::get_if<EncodedPacket>(&*read.Value())) {
        Check(packet->pts == MediaTimestamp{48000 - 1024 + packets * 1024, 48000}, "Sample anchor was not restored");
        Check(!packet->dts, "Missing platform DTS was invented"); ++packets;
      } else if (auto* summary = std::get_if<AudioEncodeSummary>(&*read.Value())) {
        Check(summary->input_samples == 1537 && summary->encoded_samples == 3072 && summary->priming_samples == 1024 && summary->padding_samples == 511,
            "Known trim or library tail fill is wrong"); ++summaries;
      } else if (std::holds_alternative<EndOfStream>(*read.Value())) ended = true;
    }
    return ended;
  }, "Single-slot tiny PCM inputs deadlocked");
  Check(packets == 3 && summaries == 1, "Wrong packet/summary count");
  {
    std::lock_guard lock(control->mutex);
    Check(control->pcm.size() == 2048, "Unbounded or incomplete PCM assembly");
    Check(std::all_of(control->pcm.begin(), control->pcm.begin() + 1537, [](auto value) { return value == 73; }), "PCM samples were changed");
    Check(std::all_of(control->pcm.begin() + 1537, control->pcm.end(), [](auto value) { return value == 0; }), "Tail fill was not silent");
  }
  Close(state, control); Check(control->cleanup_ok, "Cleanup failed");
}
void BackpressureAndRetainedSlices() {
  auto config = Config(); config.limits.output_items = 16; config.limits.output_bytes = 16 * 65536;
  auto control = std::make_shared<Control>(); auto state = Create(config, control); Ready(state, control);
  Submit(state, Frame(config, 24 * 1024, 0));
  std::vector<BufferReference> held;
  Eventually([&] {
    for (;;) {
      auto read = state->Read(); Check(read.Succeeded(), "Encoding failed while holding packets"); if (!read.Value()) break;
      if (auto* packet = std::get_if<EncodedPacket>(&*read.Value())) held.push_back(packet->data.Slice(0, 1));
    }
    return held.size() == 9;
  }, "Output pool did not reach its bounded window");
  Check(control->submitted == 8, "Held packet slices did not retain full item credits");
  Check(state->Reset().Succeeded(), "Reset failed");
  Ready(state, control, 2);
  Submit(state, Frame(config, 1024, 0));
  std::this_thread::sleep_for(std::chrono::milliseconds(15));
  Check(control->submitted == 8, "Reset replenished held output credit");
  held.erase(held.begin(), held.begin() + 2);
  Eventually([&] { return control->submitted > 8; }, "Released slice did not wake encoder");
  Close(state, control);
  Check(!held.empty() && held.front().AsBytes()[0] == std::byte{37}, "Slice changed after cleanup");
}
void StartupCloseAndIsolation() {
  auto config = Config(); auto control = std::make_shared<Control>(); control->block_open = true;
  Unblock unblock{control};
  auto state = Create(config, control);
  Eventually([&] { std::lock_guard lock(control->mutex); return control->entered_open; }, "Open did not start");
  Check(state->Submit(Frame(config, 1, 0)).Value() == SubmitStatus::WouldBlock, "Starting accepted input");
  auto other_control = std::make_shared<Control>(); auto other = Create(config, other_control); Ready(other, other_control); Close(other, other_control);
  state->Close(); Check(!state->Read().Succeeded(), "Close barrier not immediate");
  { std::lock_guard lock(control->mutex); control->block_open = false; control->changed.notify_all(); }
  Eventually([&] { return control->cleaned == 1; }, "Close during startup stalled");
  Check(control->wakes == 0 && control->closed == 1, "Retired startup admitted a callback");
}
void FailureAndWorkerExit() {
  auto config = Config(); auto control = std::make_shared<Control>();
  Unblock unblock{control};
  auto state = Create(config, control); Ready(state, control);
  { std::lock_guard lock(control->mutex); control->block_exit = true; }
  control->invalid_packet = true; control->fail_close = true;
  Submit(state, Frame(config, 1024, 0));
  Eventually([&] { auto read = state->Read(); return !read.Succeeded() && read.Error().code == CodecErrorCode::InvalidData; }, "Invalid platform AAC packet did not fail");
  Eventually([&] { std::lock_guard lock(control->mutex); return control->entered_exit; }, "Worker exit did not start");
  Check(control->cleaned == 0 && !state->Reset().Succeeded(), "Cleanup raced TLS exit or failed encoder reset");
  { std::lock_guard lock(control->mutex); control->block_exit = false; control->changed.notify_all(); }
  Eventually([&] { return control->cleaned == 1; }, "Failure cleanup stalled");
  Check(!control->cleanup_ok && control->cleanup_after_exit, "Cleanup failure/exit contract");
  state->Close(); Check(state->Read().Error().code == CodecErrorCode::Closed, "Close did not replace recorded failure");
}
void WakeupClose() {
  auto config = Config(); auto control = std::make_shared<Control>(); control->block_open = true;
  Unblock unblock{control};
  auto slot = std::make_shared<std::weak_ptr<AudioEncoderState>>();
  config.wakeup = [slot] { if (auto state = slot->lock()) state->Close(); };
  config.cleanup_completed = [control](auto result) { control->cleanup_ok = result.Succeeded(); ++control->cleaned; };
  auto created = AudioEncoderState::Create(config, [control] { return std::make_unique<Fake>(control); });
  Check(created.Succeeded(), "Cannot create callback-close encoder");
  auto state = std::move(created).Value(); *slot = state;
  { std::lock_guard lock(control->mutex); control->block_open = false; control->changed.notify_all(); }
  Eventually([&] { return control->cleaned == 1; }, "Close from wakeup deadlocked");
  Check(control->cleanup_ok && state->Read().Error().code == CodecErrorCode::Closed, "Callback close failed");
}
void Validation() {
  auto config = Config(); Check(ValidateAudioEncoderConfig(config).Succeeded(), "Default config invalid");
  config.input.sample_format = AudioSampleFormat::F32;
  Check(ValidateAudioEncoderConfig(config).Error().code == CodecErrorCode::Unsupported, "Unsupported PCM accepted");
  config = Config(); config.limits.output_bytes = 16 * 65536 - 1;
  Check(!ValidateAudioEncoderConfig(config).Succeeded(), "Impossible output pool accepted");
  config = Config(); config.limits.input_items = 0;
  Check(!ValidateAudioEncoderConfig(config).Succeeded(), "Zero input pool accepted");
  config = Config();
  const auto format = DescribeAac(config);
  Check(ReadAudioEncodeFormat(format.initialization_data, config).Succeeded(), "Invalid canonical ASC");
  auto extended = format.initialization_data; extended.insert(extended.end(), {std::byte{0x56}, std::byte{0xe5}, std::byte{0}});
  Check(ReadAudioEncodeFormat(extended, config).Value() == format, "LC extension not normalized");
  extended.back() = std::byte{1}; Check(!ReadAudioEncodeFormat(extended, config).Succeeded(), "Unknown ASC extension accepted");
}

#if defined(MEDIA_HAS_FLAC_ENCODER) || defined(MEDIA_HAS_MP3_ENCODER)
struct EncoderActivity {
  std::atomic<unsigned> submits{0}, polls{0};
};

class ObservedEncoder final : public AudioEncoderBackend {
public:
  ObservedEncoder(std::unique_ptr<AudioEncoderBackend> encoder, std::shared_ptr<EncoderActivity> activity)
      : encoder_(std::move(encoder)), activity_(std::move(activity)) {}
  CodecResult<void> Open(const AudioEncoderConfig& config) override { return encoder_->Open(config); }
  CodecResult<SubmitStatus> Submit(std::span<const std::byte> pcm, std::uint32_t samples, std::int64_t start) override {
    auto result = encoder_->Submit(pcm, samples, start);
    if (result.Succeeded() && result.Value() == SubmitStatus::Accepted) ++activity_->submits;
    return result;
  }
  CodecResult<std::optional<AudioEncoderBackendOutput>> Poll() override {
    if (activity_->submits) ++activity_->polls;
    return encoder_->Poll();
  }
  CodecResult<SubmitStatus> Finish() override { return encoder_->Finish(); }
  CodecResult<void> Close() override { return encoder_->Close(); }

private:
  std::unique_ptr<AudioEncoderBackend> encoder_;
  std::shared_ptr<EncoderActivity> activity_;
};

void SoftwareEncoderWaitsForInput(CodecId codec, AudioEncoderFactory factory) {
  auto config = Config();
  config.codec = codec;
  auto activity = std::make_shared<EncoderActivity>();
  auto cleaned = std::make_shared<std::atomic<bool>>(false);
  config.cleanup_completed = [cleaned](auto result) { *cleaned = result.Succeeded(); };
  auto opened = AudioEncoderState::Create(config, [factory = std::move(factory), activity] {
    return std::make_unique<ObservedEncoder>(factory(), activity);
  });
  Check(opened.Succeeded(), "Cannot create software encoder");
  auto state = std::move(opened).Value();
  const auto block = AudioEncoderBlockSamples(codec);
  Submit(state, Frame(config, block, 0));
  Eventually([&] { return activity->polls > 0; }, "Software encoder did not process its first PCM block");
  const auto before = activity->polls.load();
  std::this_thread::sleep_for(std::chrono::milliseconds(80));
  Check(activity->polls <= before + 3, "Software encoder repeatedly polled without new input");

  Submit(state, Frame(config, block, block));
  Eventually([&] { return activity->submits == 2; }, "New PCM did not wake the software encoder");
  Submit(state, Frame(config, 1, block * 2));
  Check(state->Finish().Succeeded(), "Software encoder rejected drain");
  std::optional<AudioEncodeSummary> summary;
  Eventually([&] {
    auto read = state->Read();
    Check(read.Succeeded(), "Software encoder failed during drain");
    if (!read.Value()) return false;
    if (auto* value = std::get_if<AudioEncodeSummary>(&*read.Value())) summary = *value;
    return std::holds_alternative<EndOfStream>(*read.Value());
  }, "Software encoder did not drain after waiting for input");
  Check(summary && summary->input_samples == block * 2 + 1, "Software encoder changed the accepted sample count");
  state->Close();
  Eventually([&] { return cleaned->load(); }, "Software encoder cleanup stalled");
}
#endif

#ifdef MEDIA_HAS_FLAC_ENCODER
void FlacStreamableSampleRates() {
  auto config = Config();
  config.codec = CodecId::Flac;
  for (const auto rate : {65536u, 65537u, 191999u}) {
    config.input.sample_rate = rate;
    auto result = AudioEncoder::Create(config);
    Check(!result.Succeeded() && result.Error().code == CodecErrorCode::Unsupported &&
        result.Error().message.find("Streamable Subset") != std::string::npos, "FLAC accepted a non-streamable rate");
  }
  for (const auto rate : {8000u, 65535u, 65540u, 192000u}) {
    config.input.sample_rate = rate;
    std::atomic<bool> completed{false}, supported{false};
    AudioEncoder::Probe(config, [&](auto result) { supported = result.Succeeded(); completed = true; });
    Eventually([&] { return completed.load(); }, "FLAC sample-rate probe stalled");
    Check(supported, "FLAC rejected a streamable rate");
  }
}
#endif
} // namespace
int main() {
  try {
    Validation(); TinyInputsAndTiming(); BackpressureAndRetainedSlices(); StartupCloseAndIsolation(); FailureAndWorkerExit(); WakeupClose();
    std::cout << "AAC encoder queue, timing, trim, retention, isolation and lifecycle contracts passed\n";
#ifdef MEDIA_HAS_FLAC_ENCODER
    SoftwareEncoderWaitsForInput(CodecId::Flac, MakeFlacEncoderBackend);
    FlacStreamableSampleRates();
    std::cout << "FLAC encoder idle, drain, and sample-rate contracts passed\n";
#endif
#ifdef MEDIA_HAS_MP3_ENCODER
    SoftwareEncoderWaitsForInput(CodecId::Mp3, MakeMp3EncoderBackend);
    std::cout << "MP3 encoder idle and drain contracts passed\n";
#endif
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
