#pragma once
#include "capture/capture.hpp"

namespace nss::preview {
using Json = nlohmann::json;
inline std::atomic<uint64_t> live_channels{}, live_client_leases{},
    live_bytes{};
struct Frame {
  nss_preview_frame_v1 cpu{};
  nss_gpu_surface_v1 gpu{};
  std::shared_ptr<void> storage;
};
class Channel final {
  struct Impl;
  std::unique_ptr<Impl> impl_;

public:
  Channel(const nss_preview_config_v1 &request, uint64_t session,
          uint64_t source);
  ~Channel();
  std::shared_ptr<Frame> acquire(bool gpu);
  void offer(const capture::Frame &frame);
  void queue_gpu(const capture::Frame &frame);
  bool dispatch_gpu();
  void set_enabled(bool enabled);
  bool active() const;
  void pump();
  void detach() noexcept;
  void close();
  Json metrics() const;
  bool retired() const;
};
class Hub final {
  mutable std::mutex gate_;
  std::vector<std::shared_ptr<Channel>> channels_;
  uint64_t session_{}, source_{};
  bool ended_{};
  bool defer_gpu_{};

public:
  void bind(uint64_t session, uint64_t source);
  std::shared_ptr<Channel> attach(const nss_preview_config_v1 &request);
  void offer(const capture::Frame &frame);
  void defer_gpu(bool value) { defer_gpu_ = value; }
  bool active() const;
  bool dispatch_gpu();
  void pump();
  void close();
  Json metrics() const;
};
} // namespace nss::preview
