#include "session.h"
#include "copy.h"

#include <algorithm>
#include <new>
#include <system_error>

namespace huxerui::media::detail {
template<class Output>
DecoderState<Output>::DecoderState(DecoderConfig config, std::size_t bytes, BackendFactory factory)
    : config_(std::move(config)), limits_(std::visit([](const auto& c) { return c.limits; }, config_)),
      frame_bytes_(bytes), factory_(std::move(factory)) {
  std::visit([this](auto& c) {
    wakeup_ = std::move(c.wakeup);
    cleanup_completed_ = std::move(c.cleanup_completed);
  }, config_);
}
template<class Output>
CodecResult<std::shared_ptr<DecoderState<Output>>> DecoderState<Output>::Create(DecoderConfig config, BackendFactory factory) {
  using R = CodecResult<std::shared_ptr<DecoderState>>;
  const auto valid = ValidateConfig(config);
  if (!valid.Succeeded()) return R::Failure(valid.Error());
  try {
    auto state = std::shared_ptr<DecoderState>(new DecoderState(std::move(config), valid.Value(), std::move(factory)));
    state->budget_->session = state;
    state->input_budget_->session = state;
#ifndef __EMSCRIPTEN__
    state->worker_ = MediaWorker::Start([state](bool credit_available) { state->Run(credit_available); });
#endif
    std::lock_guard lock(state->mutex_);
    state->ScheduleLocked();
    return R::Success(std::move(state));
  } catch (const std::bad_alloc&) {
    return R::Failure(Error(CodecErrorCode::ResourceExhausted, "Cannot allocate decoder control state"));
  } catch (const std::system_error& error) {
    return R::Failure(Error(CodecErrorCode::ResourceExhausted, "Cannot start decoder worker", error.what()));
  }
}
template<class Output> DecoderState<Output>::Credit::~Credit() {
  bool notify_session;
  {
    std::lock_guard lock(budget->mutex);
    --budget->items;
    budget->bytes -= bytes;
    notify_session = notify && budget->active;
  }
  if (auto session = budget->session.lock(); notify_session && session) {
#ifdef __EMSCRIPTEN__
    DispatchMedia([session] { session->CreditAvailable(); });
#else
    session->worker_->Signal();
#endif
  }
}
#ifdef __EMSCRIPTEN__
template<class Output> void DecoderState<Output>::CreditAvailable() {
  std::lock_guard lock(mutex_);
  if (phase_ == Phase::Closed || phase_ == Phase::Failed) return;
  wake_pending_ = true;
  ScheduleLocked();
}
#endif
template<class Output> std::shared_ptr<typename DecoderState<Output>::Credit> DecoderState<Output>::Reserve() {
  std::lock_guard lock(budget_->mutex);
  if (budget_->items >= limits_.output_items || budget_->bytes > limits_.output_bytes - frame_bytes_) return {};
  auto credit = std::make_shared<Credit>(budget_, frame_bytes_);
  ++budget_->items;
  budget_->bytes += frame_bytes_;
  return credit;
}
template<class Output> std::shared_ptr<typename DecoderState<Output>::Credit> DecoderState<Output>::ReserveInput(std::size_t bytes) {
  std::lock_guard lock(input_budget_->mutex);
  if (input_budget_->items >= limits_.input_items || bytes > limits_.input_bytes - input_budget_->bytes) return {};
  auto credit = std::make_shared<Credit>(input_budget_, bytes);
  ++input_budget_->items;
  input_budget_->bytes += bytes;
  return credit;
}
template<class Output> void DecoderState<Output>::ScheduleLocked(unsigned delay) {
#ifdef __EMSCRIPTEN__
  if (scheduled_) return;
  auto self = this->shared_from_this();
  DispatchMedia([self] { self->Run(); }, delay);
  scheduled_ = true;
#else
  worker_->Schedule(delay);
#endif
}
template<class Output> CodecError DecoderState<Output>::StateError() const {
  if (phase_ == Phase::Closed) return Error(CodecErrorCode::Closed, "Decoder is closed");
  return failure_.value_or(Error(CodecErrorCode::InvalidState, "Decoder cannot accept this operation in its current state"));
}
template<class Output> CodecResult<SubmitStatus> DecoderState<Output>::Submit(const EncodedPacket& packet) {
  using R = CodecResult<SubmitStatus>;
  std::lock_guard lock(mutex_);
  if (phase_ == Phase::Starting || phase_ == Phase::Resetting) return R::Success(SubmitStatus::WouldBlock);
  if (phase_ != Phase::Running) return R::Failure(StateError());
  const auto size = packet.data.AsBytes().size();
  if (size > limits_.input_bytes) return R::Failure(Error(CodecErrorCode::InvalidData, "Packet exceeds input byte limit"));
  const auto valid = ValidatePacket(packet, config_, needs_key_);
  if (!valid.Succeeded()) return R::Failure(valid.Error());
  try {
    auto credit = ReserveInput(size);
    if (!credit) return R::Success(SubmitStatus::WouldBlock);
    struct Storage { Bytes bytes; std::shared_ptr<Credit> credit; };
    auto owner = std::make_shared<Storage>(Storage{Bytes(packet.data.AsBytes().begin(), packet.data.AsBytes().end()), credit});
    auto copy = packet;
    copy.data = BufferReference(owner->bytes, owner);
    ScheduleLocked();
    input_.push_back(std::move(copy));
    credit->notify = true;
    needs_key_ = false;
    return R::Success(SubmitStatus::Accepted);
  } catch (const std::bad_alloc&) {
    return R::Failure(Error(CodecErrorCode::ResourceExhausted, "Cannot copy codec input"));
  }
}
template<class Output> CodecResult<std::optional<Output>> DecoderState<Output>::Read() {
  using R = CodecResult<std::optional<Output>>;
  std::lock_guard lock(mutex_);
  if (phase_ == Phase::Closed || phase_ == Phase::Failed) return R::Failure(StateError());
  if (phase_ == Phase::Resetting || output_.empty()) return R::Success(std::nullopt);
  auto value = std::move(output_.front());
  output_.pop_front();
  if (std::holds_alternative<EndOfStream>(value)) phase_ = Phase::Ended;
  ScheduleLocked();
  return R::Success(std::move(value));
}
template<class Output> CodecResult<void> DecoderState<Output>::Finish() {
  std::lock_guard lock(mutex_);
  if (phase_ == Phase::Closed) return CodecResult<void>::Failure(StateError());
  if (phase_ == Phase::Draining || phase_ == Phase::Ended) return CodecResult<void>::Success();
  if (phase_ != Phase::Running) return CodecResult<void>::Failure(Error(CodecErrorCode::InvalidState, "Decoder is not running"));
  ScheduleLocked();
  phase_ = Phase::Draining;
  return CodecResult<void>::Success();
}
template<class Output> CodecResult<void> DecoderState<Output>::Reset() {
  std::lock_guard lock(mutex_);
  if (phase_ == Phase::Closed) return CodecResult<void>::Failure(StateError());
  if (phase_ == Phase::Resetting) return CodecResult<void>::Success();
  if (phase_ == Phase::Starting || phase_ == Phase::Failed)
    return CodecResult<void>::Failure(Error(CodecErrorCode::InvalidState, "Decoder cannot reset before initialization or after failure"));
  ScheduleLocked();
  ++generation_;
  phase_ = Phase::Resetting;
  return CodecResult<void>::Success();
}
template<class Output> void DecoderState<Output>::Close() noexcept {
  std::lock_guard lock(mutex_);
  if (phase_ == Phase::Closed) return;
  phase_ = Phase::Closed;
  ++generation_;
  if (!cleaned_) ScheduleLocked();
}
template<class Output> typename DecoderState<Output>::RetiredQueues DecoderState<Output>::ClearQueues() {
  RetiredQueues retired;
  input_.swap(retired.input);
  inflight_.swap(retired.inflight);
  output_.swap(retired.output);
  reordered_.swap(retired.reordered);
  last_pts_.reset();
  last_format_.reset();
  finish_sent_ = eos_queued_ = waiting_credit_ = false;
  needs_key_ = true;
  return retired;
}
template<class Output> void DecoderState<Output>::Fail(CodecError error, std::uint64_t generation) {
  std::lock_guard lock(mutex_);
  if (generation != generation_ || phase_ == Phase::Closed || phase_ == Phase::Failed) return;
  failure_ = std::move(error);
  phase_ = Phase::Failed;
  wake_pending_ = true;
}
template<class Output> void DecoderState<Output>::Notify() {
  bool admitted = false;
  {
    std::lock_guard lock(mutex_);
    admitted = wake_pending_ && phase_ != Phase::Closed && !cleaned_;
    wake_pending_ = false;
  }
  if (admitted && wakeup_) wakeup_();
}
template<class Output> void DecoderState<Output>::Cleanup() {
  auto result = backend_ ? backend_->Close() : CodecResult<void>::Success();
  if (cleanup_failure_) result = CodecResult<void>::Failure(*cleanup_failure_);
  backend_.reset();
  {
    std::lock_guard lock(budget_->mutex);
    budget_->active = false;
  }
  {
    std::lock_guard lock(input_budget_->mutex);
    input_budget_->active = false;
  }
  std::visit([](auto& config) { Bytes{}.swap(config.initialization_data); }, config_);
  std::function<void(CodecResult<void>)> completion;
  {
    RetiredQueues retired;
    {
      std::lock_guard lock(mutex_);
      retired = ClearQueues();
      cleaned_ = true;
      completion = std::move(cleanup_completed_);
    }
  }
  wakeup_ = {};
  factory_ = {};
#ifdef __EMSCRIPTEN__
  if (completion) completion(std::move(result));
#else
  worker_->Finish(std::move(result), std::move(completion));
#endif
}
template<class Output> void DecoderState<Output>::Run(bool credit_available) {
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
    if (credit_available && phase != Phase::Closed && phase != Phase::Failed) wake_pending_ = true;
  }
  try {
    if (phase == Phase::Starting || phase == Phase::Resetting) {
      if (phase == Phase::Resetting && backend_ && backend_generation_ != generation) {
        const auto closed = backend_->Close();
        backend_.reset();
        if (!closed.Succeeded()) { cleanup_failure_ = closed.Error(); Fail(closed.Error(), generation); }
      }
      {
        RetiredQueues retired;
        {
          std::lock_guard lock(mutex_);
          if (generation != generation_ || phase_ == Phase::Failed || phase_ == Phase::Closed) phase = Phase::Closed;
          else if (!backend_) retired = ClearQueues();
        }
      }
      if (phase != Phase::Closed) {
        if (!backend_) {
          backend_ = factory_();
          backend_generation_ = generation;
          const auto opened = backend_->Open(config_);
          if (!opened.Succeeded()) Fail(opened.Error(), generation);
        }
        const auto ready = backend_->Ready();
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
  } catch (const std::bad_alloc&) {
    Fail(Error(CodecErrorCode::ResourceExhausted, "Codec allocation failed"), generation);
  } catch (const std::exception& error) {
    Fail(Error(CodecErrorCode::PlatformFailure, error.what()), generation);
  }
  Notify();
  bool terminal;
  {
    std::lock_guard lock(mutex_);
    terminal = phase_ == Phase::Closed || phase_ == Phase::Failed;
    if (!terminal && ((!input_.empty() && !waiting_credit_) || !inflight_.empty() || (phase_ == Phase::Draining && input_.empty() && !eos_queued_))) ScheduleLocked(2);
  }
  if (terminal) Cleanup();
}
template<class Output> void DecoderState<Output>::PublishReordered(bool all) {
  constexpr bool video = std::is_same_v<Output, VideoDecodeOutput>;
  const auto delay = video && !all ? 16u : 0u;
  while (reordered_.size() > delay) {
    auto earliest = std::min_element(reordered_.begin(), reordered_.end(), [](const auto& a, const auto& b) {
      const auto at = std::visit([](const auto& f) { return f.pts; }, a);
      const auto bt = std::visit([](const auto& f) { return f.pts; }, b);
      return at.Compare(bt).value() < 0;
    });
    auto frame = std::move(*earliest);
    reordered_.erase(earliest);
    const auto pts = std::visit([](const auto& f) { return f.pts; }, frame);
    if (last_pts_ && pts.Compare(*last_pts_).value() < 0) {
      failure_ = Error(CodecErrorCode::Unsupported, "Platform frame reordering exceeds the supported bound");
      phase_ = Phase::Failed;
      wake_pending_ = true;
      return;
    }
    last_pts_ = pts;
    using Frame = std::conditional_t<video, VideoFrame, AudioFrame>;
    auto& value = std::get<Frame>(frame);
    std::variant<AudioFormat, VideoFormat> format = value.format;
    if (!last_format_ || *last_format_ != format) {
      output_.push_back(value.format);
      last_format_ = format;
    }
    output_.push_back(std::move(value));
    wake_pending_ = true;
  }
}
template<class Output> void DecoderState<Output>::Publish(DecodedFrame frame, std::uint64_t generation) {
  const bool valid = std::visit([](const auto& f) { return f.Validate(); }, frame);
  std::lock_guard lock(mutex_);
  if (generation != generation_ || phase_ == Phase::Closed || phase_ == Phase::Failed) return;
  if (!valid || FrameBytes(frame) > frame_bytes_ || inflight_.empty()) {
    failure_ = Error(CodecErrorCode::InvalidData, "Platform output is invalid or exceeds reserved frame capacity");
    phase_ = Phase::Failed;
    wake_pending_ = true;
    return;
  }
  const auto timescale = backend_->PresentationTimescale();
  const auto platform_pts = std::visit([timescale](const auto& f) { return f.pts.Rescale(timescale); }, frame);
  auto position = std::find_if(inflight_.begin(), inflight_.end(), [&](const auto& item) {
    return platform_pts && item.packet.pts.Rescale(timescale) == platform_pts;
  });
  if (position == inflight_.end()) {
    failure_ = Error(CodecErrorCode::InvalidData, "Platform output timing does not match an accepted access unit");
    phase_ = Phase::Failed;
    wake_pending_ = true;
    return;
  }
  auto input = std::move(*position);
  inflight_.erase(position);
  std::visit([&](auto& value) {
    value.pts = input.packet.pts;
    if constexpr (std::is_same_v<std::decay_t<decltype(value)>, VideoFrame>) {
      if (input.packet.duration) value.duration = input.packet.duration;
    }
  }, frame);
  input.credit->notify = true;
  RetainFrame(frame, input.credit);
  reordered_.push_back(std::move(frame));
  wake_pending_ = true;
  PublishReordered(false);
}
template<class Output> void DecoderState<Output>::Process(std::uint64_t generation) {
  for (unsigned step = 0; step < 32; ++step) {
    {
      std::lock_guard lock(mutex_);
      if (generation != generation_ || (phase_ != Phase::Running && phase_ != Phase::Draining)) return;
    }
    auto polled = backend_->Poll();
    if (!polled.Succeeded()) { Fail(polled.Error(), generation); return; }
    if (polled.Value()) {
      if (auto* frame = std::get_if<DecodedFrame>(&*polled.Value())) Publish(std::move(*frame), generation);
      else {
        std::lock_guard lock(mutex_);
        if (generation != generation_ || phase_ != Phase::Draining) return;
        if (!inflight_.empty()) {
          failure_ = Error(CodecErrorCode::InvalidData, "Platform decoder ended before producing all accepted access units");
          phase_ = Phase::Failed;
          wake_pending_ = true;
          return;
        }
        PublishReordered(true);
        if (phase_ == Phase::Failed) return;
        inflight_.clear();
        output_.push_back(EndOfStream{});
        finish_sent_ = false;
        eos_queued_ = true;
        wake_pending_ = true;
        return;
      }
      continue;
    }
    std::optional<EncodedPacket> packet;
    std::shared_ptr<Credit> credit;
    bool finish = false;
    {
      std::lock_guard lock(mutex_);
      if (generation != generation_ || (phase_ != Phase::Running && phase_ != Phase::Draining)) return;
      if (!input_.empty()) {
        credit = Reserve();
        waiting_credit_ = !credit;
        if (credit) packet = input_.front();
      } else finish = phase_ == Phase::Draining && !finish_sent_ && !eos_queued_;
    }
    if (packet) {
      auto result = backend_->Submit(*packet);
      if (!result.Succeeded()) { Fail(result.Error(), generation); return; }
      if (result.Value() == SubmitStatus::WouldBlock) return;
      std::lock_guard lock(mutex_);
      if (generation != generation_) return;
      auto timing = std::move(input_.front());
      timing.data = {};
      inflight_.push_back({std::move(timing), std::move(credit)});
      input_.pop_front();
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
template class DecoderState<AudioDecodeOutput>;
template class DecoderState<VideoDecodeOutput>;
} // namespace huxerui::media::detail
