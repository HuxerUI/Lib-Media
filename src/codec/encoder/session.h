#pragma once

#include "backend.h"
#include "execution/executor.h"
#include <array>
#include <deque>
#include <map>
#include <mutex>

namespace huxerui::media::detail {
class AudioEncoderState : public std::enable_shared_from_this<AudioEncoderState> {
public:
  static CodecResult<std::shared_ptr<AudioEncoderState>> Create(AudioEncoderConfig config, AudioEncoderFactory factory = MakeAudioEncoderBackend);
  CodecResult<SubmitStatus> Submit(const AudioFrame& frame);
  CodecResult<std::optional<AudioEncodeOutput>> Read();
  CodecResult<void> Finish();
  CodecResult<void> Reset();
  void Close() noexcept;
private:
  enum class Phase { Starting, Running, Draining, Ended, Resetting, Failed, Closed };
  struct Budget {
    std::mutex mutex;
    std::size_t items = 0, bytes = 0;
    bool active = true;
    std::weak_ptr<AudioEncoderState> session;
  };
  struct Credit {
    std::shared_ptr<Budget> budget;
    std::size_t bytes;
    bool notify = false;
    ~Credit();
    void Shrink(std::size_t size);
  };
  struct Input { BufferReference data; std::size_t offset = 0; };
  struct Retired {
    std::deque<Input> input;
    std::deque<AudioEncodeOutput> output;
    std::deque<std::shared_ptr<Credit>> reservations;
  };
  AudioEncoderState(AudioEncoderConfig config, AudioEncoderFactory factory);
  std::shared_ptr<Credit> Reserve(const std::shared_ptr<Budget>& budget, std::size_t bytes, std::size_t max_items, std::size_t max_bytes);
  void ScheduleLocked(unsigned delay = 0);
  void Run(bool capacity = false);
  void Process(std::uint64_t generation);
  void Publish(AudioEncoderBackendOutput value, std::uint64_t generation);
  void End(AudioEncoderEnd end);
  void Fail(CodecError error, std::uint64_t generation);
  void Notify();
  void Cleanup();
  Retired Clear();
  CodecError StateError() const;
#ifdef __EMSCRIPTEN__
  void CreditAvailable();
  bool scheduled_ = false;
#else
  std::shared_ptr<MediaWorker> worker_;
#endif
  std::mutex mutex_;
  AudioEncoderConfig config_;
  AudioEncoderFactory factory_;
  std::unique_ptr<AudioEncoderBackend> backend_;
  std::shared_ptr<Budget> input_budget_ = std::make_shared<Budget>(), output_budget_ = std::make_shared<Budget>();
  Phase phase_ = Phase::Starting;
  std::uint64_t generation_ = 0, backend_generation_ = 0;
  bool wake_pending_ = false, cleaned_ = false, waiting_credit_ = false, finish_sent_ = false, eos_queued_ = false, extra_reserved_ = false;
  std::optional<CodecError> failure_, cleanup_failure_;
  std::deque<Input> input_;
  std::deque<AudioEncodeOutput> output_;
  std::deque<std::shared_ptr<Credit>> reservations_;
  std::array<std::byte, 4096> assembly_{};
  std::uint32_t assembly_samples_ = 0;
  std::uint64_t input_samples_ = 0, submitted_samples_ = 0, encoded_samples_ = 0;
  std::optional<MediaTimestamp> input_start_, last_packet_pts_;
  std::optional<AudioEncodeFormat> format_;
};
class VideoEncoderState : public std::enable_shared_from_this<VideoEncoderState> {
public:
  static CodecResult<std::shared_ptr<VideoEncoderState>> Create(VideoEncoderConfig config, VideoEncoderFactory factory = MakeVideoEncoderBackend);
  CodecResult<SubmitStatus> Submit(const VideoFrame& frame);
  CodecResult<std::optional<VideoEncodeOutput>> Read();
  CodecResult<void> RequestKeyFrame();
  CodecResult<void> Finish();
  CodecResult<void> Reset();
  void Close() noexcept;
private:
  enum class Phase { Starting, Running, Draining, Ended, Resetting, Failed, Closed };
  struct Budget {
    std::mutex mutex;
    std::size_t items = 0, bytes = 0;
    bool active = true;
    std::weak_ptr<VideoEncoderState> session;
  };
  struct Credit {
    std::shared_ptr<Budget> budget;
    std::size_t bytes = 0;
    bool notify = false;
    ~Credit();
    void Shrink(std::size_t size);
  };
  struct Timing {
    MediaTimestamp pts;
    std::optional<MediaTimestamp> duration;
    std::shared_ptr<Credit> credit;
  };
  struct Retired {
    std::deque<VideoFrame> input;
    std::deque<VideoEncodeOutput> output;
    std::map<std::int64_t, Timing> pending;
  };
  VideoEncoderState(VideoEncoderConfig config, VideoEncoderFactory factory, std::size_t frame_bytes);
  std::shared_ptr<Credit> Reserve(const std::shared_ptr<Budget>& budget, std::size_t bytes, std::size_t max_items, std::size_t max_bytes);
  void ScheduleLocked(unsigned delay = 0);
  void Run(bool capacity = false);
  void Process(std::uint64_t generation);
  void Publish(VideoEncoderBackendOutput value, std::uint64_t generation);
  void Fail(CodecError error, std::uint64_t generation);
  void Notify();
  void Cleanup();
  Retired Clear();
  CodecError StateError() const;
#ifdef __EMSCRIPTEN__
  void CreditAvailable();
  bool scheduled_ = false;
#else
  std::shared_ptr<MediaWorker> worker_;
#endif
  std::mutex mutex_;
  VideoEncoderConfig config_;
  VideoEncoderFactory factory_;
  std::size_t frame_bytes_;
  std::unique_ptr<VideoEncoderBackend> backend_;
  std::shared_ptr<Budget> input_budget_ = std::make_shared<Budget>(), output_budget_ = std::make_shared<Budget>();
  Phase phase_ = Phase::Starting;
  std::uint64_t generation_ = 0, backend_generation_ = 0;
  bool wake_pending_ = false, cleaned_ = false, waiting_credit_ = false, finish_sent_ = false;
  bool eos_queued_ = false, key_requested_ = false, needs_key_frame_ = true;
  std::uint32_t b_run_ = 0;
  std::optional<CodecError> failure_, cleanup_failure_;
  std::deque<VideoFrame> input_;
  std::deque<VideoEncodeOutput> output_;
  std::map<std::int64_t, Timing> pending_;
  std::optional<std::int64_t> origin_, last_micro_pts_, last_dts_;
  std::optional<MediaTimestamp> last_input_pts_;
  std::optional<VideoEncodeFormat> format_;
};
} // namespace huxerui::media::detail
