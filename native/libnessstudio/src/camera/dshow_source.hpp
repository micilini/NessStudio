#pragma once
#include "dshow_sink.hpp"
#include <condition_variable>
#include <array>
#include <thread>

namespace nss::camera::detail {
struct CpuSurface final : capture::Surface {
  FramePool::Lease storage;
  explicit CpuSurface(FramePool::Lease bytes):storage(std::move(bytes)) {}
  std::span<const std::byte> cpu_pixels() const override {return storage.view();}
};
class SampleReceiver final : public SinkReceiver {
  struct Input {
    FramePool::Lease bytes;
    size_t size{};
    uint64_t id{},generation{};
    int64_t timestamp{},arrival{};
  };
  Plan plan_;
  VideoFormat format_;
  FramePool raw_,decoded_;
  mutable std::mutex gate_;
  std::condition_variable changed_;
  std::array<std::optional<Input>,4> queue_{};
  size_t head_{},count_{},peak_{};
  std::optional<capture::Frame> latest_;
  std::thread worker_;
  HANDLE ready_{};
  HRESULT failure_{S_OK};
  std::string failure_message_;
  bool closing_{};
  int64_t stream_zero_{},first_timestamp_{},last_timestamp_{},first_arrival_{},last_arrival_{},max_delay_{};
  int64_t first_raw_timestamp_{},last_raw_timestamp_{},maximum_raw_future_{};
  uint64_t uncorrelated_raw_samples_{};
  uint64_t callbacks_{},samples_{},frames_{},raw_bytes_{},raw_drops_{},decoded_drops_{},mailbox_drops_{},discontinuities_{},generation_{1};
  uint64_t warmup_rejected_{};
  HRESULT warmup_error_{S_OK};
  size_t warmup_error_bytes_{};
  std::array<std::array<int64_t,5>,32> timestamps_{};
  size_t timestamp_count_{};
  void fail_locked(HRESULT,const char *) noexcept;
  void process() noexcept;
public:
  SampleReceiver(Plan,const VideoFormat &);
  ~SampleReceiver() override;
  HRESULT receive(IMediaSample *) noexcept override;
  HRESULT run(REFERENCE_TIME) noexcept override;
  void flush() noexcept override;
  void end_stream() noexcept override;
  void fail(HRESULT,const char *) noexcept;
  void close() noexcept;
  std::optional<capture::Frame> poll();
  Json metrics() const;
  HANDLE ready_event() const noexcept {return ready_;}
};
} // namespace nss::camera::detail
