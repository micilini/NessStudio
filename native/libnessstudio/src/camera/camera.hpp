#pragma once
#include "capture/capture.hpp"
#include <algorithm>

namespace nss::camera {
using Json = nlohmann::json;
inline std::atomic<uint64_t> live_sources{};
enum class TimestampMode { DeliveryQpc, StrictStreamQpc };
inline constexpr int64_t delivery_inactivity_hns=2*hns_per_second;
struct Plan {
  std::string device_id, subtype;
  uint32_t width{1280}, height{720}, fps_num{30}, fps_den{1};
  Json controls=Json::object();
  TimestampMode timestamp_mode{TimestampMode::DeliveryQpc};
};
// Establish three real deliveries before Ready without promising the advertised
// frame rate. Automatic exposure can reduce physical cadence below one quarter
// of nominal. Use the same inactivity boundary as the running source.
class StartupCadence final {
  int64_t nominal_interval_, previous_{};
  uint64_t observed_{}, consecutive_{};
public:
  StartupCadence(uint32_t numerator, uint32_t denominator)
      : nominal_interval_(frame_time(1,numerator,denominator)) {}
  bool observe(int64_t timestamp) noexcept {
    const auto delta = timestamp - previous_;
    consecutive_ = observed_ && delta > 0 && delta <= delivery_inactivity_hns ? consecutive_ + 1 : 0;
    previous_ = timestamp;
    ++observed_;
    return consecutive_ >= 2;
  }
  Json metrics() const {
    return {{"cameraWarmupFrames", observed_}, {"consecutiveIntervals", consecutive_},
            {"requiredIntervals", 2}, {"maximumIntervalHns", delivery_inactivity_hns},
            {"nominalIntervalHns",nominal_interval_},
            {"readinessPolicy","Three real deliveries within source inactivity limit; no nominal-FPS assumption"},
            {"lastCaptureHns", previous_}, {"timestampsChanged", false}};
  }
};
Json devices();
Plan plan(const Json &request);
Json plan_json(const Plan &value);
std::unique_ptr<capture::Pipeline> make_cpu(const Plan &source,
                                            const capture::Plan &output);
// DirectShow owns one physical source, continues acquiring through pauses,
// and publishes immutable bounded CPU BGRA leases to native consumers only.
class Source final {
  struct Impl;
  std::unique_ptr<Impl> impl_;

public:
  explicit Source(const Plan &request);
  ~Source();
  std::optional<capture::Frame> poll();
  Json metrics() const;
  void *wait_event() const noexcept;
  void close();
};
} // namespace nss::camera
