#pragma once
#include "runtime/session.hpp"

namespace nss::jobs {
inline std::atomic<uint64_t> live_jobs{}, workers{}, children{};
class Job final {
  struct Impl;
  std::unique_ptr<Impl> impl_;

public:
  Job(Json request, std::shared_ptr<std::atomic<bool>> budget);
  ~Job();
  nss_result start(uint64_t &id);
  nss_result cancel(uint64_t &id);
  Json snapshot() const;
  nss_result poll(uint32_t timeout, nss_event_v1 &out);
  void interrupt() noexcept;
  void abandon() noexcept;
};
} // namespace nss::jobs
