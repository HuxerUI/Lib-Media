#include "session.h"
#include "format/platform_video_storage.h"

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
    : config_(std::move(config)), factory_(std::move(factory)),
      block_samples_(AudioEncoderBlockSamples(config_.codec)),
      sample_stride_(config_.input.ChannelCount() * config_.input.BytesPerSample()),
      assembly_(block_samples_ * sample_stride_) {}

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
  const auto bytes = std::size_t(count) * sample_stride_;
  if (bytes > config_.limits.input_bytes)
    return R::Failure(Error(CodecErrorCode::InvalidData, "PCM frame exceeds input byte limit"));
  const auto maximum = std::numeric_limits<std::int64_t>::max();
  if (input_samples_ > std::uint64_t(maximum - block_samples_ + 1) - count || pts->ticks > maximum - count ||
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
  input_start_.reset(); next_packet_pts_.reset(); format_.reset();
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
        ((!waiting_credit_ && (!input_.empty() || assembly_samples_ == block_samples_)) ||
            (config_.codec == CodecId::Aac && submitted_samples_ > encoded_samples_ && !waiting_credit_) ||
            (phase_ == Phase::Draining && (!waiting_credit_ || finish_sent_)))) ScheduleLocked(2);
  }
  if (terminal) Cleanup();
}
void AudioEncoderState::End(AudioEncoderEnd end) {
  if (eos_queued_) return;
  if (phase_ != Phase::Draining || !input_.empty() || assembly_samples_ || (input_samples_ && !finish_sent_))
    Reject("Audio encoder ended before draining accepted PCM");
  if (!input_samples_) end = {0, 0};
  else if (end.padding_samples) {
    const auto fill = submitted_samples_ - input_samples_;
    if (*end.padding_samples > UINT64_MAX - fill) Reject("Audio encoder padding count overflow");
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
    Reject("Audio encoder sample accounting is inconsistent");
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
    if (format_ && *format_ != *format) Reject("Audio format changed during encoding");
    if (!format_) { format_ = *format; output_.push_back(std::move(*format)); wake_pending_ = true; }
  } else if (auto* packet = std::get_if<EncodedPacket>(&value)) {
    const auto size = packet->data.AsBytes().size();
    const auto pts = packet->pts.Rescale(config_.input.sample_rate);
    const auto dts = packet->dts ? packet->dts->Rescale(config_.input.sample_rate) : std::optional<MediaTimestamp>{};
    const auto length = config_.codec == CodecId::Aac ? std::optional{MediaTimestamp{block_samples_, config_.input.sample_rate}} :
        packet->duration ? packet->duration->Rescale(config_.input.sample_rate) : std::nullopt;
    const auto anchor = input_start_ ? input_start_->ticks : 0;
    auto fits = [anchor](std::int64_t ticks) {
      return (anchor <= 0 || ticks <= INT64_MAX - anchor) && (anchor >= 0 || ticks >= INT64_MIN - anchor);
    };
    if (!format_ || !input_start_ || !size || size > kAudioEncoderPacketBytes || reservations_.empty() || !pts || !length ||
        length->ticks <= 0 || length->ticks > block_samples_ ||
        (packet->dts && !dts) || !fits(pts->ticks) || (dts && !fits(dts->ticks)) ||
        (next_packet_pts_ && pts->ticks != *next_packet_pts_) || pts->ticks > INT64_MAX - length->ticks ||
        encoded_samples_ > UINT64_MAX - std::uint64_t(length->ticks))
      Reject("Audio packet exceeds reserved storage or has invalid timing");
    auto credit = std::move(reservations_.front());
    reservations_.pop_front();
    credit->Shrink(size);
    struct Storage { std::shared_ptr<Credit> credit; BufferReference bytes; };
    auto storage = std::make_shared<Storage>(Storage{credit, packet->data});
    packet->data = BufferReference(storage->bytes.AsBytes(), storage);
    next_packet_pts_ = pts->ticks + length->ticks;
    packet->pts = {pts->ticks + anchor, config_.input.sample_rate};
    if (dts) packet->dts = MediaTimestamp{dts->ticks + anchor, config_.input.sample_rate};
    packet->duration = length;
    if (config_.codec == CodecId::Aac) packet->key_frame = true;
    encoded_samples_ += length->ticks;
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
      while (assembly_samples_ < block_samples_ && !input_.empty()) {
        auto& input = input_.front();
        const auto bytes = input.data.AsBytes().subspan(input.offset);
        const auto count = std::min<std::size_t>(block_samples_ - assembly_samples_, bytes.size() / sample_stride_);
        std::memcpy(assembly_.data() + assembly_samples_ * sample_stride_, bytes.data(), count * sample_stride_);
        assembly_samples_ += static_cast<std::uint32_t>(count);
        input.offset += count * sample_stride_;
        if (input.offset == input.data.AsBytes().size()) input_.pop_front();
      }
      const bool tail = phase_ == Phase::Draining && input_.empty();
      if (tail && config_.codec == CodecId::Aac && assembly_samples_ && assembly_samples_ < block_samples_) {
        std::fill(assembly_.begin() + assembly_samples_ * sample_stride_, assembly_.end(), std::byte{});
        assembly_samples_ = block_samples_;
      }
      waiting_credit_ = false;
      if (assembly_samples_ == block_samples_ || (tail && assembly_samples_)) {
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
      auto result = backend_->Submit(std::span(assembly_).first(assembly_samples_ * sample_stride_), assembly_samples_,
          static_cast<std::int64_t>(submitted_samples_));
      if (!result.Succeeded()) { Fail(result.Error(), generation); return; }
      std::lock_guard lock(mutex_);
      if (generation != generation_) return;
      if (result.Value() == SubmitStatus::WouldBlock) { reservations_.pop_back(); return; }
      submitted_samples_ += assembly_samples_;
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
VideoEncoderState::VideoEncoderState(VideoEncoderConfig config, VideoEncoderFactory factory, std::size_t frame_bytes)
    : config_(std::move(config)), factory_(std::move(factory)), frame_bytes_(frame_bytes) {}
CodecResult<std::shared_ptr<VideoEncoderState>> VideoEncoderState::Create(VideoEncoderConfig config, VideoEncoderFactory factory) {
  using R = CodecResult<std::shared_ptr<VideoEncoderState>>;
  auto valid = ValidateVideoEncoderConfig(config);
  if (!valid.Succeeded()) return R::Failure(valid.Error());
  try {
    auto state = std::shared_ptr<VideoEncoderState>(new VideoEncoderState(std::move(config), std::move(factory), valid.Value()));
    state->input_budget_->session = state; state->output_budget_->session = state;
#ifndef __EMSCRIPTEN__
    state->worker_ = MediaWorker::Start([state](bool capacity) { state->Run(capacity); });
#endif
    std::lock_guard lock(state->mutex_);
    state->ScheduleLocked();
    return R::Success(std::move(state));
  } catch (const std::bad_alloc&) {
    return R::Failure(Error(CodecErrorCode::ResourceExhausted, "Cannot allocate video encoder state"));
  } catch (const std::system_error& error) {
    return R::Failure(Error(CodecErrorCode::ResourceExhausted, "Cannot start video encoder worker", error.what()));
  }
}
VideoEncoderState::Credit::~Credit() {
  bool signal;
  {
    std::lock_guard lock(budget->mutex);
    --budget->items; budget->bytes -= bytes;
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
void VideoEncoderState::Credit::Shrink(std::size_t size) {
  std::lock_guard lock(budget->mutex);
  budget->bytes -= bytes - size; bytes = size;
}
std::shared_ptr<VideoEncoderState::Credit> VideoEncoderState::Reserve(const std::shared_ptr<Budget>& budget,
    std::size_t bytes, std::size_t max_items, std::size_t max_bytes) {
  std::lock_guard lock(budget->mutex);
  if (budget->items >= max_items || bytes > max_bytes - budget->bytes) return {};
  auto credit = std::make_shared<Credit>();
  credit->budget = budget; credit->bytes = bytes;
  ++budget->items; budget->bytes += bytes;
  return credit;
}
void VideoEncoderState::ScheduleLocked(unsigned delay) {
#ifdef __EMSCRIPTEN__
  if (scheduled_) return;
  auto self = shared_from_this();
  DispatchMedia([self] { self->Run(); }, delay); scheduled_ = true;
#else
  worker_->Schedule(delay);
#endif
}
#ifdef __EMSCRIPTEN__
void VideoEncoderState::CreditAvailable() {
  std::lock_guard lock(mutex_);
  if (phase_ == Phase::Closed || phase_ == Phase::Failed) return;
  wake_pending_ = true; ScheduleLocked();
}
#endif
CodecError VideoEncoderState::StateError() const {
  if (phase_ == Phase::Closed) return Error(CodecErrorCode::Closed, "Video encoder is closed");
  return failure_.value_or(Error(CodecErrorCode::InvalidState, "Video encoder is not running"));
}
CodecResult<SubmitStatus> VideoEncoderState::Submit(const VideoFrame& frame) {
  using R = CodecResult<SubmitStatus>;
  std::lock_guard lock(mutex_);
  if (phase_ == Phase::Starting || phase_ == Phase::Resetting) return R::Success(SubmitStatus::WouldBlock);
  if (phase_ != Phase::Running) return R::Failure(StateError());
  const auto pts = frame.pts.Rescale(1000000);
  const auto duration = frame.duration ? frame.duration->Rescale(1000000) : std::optional<MediaTimestamp>{};
  if (!frame.Validate() || frame.format != config_.input || !pts ||
      frame.Storage() != config_.input_storage ||
      (frame.duration && (!duration || duration->ticks <= 0)) ||
      (last_input_pts_ && frame.pts.Compare(*last_input_pts_) != 1) ||
      (last_micro_pts_ && pts->ticks <= *last_micro_pts_) ||
      (origin_ && *origin_ < 0 && pts->ticks > INT64_MAX + *origin_))
    return R::Failure(Error(CodecErrorCode::InvalidData, "Invalid video pixels, nonincreasing PTS, or unrepresentable timing"));
  try {
    auto credit = Reserve(input_budget_, frame_bytes_, config_.limits.input_items, config_.limits.input_bytes);
    if (!credit) return R::Success(SubmitStatus::WouldBlock);
    VideoFrame copy{config_.input, frame.pts, frame.duration, {}};
    if (frame.Storage() == VideoStorage::Platform) {
      copy = frame;
      VideoFrameAccess::RetainPlatformStorage(copy, credit);
    } else {
      struct Storage { std::shared_ptr<Credit> credit; Bytes bytes; };
      auto owner = std::make_shared<Storage>(Storage{credit, Bytes(frame_bytes_)});
      const auto layout = config_.input.pixel_format;
      CopyVideoEncoderPixels(frame, layout, owner->bytes);
      const auto sample = VideoComponentBytes(layout);
      const auto row = std::size_t(config_.input.width) * sample;
      const auto luma = row * config_.input.height;
      BufferReference pixels(owner->bytes, owner);
      copy.planes.push_back({pixels.Slice(0, luma), row, sample});
      if (layout != VideoPixelFormat::I420)
        copy.planes.push_back({pixels.Slice(luma, frame_bytes_ - luma), row, sample * 2});
      else {
        copy.planes.push_back({pixels.Slice(luma, luma / 4), row / 2, 1});
        copy.planes.push_back({pixels.Slice(luma + luma / 4, luma / 4), row / 2, 1});
      }
    }
    ScheduleLocked(); input_.push_back(std::move(copy));
    if (!origin_) origin_ = pts->ticks;
    last_micro_pts_ = pts->ticks; last_input_pts_ = frame.pts; credit->notify = true;
    return R::Success(SubmitStatus::Accepted);
  } catch (const std::bad_alloc&) {
    return R::Failure(Error(CodecErrorCode::ResourceExhausted, "Cannot retain video encoder input"));
  }
}
CodecResult<std::optional<VideoEncodeOutput>> VideoEncoderState::Read() {
  using R = CodecResult<std::optional<VideoEncodeOutput>>;
  std::lock_guard lock(mutex_);
  if (phase_ == Phase::Closed || phase_ == Phase::Failed) return R::Failure(StateError());
  if (phase_ == Phase::Resetting || output_.empty()) return R::Success(std::nullopt);
  auto value = std::move(output_.front()); output_.pop_front();
  if (std::holds_alternative<EndOfStream>(value)) phase_ = Phase::Ended;
  ScheduleLocked();
  return R::Success(std::move(value));
}
CodecResult<void> VideoEncoderState::RequestKeyFrame() {
  std::lock_guard lock(mutex_);
  if (phase_ != Phase::Running) return CodecResult<void>::Failure(StateError());
  key_requested_ = true; ScheduleLocked();
  return CodecResult<void>::Success();
}
CodecResult<void> VideoEncoderState::Finish() {
  std::lock_guard lock(mutex_);
  if (phase_ == Phase::Draining || phase_ == Phase::Ended) return CodecResult<void>::Success();
  if (phase_ != Phase::Running) return CodecResult<void>::Failure(StateError());
  phase_ = Phase::Draining; ScheduleLocked();
  return CodecResult<void>::Success();
}
CodecResult<void> VideoEncoderState::Reset() {
  std::lock_guard lock(mutex_);
  if (phase_ == Phase::Resetting) return CodecResult<void>::Success();
  if (phase_ == Phase::Starting || phase_ == Phase::Failed || phase_ == Phase::Closed)
    return CodecResult<void>::Failure(phase_ == Phase::Closed ? StateError() : Error(CodecErrorCode::InvalidState, "Cannot reset this video encoder"));
  ++generation_; phase_ = Phase::Resetting; ScheduleLocked();
  return CodecResult<void>::Success();
}
void VideoEncoderState::Close() noexcept {
  std::lock_guard lock(mutex_);
  if (phase_ == Phase::Closed) return;
  phase_ = Phase::Closed; ++generation_;
  if (!cleaned_) ScheduleLocked();
}
VideoEncoderState::Retired VideoEncoderState::Clear() {
  Retired retired;
  retired.input.swap(input_); retired.output.swap(output_); retired.pending.swap(pending_);
  origin_.reset(); last_micro_pts_.reset(); last_input_pts_.reset(); last_dts_.reset(); format_.reset();
  waiting_credit_ = finish_sent_ = eos_queued_ = key_requested_ = false; needs_key_frame_ = true; b_run_ = 0;
  return retired;
}
void VideoEncoderState::Fail(CodecError error, std::uint64_t generation) {
  std::lock_guard lock(mutex_);
  if (generation != generation_ || phase_ == Phase::Closed || phase_ == Phase::Failed) return;
  failure_ = std::move(error); phase_ = Phase::Failed; wake_pending_ = true;
}
void VideoEncoderState::Notify() {
  bool admitted;
  {
    std::lock_guard lock(mutex_);
    admitted = wake_pending_ && phase_ != Phase::Closed && !cleaned_; wake_pending_ = false;
  }
  if (admitted && config_.wakeup) config_.wakeup();
}
void VideoEncoderState::Cleanup() {
  auto result = backend_ ? backend_->Close() : CodecResult<void>::Success();
  if (cleanup_failure_) result = CodecResult<void>::Failure(*cleanup_failure_);
  backend_.reset();
  for (auto& budget : {input_budget_, output_budget_}) {
    std::lock_guard lock(budget->mutex); budget->active = false;
  }
  std::function<void(CodecResult<void>)> completion;
  {
    Retired retired;
    {
      std::lock_guard lock(mutex_);
      retired = Clear(); cleaned_ = true; completion = std::move(config_.cleanup_completed);
    }
  }
  config_.wakeup = {}; factory_ = {};
#ifdef __EMSCRIPTEN__
  if (completion) completion(std::move(result));
#else
  worker_->Finish(std::move(result), std::move(completion));
#endif
}
void VideoEncoderState::Run(bool capacity) {
  std::uint64_t generation; Phase phase;
  {
    std::lock_guard lock(mutex_);
#ifdef __EMSCRIPTEN__
    scheduled_ = false;
#endif
    if (cleaned_) return;
    generation = generation_; phase = phase_;
    if (capacity && phase != Phase::Closed && phase != Phase::Failed) wake_pending_ = true;
  }
  try {
    if (phase == Phase::Starting || phase == Phase::Resetting) {
      if (backend_ && backend_generation_ != generation) {
        auto closed = backend_->Close(); backend_.reset();
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
          backend_ = factory_(); backend_generation_ = generation;
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
  } catch (const SessionFailure& failure) { Fail(failure.error, generation); }
  catch (const std::bad_alloc&) { Fail(Error(CodecErrorCode::ResourceExhausted, "Video encoder allocation failed"), generation); }
  catch (const std::exception& error) { Fail(Error(CodecErrorCode::PlatformFailure, error.what()), generation); }
  Notify();
  bool terminal;
  {
    std::lock_guard lock(mutex_);
    terminal = phase_ == Phase::Closed || phase_ == Phase::Failed;
    if (!terminal && (phase_ == Phase::Running || phase_ == Phase::Draining) && !eos_queued_ &&
        (!pending_.empty() || (!input_.empty() && !waiting_credit_) ||
            (phase_ == Phase::Draining && (!waiting_credit_ || finish_sent_)))) ScheduleLocked(2);
  }
  if (terminal) Cleanup();
}
void VideoEncoderState::Publish(VideoEncoderBackendOutput value, std::uint64_t generation) {
  std::lock_guard lock(mutex_);
  if (generation != generation_ || phase_ == Phase::Closed || phase_ == Phase::Failed) return;
  if (auto* format = std::get_if<VideoEncodeFormat>(&value)) {
    auto parsed = ReadVideoEncodeFormat(format->initialization_data, config_);
    if (!parsed.Succeeded()) throw SessionFailure{parsed.Error()};
    if (parsed.Value() != *format || (format_ && *format_ != *format)) Reject("Video format is invalid or changed during encoding");
    if (!format_) { format_ = *format; output_.push_back(std::move(*format)); wake_pending_ = true; }
  } else if (auto* packet = std::get_if<EncodedPacket>(&value)) {
    auto size = packet->data.AsBytes().size();
    const auto pts = packet->pts.Rescale(1000000);
    auto dts = packet->dts ? packet->dts->Rescale(1000000) : std::optional<MediaTimestamp>{};
    const bool prores = config_.codec == CodecId::ProRes;
    if (size > config_.max_packet_bytes)
      throw SessionFailure{Error(CodecErrorCode::ResourceExhausted, "Video output exceeds the reserved packet bound")};
    if (!format_ || !origin_ || !size || !pts || (packet->dts && !dts))
      Reject("Video output has missing configuration or invalid timing");
    if (prores) {
      if (dts && *dts != *pts) Reject("ProRes decode and presentation timestamps must match");
      dts = pts;
    }
    auto found = pending_.find(pts->ticks);
    if (found == pending_.end()) {
      std::string timing = std::to_string(pts->ticks) + "; pending";
      unsigned count = 0;
      for (const auto& [value, entry] : pending_) {
        timing += " " + std::to_string(value);
        if (++count == 4) break;
      }
      throw SessionFailure{Error(CodecErrorCode::InvalidData, "Video output PTS does not match an accepted picture", std::move(timing))};
    }
    auto picture = InspectVideoEncoderPacket(*packet, *format_);
    if (!picture.Succeeded()) throw SessionFailure{picture.Error()};
    if (picture.Value().headers) {
      auto normalized = NormalizeVideoEncoderPacket(packet->data.AsBytes(), 4, config_.max_packet_bytes, config_.codec, true);
      if (!normalized.Succeeded()) throw SessionFailure{normalized.Error()};
      packet->data = Own(std::move(normalized).Value()); size = packet->data.AsBytes().size();
    }
    if (needs_key_frame_ && !picture.Value().key_frame) Reject("Video stream must start with a random-access picture");
    needs_key_frame_ = false;
    b_run_ = picture.Value().b_frame ? b_run_ + 1 : 0;
    if (config_.max_b_frames && b_run_ > *config_.max_b_frames)
      throw SessionFailure{Error(CodecErrorCode::Unsupported, "Video encoder exceeded the requested B-picture bound")};
    if (dts) {
      if ((last_dts_ && dts->ticks <= *last_dts_) ||
          (*origin_ > 0 && dts->ticks > INT64_MAX - *origin_) ||
          (*origin_ < 0 && dts->ticks < INT64_MIN - *origin_)) Reject("Video DTS is nonincreasing or overflows the input timeline");
      last_dts_ = dts->ticks;
      packet->dts = MediaTimestamp{dts->ticks + *origin_, 1000000};
    }
    auto timing = std::move(found->second); pending_.erase(found);
    timing.credit->Shrink(size);
    struct Storage { std::shared_ptr<Credit> credit; BufferReference bytes; };
    auto storage = std::make_shared<Storage>(Storage{timing.credit, packet->data});
    packet->data = BufferReference(storage->bytes.AsBytes(), storage);
    packet->pts = timing.pts; packet->duration = timing.duration; packet->key_frame = picture.Value().key_frame;
    if (prores) packet->dts = timing.pts;
    output_.push_back(std::move(*packet)); wake_pending_ = true;
  } else {
    if (phase_ != Phase::Draining || !input_.empty() || !pending_.empty() || (origin_ && !finish_sent_))
      Reject("Video encoder ended before emitting every accepted picture");
    output_.push_back(EndOfStream{}); eos_queued_ = true; wake_pending_ = true;
  }
}
void VideoEncoderState::Process(std::uint64_t generation) {
  for (unsigned step = 0; step < 32; ++step) {
    bool key = false;
    {
      std::lock_guard lock(mutex_);
      if (generation != generation_ || (phase_ != Phase::Running && phase_ != Phase::Draining) || eos_queued_) return;
      key = std::exchange(key_requested_, false);
    }
    if (key) {
      auto result = backend_->RequestKeyFrame();
      if (!result.Succeeded()) { Fail(result.Error(), generation); return; }
    }
    auto polled = backend_->Poll();
    if (!polled.Succeeded()) { Fail(polled.Error(), generation); return; }
    if (polled.Value()) { Publish(std::move(*polled.Value()), generation); continue; }
    VideoFrame frame;
    std::shared_ptr<Credit> credit;
    bool submit = false, finish = false, empty = false;
    {
      std::lock_guard lock(mutex_);
      if (generation != generation_ || (phase_ != Phase::Running && phase_ != Phase::Draining)) return;
      waiting_credit_ = false;
      if (!input_.empty()) {
        credit = Reserve(output_budget_, config_.max_packet_bytes, config_.limits.output_items, config_.limits.output_bytes);
        waiting_credit_ = !credit;
        if (credit) {
          frame = input_.front(); frame.pts = {frame.pts.Rescale(1000000)->ticks - *origin_, 1000000};
          submit = true;
        }
      } else if (phase_ == Phase::Draining && !finish_sent_) {
        empty = !origin_; finish = !empty;
      }
    }
    if (empty) { Publish(VideoEncoderEnd{}, generation); return; }
    if (submit) {
      auto result = backend_->Submit(frame);
      if (!result.Succeeded()) { Fail(result.Error(), generation); return; }
      if (result.Value() == SubmitStatus::WouldBlock) {
        auto progress = backend_->FlushPending();
        if (!progress.Succeeded()) Fail(progress.Error(), generation);
        return;
      }
      std::lock_guard lock(mutex_);
      if (generation != generation_ || phase_ == Phase::Closed) return;
      const auto& input = input_.front();
      pending_.emplace(frame.pts.ticks, Timing{input.pts, input.duration, credit});
      credit->notify = true; input_.pop_front();
    } else if (finish) {
      auto result = backend_->Finish();
      if (!result.Succeeded()) { Fail(result.Error(), generation); return; }
      std::lock_guard lock(mutex_);
      if (generation == generation_ && result.Value() == SubmitStatus::Accepted) finish_sent_ = true;
      return;
    } else {
      auto result = backend_->FlushPending();
      if (!result.Succeeded()) Fail(result.Error(), generation);
      return;
    }
  }
  std::lock_guard lock(mutex_); ScheduleLocked();
}
} // namespace huxerui::media::detail
