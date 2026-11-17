#pragma once
#include "capture.hpp"
#include <deque>
#include <condition_variable>
#include <future>
#include <thread>
#include <type_traits>
#include <windows.h>

namespace nss::capture::detail {
static_assert(std::is_trivially_copyable_v<GdiSource::Statistics>);
// Acquisition and conversion overlap through leased DIBs. Selected conversion
// retains acquisition order: a newer image may be beyond the CFR boundary and
// cannot replace an unexamined eligible image. The existing DIB pool bounds all
// queued/consumer/producer leases together; no extra image storage is allocated.
class GdiWorker final {
  struct WakeEvent {
    HANDLE value = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    WakeEvent() {
      if (!value)
        throw Error(NSS_E_UNAVAILABLE, NSS_STAGE_CAPTURE, HRESULT_FROM_WIN32(GetLastError()),
                    "CPU acquisition event creation failed");
    }
    ~WakeEvent() { CloseHandle(value); }
    void notify() const {
      if (!SetEvent(value))
        throw Error(NSS_E_UNAVAILABLE, NSS_STAGE_CAPTURE, HRESULT_FROM_WIN32(GetLastError()),
                    "CPU acquisition notification failed");
    }
  } wake_;
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  std::deque<GdiSource::Image> images_;
  std::atomic<bool> stopping_{};
  std::exception_ptr failure_;
  GdiSource::Statistics statistics_;
  uint64_t decimated_{};
  size_t limit_{1}, peak_{};
  bool preserve_order_{};
  std::thread thread_;

public:
  explicit GdiWorker(Plan plan)
      : limit_(plan.defer_conversion ? (std::min)(size_t{2}, size_t(plan.slots - 1)) : 1),
        preserve_order_(plan.defer_conversion) {
    std::promise<void> ready;
    auto initialized = ready.get_future();
    thread_ = std::thread([this, plan, signal = std::move(ready)]() mutable {
      try {
        (void)SetThreadDescription(GetCurrentThread(), L"Ness GDI acquisition");
        GdiSource source(plan);
        {
          std::lock_guard lock(mutex_);
          statistics_ = source.statistics();
        }
        signal.set_value();
        while (!stopping_) {
          if (preserve_order_) {
            std::unique_lock lock(mutex_);
            changed_.wait(lock, [&] { return stopping_ || images_.size() < limit_; });
            if (stopping_) break;
          }
          auto image = source.acquire();
          {
            std::lock_guard lock(mutex_);
            statistics_ = source.statistics();
            if (image) {
              if (!preserve_order_ && !images_.empty()) {
                images_.pop_front();
                ++decimated_;
              }
              images_.push_back(std::move(*image));
              peak_ = (std::max)(peak_, images_.size());
            }
          }
          if (image) wake_.notify();
          if (!image)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
      } catch (...) {
        {
          std::lock_guard lock(mutex_);
          failure_ = std::current_exception();
        }
        // The 10 ms owner fallback also observes failure if this best-effort
        // notification itself fails. The handle outlives the joined producer.
        (void)SetEvent(wake_.value);
        try {
          signal.set_exception(std::current_exception());
        } catch (...) {
        }
      }
    });
    try {
      initialized.get();
    } catch (...) {
      stopping_ = true;
      if (thread_.joinable())
        thread_.join();
      throw;
    }
  }
  GdiWorker(const GdiWorker &) = delete;
  GdiWorker &operator=(const GdiWorker &) = delete;
  ~GdiWorker() { close(); }
  void *wake_event() const noexcept { return wake_.value; }
  std::optional<GdiSource::Image> acquire() {
    std::unique_lock lock(mutex_);
    if (failure_)
      std::rethrow_exception(failure_);
    if (images_.empty())
      return {};
    auto result = std::move(images_.front());
    images_.pop_front();
    // Auto-reset notifications may coalesce while two acquisitions queue.
    // Re-arm for the remaining image before unlocking the queue predicate.
    if (!images_.empty()) wake_.notify();
    lock.unlock();
    changed_.notify_one();
    return result;
  }
  Json metrics() const {
    GdiSource::Statistics snapshot;
    uint64_t peak{}, decimated{};
    size_t queued{};
    bool joined{};
    {
      std::lock_guard lock(mutex_);
      snapshot = statistics_;
      peak = peak_;
      decimated = decimated_;
      queued = images_.size();
      joined = !thread_.joinable();
    }
    auto result = GdiSource::describe(snapshot);
    result["acquisitionQueueLimit"] = limit_;
    result["acquisitionQueuePeak"] = peak;
    result["acquisitionDeliveryPolicy"] = preserve_order_ ? "ordered-with-bounded-producer-wait" : "latest-image";
    result["acquisitionThreadsMax"] = 1;
    result["acquisitionQueued"] = queued;
    result["acquisitionDecimation"] = decimated;
    result["acquisitionThreadJoined"] = joined;
    result["acquisitionWakeEvent"] = true;
    result["acquisitionMetricsPolicy"] = "value snapshot each attempt; JSON only on diagnostics request outside queue lock";
    return result;
  }
  void close() {
    // Pair the predicate change with the same mutex as wait(); otherwise a
    // close between its predicate check and sleep can lose the notification.
    { std::lock_guard lock(mutex_); stopping_ = true; }
    changed_.notify_all();
    if (thread_.joinable())
      thread_.join();
    std::lock_guard lock(mutex_);
    images_.clear();
  }
};
} // namespace nss::capture::detail
