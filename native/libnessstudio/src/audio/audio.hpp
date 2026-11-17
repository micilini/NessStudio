#pragma once
#include "runtime/session.hpp"
#include <memory>

namespace nss::audio {
inline std::atomic<uint64_t> live_sources{};
// Enumeration opens no streams and creates no accelerated device.
Json devices(uint32_t kind);
Json validate(const Json &request, const std::string &video_path);
class Session final {
  struct Impl;
  std::unique_ptr<Impl> impl_;

public:
  explicit Session(Json plan);
  ~Session();
  void prepare();
  void start(SessionClock &clock);
  void check();
  void stop();
  void finalize(int64_t useful_hns);
  Json snapshot() const;
};
} // namespace nss::audio
