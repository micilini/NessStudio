#pragma once
#include "json.hpp"
#include <atomic>
#include <condition_variable>
#include <exception>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>

namespace nss {
inline std::atomic<uint64_t> live_checkpoint_workers{};

// One durable writer, one in-flight value and at most one pending snapshot.
// Periodic snapshots may replace pending periodic state. A mandatory barrier
// serializes producers until its own snapshot has been durably stored.
class CheckpointWriter final {
    struct Pending { uint64_t sequence; nlohmann::json value; };
    std::function<void(nlohmann::json)> store_;
    mutable std::mutex mutex_;
    std::mutex producer_mutex_;
    std::condition_variable changed_;
    std::thread worker_;
    std::optional<Pending> pending_;
    std::exception_ptr failure_;
    bool closing_{}, active_{};
    uint64_t submitted_{}, completed_sequence_{}, completed_count_{}, coalesced_{};
    int64_t total_write_hns_{}, maximum_write_hns_{};
    void run() noexcept;
    void submit(nlohmann::json, bool barrier);
public:
    explicit CheckpointWriter(std::function<void(nlohmann::json)> store);
    ~CheckpointWriter();
    CheckpointWriter(const CheckpointWriter&) = delete;
    CheckpointWriter& operator=(const CheckpointWriter&) = delete;
    void store_periodic(nlohmann::json value) { submit(std::move(value), false); }
    void store_barrier(nlohmann::json value) { submit(std::move(value), true); }
    // Close drains the latest pending state and joins the owned worker. Errors
    // remain observable on submission/barriers, never thrown by destruction.
    void close();
    nlohmann::json metrics() const;
};
} // namespace nss
