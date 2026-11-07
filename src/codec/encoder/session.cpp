#include "session.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <system_error>

namespace huxerui::media::detail {
namespace {
struct SessionFailure { CodecError error; };
void Reject(const char* message) { throw SessionFailure{Error(CodecErrorCode::InvalidData, message)}; }
}
AudioEncoderState::AudioEncoderState(AudioEncoderConfig config, AudioEncoderFactory factory)
    : config_(std::move(config)), factory_(std::move(factory)) {}

CodecResult<std::shared_ptr<AudioEncoderState>> AudioEncoderState::Create(AudioEncoderConfig config, AudioEncoderFactory factory) {
  using R = CodecResult<std::shared_ptr<AudioEncoderState>>;
  auto valid = ValidateAudioEncoderConfig(config);
  if (!valid.Succeeded()) return R::Failure(valid.Error());
  try {
    auto state = std::shared_ptr<AudioEncoderState>(new AudioEncoderState(std::move(config), std::move(factory)));
    state->input_budget_->session = state;
    state->output_budget_->session = state;
#ifndef __EMSCRIPTEN__
    state->worker_ = MediaWorker::Start([state](bool capacity) { state->Run(capacity); });
#endif
    std::lock_guard lock(state->mutex_);
    state->ScheduleLocked();
    return R::Success(std::move(state));
  } catch (const std::bad_alloc&) {
    return R::Failure(Error(CodecErrorCode::ResourceExhausted, "Cannot allocate audio encoder state"));
  } catch (const std::system_error& error) {
    return R::Failure(Error(CodecErrorCode::ResourceExhausted, "Cannot start audio encoder worker", error.what()));
  }
}
AudioEncoderState::Credit::~Credit() {
  bool signal;
  {
    std::lock_guard lock(budget->mutex);
    --budget->items;
    budget->bytes -= bytes;
    signal = notify && budget->active;
  }
  if (auto session = budget->session.lock(); signal && session) {
#ifdef __EMSCRIPTEN__
    DispatchMedia([session] { session->CreditAvailable(); });
#else
    session->worker_->Signal();
#endif
  }
}
void AudioEncoderState::Credit::Shrink(std::size_t size) {
  std::lock_guard lock(budget->mutex);
  budget->bytes -= bytes - size;
  bytes = size;
}
std::shared_ptr<AudioEncoderState::Credit> AudioEncoderState::Reserve(const std::shared_ptr<Budget>& budget,
    std::size_t bytes, std::size_t max_items, std::size_t max_bytes) {
  std::lock_guard lock(budget->mutex);
  if (budget->items >= max_items || bytes > max_bytes - budget->bytes) return {};
  auto credit = std::make_shared<Credit>();
  credit->budget = budget;
  credit->bytes = bytes;
  ++budget->items;
  budget->bytes += bytes;
  return credit;
}
void AudioEncoderState::ScheduleLocked(unsigned delay) {
#ifdef __EMSCRIPTEN__
  if (scheduled_) return;
  auto self = shared_from_this();
  DispatchMedia([self] { self->Run(); }, delay);
  scheduled_ = true;
#else
  worker_->Schedule(delay);
#endif
}
#ifdef __EMSCRIPTEN__
void AudioEncoderState::CreditAvailable() {
  std::lock_guard lock(mutex_);
  if (phase_ == Phase::Closed || phase_ == Phase::Failed) return;
  wake_pending_ = true;
  ScheduleLocked();
}
#endif
CodecError AudioEncoderState::StateError() const {
  if (phase_ == Phase::Closed) return Error(CodecErrorCode::Closed, "Audio encoder is closed");
  return failure_.value_or(Error(CodecErrorCode::InvalidState, "Audio encoder is not running"));
}
CodecResult<SubmitStatus> AudioEncoderState::Submit(const AudioFrame& frame) {
  using R = CodecResult<SubmitStatus>;
  std::lock_guard lock(mutex_);
  if (phase_ == Phase::Starting || phase_ == Phase::Resetting) return R::Success(SubmitStatus::WouldBlock);
  if (phase_ != Phase::Running) return R::Failure(StateError());
  const auto pts = frame.pts.Rescale(config_.input.sample_rate);
  if (!frame.Validate() || frame.format != config_.input || !pts)
    return R::Failure(Error(CodecErrorCode::InvalidData, "Invalid PCM frame or encoder input format"));
  const auto count = frame.sample_count;
  const auto bytes = std::size_t(count) * config_.input.ChannelCount() * 2;
  if (bytes > config_.limits.input_bytes)
    return R::Failure(Error(CodecErrorCode::InvalidData, "PCM frame exceeds input byte limit"));
  const auto maximum = std::numeric_limits<std::int64_t>::max();
  if (input_samples_ > std::uint64_t(maximum - 1023) - count || pts->ticks > maximum - count ||
      (input_start_ && (input_start_->ticks > maximum - std::int64_t(input_samples_) ||
          pts->ticks != input_start_->ticks + std::int64_t(input_samples_))))
    return R::Failure(Error(CodecErrorCode::InvalidData, "PCM timestamps must be contiguous and fit the sample timeline"));
  try {
    auto credit = Reserve(input_budget_, bytes, config_.limits.input_items, config_.limits.input_bytes);
    if (!credit) return R::Success(SubmitStatus::WouldBlock);
    struct Storage { std::shared_ptr<Credit> credit; Bytes bytes; };
    const auto source = frame.planes.front().AsBytes().first(bytes);
    auto owner = std::make_shared<Storage>(Storage{credit, Bytes(source.begin(), source.end())});
    ScheduleLocked();
    input_.push_back({BufferReference(owner->bytes, owner)});
    if (!input_start_) input_start_ = pts;
    input_samples_ += count;
    credit->notify = true;
    return R::Success(SubmitStatus::Accepted);
  } catch (const std::bad_alloc&) {
    return R::Failure(Error(CodecErrorCode::ResourceExhausted, "Cannot copy encoder PCM input"));
  }
}
CodecResult<std::optional<AudioEncodeOutput>> AudioEncoderState::Read() {
  using R = CodecResult<std::optional<AudioEncodeOutput>>;
  std::lock_guard lock(mutex_);
  if (phase_ == Phase::Closed || phase_ == Phase::Failed) return R::Failure(StateError());
  if (phase_ == Phase::Resetting || output_.empty()) return R::Success(std::nullopt);
  auto result = std::move(output_.front());
  output_.pop_front();
  if (std::holds_alternative<EndOfStream>(result)) phase_ = Phase::Ended;
  ScheduleLocked();
  return R::Success(std::move(result));
}
CodecResult<void> AudioEncoderState::Finish() {
  std::lock_guard lock(mutex_);
  if (phase_ == Phase::Draining || phase_ == Phase::Ended) return CodecResult<void>::Success();
  if (phase_ != Phase::Running) return CodecResult<void>::Failure(StateError());
  phase_ = Phase::Draining;
  ScheduleLocked();
  return CodecResult<void>::Success();
}
CodecResult<void> AudioEncoderState::Reset() {
  std::lock_guard lock(mutex_);
  if (phase_ == Phase::Resetting) return CodecResult<void>::Success();
  if (phase_ == Phase::Starting || phase_ == Phase::Failed || phase_ == Phase::Closed)
    return CodecResult<void>::Failure(phase_ == Phase::Closed ? StateError() : Error(CodecErrorCode::InvalidState, "Cannot reset this encoder"));
  ++generation_;
  phase_ = Phase::Resetting;
  ScheduleLocked();
  return CodecResult<void>::Success();
}
void AudioEncoderState::Close() noexcept {
  std::lock_guard lock(mutex_);
  if (phase_ == Phase::Closed) return;
  phase_ = Phase::Closed;
  ++generation_;
  if (!cleaned_) ScheduleLocked();
}
AudioEncoderState::Retired AudioEncoderState::Clear() {
  Retired retired;
  retired.input.swap(input_);
  retired.output.swap(output_);
  retired.reservations.swap(reservations_);
  assembly_samples_ = 0;
  input_samples_ = submitted_samples_ = encoded_samples_ = 0;
  input_start_.reset(); last_packet_pts_.reset(); format_.reset();
  waiting_credit_ = finish_sent_ = eos_queued_ = extra_reserved_ = false;
  return retired;
}
void AudioEncoderState::Fail(CodecError error, std::uint64_t generation) {
  std::lock_guard lock(mutex_);
  if (generation != generation_ || phase_ == Phase::Closed || phase_ == Phase::Failed) return;
  failure_ = std::move(error);
  phase_ = Phase::Failed;
  wake_pending_ = true;
}
void AudioEncoderState::Notify() {
  bool admitted;
  {
    std::lock_guard lock(mutex_);
    admitted = wake_pending_ && phase_ != Phase::Closed && !cleaned_;
    wake_pending_ = false;
  }
  if (admitted && config_.wakeup) config_.wakeup();
}
void AudioEncoderState::Cleanup() {
  auto result = backend_ ? backend_->Close() : CodecResult<void>::Success();
  if (cleanup_failure_) result = CodecResult<void>::Failure(*cleanup_failure_);
  backend_.reset();
  for (auto& budget : {input_budget_, output_budget_}) {
    std::lock_guard lock(budget->mutex);
    budget->active = false;
  }
  std::function<void(CodecResult<void>)> completion;
  {
    Retired retired;
    {
      std::lock_guard lock(mutex_);
      retired = Clear();
      cleaned_ = true;
      completion = std::move(config_.cleanup_completed);
    }
  }
  config_.wakeup = {};
  factory_ = {};
#ifdef __EMSCRIPTEN__
  if (completion) completion(std::move(result));
#else
  worker_->Finish(std::move(result), std::move(completion));
#endif
}
void AudioEncoderState::Run(bool capacity) {
  std::uint64_t generation;
  Phase phase;
  {
    std::lock_guard lock(mutex_);
#ifdef __EMSCRIPTEN__
    scheduled_ = false;
#endif
    if (cleaned_) return;
    generation = generation_;
    phase = phase_;
    if (capacity && phase != Phase::Closed && phase != Phase::Failed) wake_pending_ = true;
  }
  try {
    if (phase == Phase::Starting || phase == Phase::Resetting) {
      if (backend_ && backend_generation_ != generation) {
        auto closed = backend_->Close();
        backend_.reset();
        if (!closed.Succeeded()) { cleanup_failure_ = closed.Error(); Fail(closed.Error(), generation); }
      }
      {
        Retired retired;
        {
          std::lock_guard lock(mutex_);
          if (generation != generation_ || phase_ == Phase::Closed || phase_ == Phase::Failed) phase = Phase::Closed;
          else if (!backend_) retired = Clear();
        }
      }
      if (phase != Phase::Closed) {
        if (!backend_) {
          backend_ = factory_();
          backend_generation_ = generation;
          auto opened = backend_->Open(config_);
          if (!opened.Succeeded()) Fail(opened.Error(), generation);
        }
        auto ready = backend_->Ready();
        if (!ready.Succeeded()) Fail(ready.Error(), generation);
        else {
          std::lock_guard lock(mutex_);
          if (generation == generation_ && phase_ != Phase::Closed && phase_ != Phase::Failed) {
            if (ready.Value()) { phase_ = Phase::Running; wake_pending_ = true; }
            else ScheduleLocked(2);
          }
        }
      }
    }
    Process(generation);
  } catch (const SessionFailure& failure) {
    Fail(failure.error, generation);
  } catch (const std::bad_alloc&) {
    Fail(Error(CodecErrorCode::ResourceExhausted, "Audio encoder allocation failed"), generation);
  } catch (const std::exception& error) {
    Fail(Error(CodecErrorCode::PlatformFailure, error.what()), generation);
  }
  Notify();
  bool terminal;
  {
    std::lock_guard lock(mutex_);
    terminal = phase_ == Phase::Closed || phase_ == Phase::Failed;
    if (!terminal && (phase_ == Phase::Running || phase_ == Phase::Draining) && !eos_queued_ &&
        ((!waiting_credit_ && (!input_.empty() || assembly_samples_ == 1024)) ||
            (submitted_samples_ > encoded_samples_ && !waiting_credit_) ||
            (phase_ == Phase::Draining && (!waiting_credit_ || finish_sent_)))) ScheduleLocked(2);
  }
  if (terminal) Cleanup();
}
void AudioEncoderState::End(AudioEncoderEnd end) {
  if (eos_queued_) return;
  if (phase_ != Phase::Draining || !input_.empty() || assembly_samples_ || (input_samples_ && !finish_sent_))
    Reject("AAC encoder ended before draining accepted PCM");
  if (!input_samples_) end = {0, 0};
  else if (end.padding_samples) {
    const auto fill = submitted_samples_ - input_samples_;
    if (*end.padding_samples > UINT64_MAX - fill) Reject("AAC padding count overflow");
    *end.padding_samples += fill;
  }
  if (end.priming_samples && !end.padding_samples && *end.priming_samples <= encoded_samples_ &&
      input_samples_ <= encoded_samples_ - *end.priming_samples)
    end.padding_samples = encoded_samples_ - *end.priming_samples - input_samples_;
  if (encoded_samples_ < input_samples_ ||
      (end.priming_samples && *end.priming_samples > encoded_samples_ - input_samples_) ||
      (end.padding_samples && *end.padding_samples > encoded_samples_ - input_samples_) ||
      (end.priming_samples && end.padding_samples &&
      (*end.priming_samples > encoded_samples_ || *end.padding_samples > encoded_samples_ - *end.priming_samples ||
          encoded_samples_ - *end.priming_samples - *end.padding_samples != input_samples_)))
    Reject("Platform AAC sample accounting is inconsistent");
  output_.push_back(AudioEncodeSummary{input_start_, input_samples_, encoded_samples_, end.priming_samples, end.padding_samples});
  output_.push_back(EndOfStream{});
  reservations_.clear();
  eos_queued_ = true;
  wake_pending_ = true;
}
void AudioEncoderState::Publish(AudioEncoderBackendOutput value, std::uint64_t generation) {
  std::lock_guard lock(mutex_);
  if (generation != generation_ || phase_ == Phase::Closed || phase_ == Phase::Failed) return;
  if (auto* format = std::get_if<AudioEncodeFormat>(&value)) {
    auto valid = ValidateAudioEncodeFormat(*format, config_);
    if (!valid.Succeeded()) throw SessionFailure{valid.Error()};
    if (format_ && *format_ != *format) Reject("AAC format changed during encoding");
    if (!format_) { format_ = *format; output_.push_back(std::move(*format)); wake_pending_ = true; }
  } else if (auto* packet = std::get_if<EncodedPacket>(&value)) {
    const auto size = packet->data.AsBytes().size();
    const auto pts = packet->pts.Rescale(config_.input.sample_rate);
    const auto dts = packet->dts ? packet->dts->Rescale(config_.input.sample_rate) : std::optional<MediaTimestamp>{};
    const auto anchor = input_start_ ? input_start_->ticks : 0;
    auto fits = [anchor](std::int64_t ticks) {
      return (anchor <= 0 || ticks <= INT64_MAX - anchor) && (anchor >= 0 || ticks >= INT64_MIN - anchor);
    };
    if (!format_ || !input_start_ || !size || size > kAudioEncoderPacketBytes || reservations_.empty() || !pts ||
        (packet->dts && !dts) || !fits(pts->ticks) || (dts && !fits(dts->ticks)) ||
        (last_packet_pts_ && (last_packet_pts_->ticks > INT64_MAX - 1024 || pts->ticks != last_packet_pts_->ticks + 1024)) ||
        encoded_samples_ > UINT64_MAX - 1024)
      Reject("AAC packet exceeds reserved storage or has invalid timing");
    auto credit = std::move(reservations_.front());
    reservations_.pop_front();
    credit->Shrink(size);
    struct Storage { std::shared_ptr<Credit> credit; BufferReference bytes; };
    auto storage = std::make_shared<Storage>(Storage{credit, packet->data});
    packet->data = BufferReference(storage->bytes.AsBytes(), storage);
    last_packet_pts_ = pts;
    packet->pts = {pts->ticks + anchor, config_.input.sample_rate};
    if (dts) packet->dts = MediaTimestamp{dts->ticks + anchor, config_.input.sample_rate};
    packet->duration = MediaTimestamp{1024, config_.input.sample_rate};
    packet->key_frame = true;
    encoded_samples_ += 1024;
    output_.push_back(std::move(*packet));
    wake_pending_ = true;
  } else End(std::get<AudioEncoderEnd>(value));
}
void AudioEncoderState::Process(std::uint64_t generation) {
  for (unsigned step = 0; step < 32; ++step) {
    {
      std::lock_guard lock(mutex_);
      if (generation != generation_ || (phase_ != Phase::Running && phase_ != Phase::Draining) || eos_queued_) return;
    }
    auto polled = backend_->Poll();
    if (!polled.Succeeded()) { Fail(polled.Error(), generation); return; }
    if (polled.Value()) { Publish(std::move(*polled.Value()), generation); continue; }
    bool submit = false, finish = false;
    std::shared_ptr<Credit> credit;
    {
      std::lock_guard lock(mutex_);
      if (generation != generation_ || (phase_ != Phase::Running && phase_ != Phase::Draining)) return;
      const auto stride = config_.input.ChannelCount() * 2;
      while (assembly_samples_ < 1024 && !input_.empty()) {
        auto& input = input_.front();
        const auto bytes = input.data.AsBytes().subspan(input.offset);
        const auto count = std::min<std::size_t>(1024 - assembly_samples_, bytes.size() / stride);
        std::memcpy(assembly_.data() + assembly_samples_ * stride, bytes.data(), count * stride);
        assembly_samples_ += static_cast<std::uint32_t>(count);
        input.offset += count * stride;
        if (input.offset == input.data.AsBytes().size()) input_.pop_front();
      }
      if (phase_ == Phase::Draining && input_.empty() && assembly_samples_ && assembly_samples_ < 1024) {
        std::fill(assembly_.begin() + assembly_samples_ * stride, assembly_.begin() + 1024 * stride, std::byte{});
        assembly_samples_ = 1024;
      }
      waiting_credit_ = false;
      if (assembly_samples_ == 1024) {
        if (!extra_reserved_) {
          while (reservations_.size() < kAudioEncoderExtraPackets) {
            auto extra = Reserve(output_budget_, kAudioEncoderPacketBytes, config_.limits.output_items, config_.limits.output_bytes);
            if (!extra) { waiting_credit_ = true; return; }
            extra->notify = true;
            reservations_.push_back(std::move(extra));
          }
          extra_reserved_ = true;
        }
        credit = Reserve(output_budget_, kAudioEncoderPacketBytes, config_.limits.output_items, config_.limits.output_bytes);
        waiting_credit_ = !credit;
        submit = bool(credit);
        if (credit) { credit->notify = true; reservations_.push_back(credit); }
      } else if (phase_ == Phase::Draining && input_.empty() && !finish_sent_) {
        if (!input_samples_) { End({0, 0}); return; }
        finish = true;
      }
    }
    if (submit) {
      auto result = backend_->Submit(std::span(assembly_).first(config_.input.ChannelCount() * 2 * 1024), 1024,
          static_cast<std::int64_t>(submitted_samples_));
      if (!result.Succeeded()) { Fail(result.Error(), generation); return; }
      std::lock_guard lock(mutex_);
      if (generation != generation_) return;
      if (result.Value() == SubmitStatus::WouldBlock) { reservations_.pop_back(); return; }
      submitted_samples_ += 1024;
      assembly_samples_ = 0;
    } else if (finish) {
      auto result = backend_->Finish();
      if (!result.Succeeded()) { Fail(result.Error(), generation); return; }
      std::lock_guard lock(mutex_);
      if (generation == generation_ && result.Value() == SubmitStatus::Accepted) finish_sent_ = true;
      return;
    } else return;
  }
  std::lock_guard lock(mutex_);
  ScheduleLocked();
}
} // namespace huxerui::media::detail
