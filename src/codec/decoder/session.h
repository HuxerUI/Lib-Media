#pragma once

#include "backend.h"
#include "execution/executor.h"
#include <deque>
#include <mutex>

namespace huxerui::media::detail {
template<class Output> class DecoderState : public std::enable_shared_from_this<DecoderState<Output>> {
public:
  static CodecResult<std::shared_ptr<DecoderState>> Create(DecoderConfig config, BackendFactory factory = MakeDecoderBackend);
  CodecResult<SubmitStatus> Submit(const EncodedPacket& packet);
  CodecResult<std::optional<Output>> Read();
  CodecResult<void> Finish();
  CodecResult<void> Reset();
  void Close() noexcept;
private:
  enum class Phase { Starting, Running, Resetting, Draining, Ended, Failed, Closed };
  struct Budget {
    std::mutex mutex;
    std::size_t items = 0, bytes = 0;
    bool active = true;
    std::weak_ptr<DecoderState> session;
  };
  struct Credit {
    Credit(std::shared_ptr<Budget> owner, std::size_t size) : budget(std::move(owner)), bytes(size) {}
    std::shared_ptr<Budget> budget;
    std::size_t bytes;
    bool notify = false;
    ~Credit();
  };
  struct Input { EncodedPacket packet; std::shared_ptr<Credit> credit; };
  struct RetiredQueues {
    std::deque<EncodedPacket> input;
    std::deque<Input> inflight;
    std::deque<Output> output;
    std::vector<DecodedFrame> reordered;
  };
  DecoderState(DecoderConfig config, std::size_t frame_bytes, BackendFactory factory);
  void ScheduleLocked(unsigned delay = 0);
  void Run(bool credit_available = false);
  void Process(std::uint64_t generation);
  void Fail(CodecError error, std::uint64_t generation);
  void Cleanup();
  void Notify();
  void Publish(DecodedFrame frame, std::uint64_t generation);
  void PublishReordered(bool all);
  RetiredQueues ClearQueues();
#ifdef __EMSCRIPTEN__
  void CreditAvailable();
  bool scheduled_ = false;
#else
  std::shared_ptr<MediaWorker> worker_;
#endif
  std::shared_ptr<Credit> Reserve();
  std::shared_ptr<Credit> ReserveInput(std::size_t bytes);
  CodecError StateError() const;
  std::mutex mutex_;
  DecoderConfig config_;
  QueueLimits limits_;
  std::size_t frame_bytes_;
  BackendFactory factory_;
  std::shared_ptr<Budget> budget_ = std::make_shared<Budget>();
  std::shared_ptr<Budget> input_budget_ = std::make_shared<Budget>();
  Phase phase_ = Phase::Starting;
  std::uint64_t generation_ = 0, backend_generation_ = 0;
  bool waiting_credit_ = false;
  bool wake_pending_ = false, cleaned_ = false, finish_sent_ = false, eos_queued_ = false, needs_key_ = true;
  std::optional<CodecError> failure_, cleanup_failure_;
  std::unique_ptr<DecoderBackend> backend_;
  std::deque<EncodedPacket> input_;
  std::deque<Input> inflight_;
  std::deque<Output> output_;
  std::vector<DecodedFrame> reordered_;
  std::optional<MediaTimestamp> last_pts_;
  std::optional<std::variant<AudioFormat, VideoFormat>> last_format_;
  std::function<void()> wakeup_;
  std::function<void(CodecResult<void>)> cleanup_completed_;
};
extern template class DecoderState<AudioDecodeOutput>;
extern template class DecoderState<VideoDecodeOutput>;
} // namespace huxerui::media::detail
