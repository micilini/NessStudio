#pragma once
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <vector>

namespace nss {
inline std::atomic<uint64_t> live_pool_bytes{}, live_frame_leases{};
struct PoolStats { uint64_t bytes{}, in_use{}, peak{}, acquired{}, released{}, exhausted{}; };
class FramePool final {
    struct Slot { std::unique_ptr<std::byte[]> data; bool borrowed{}; };
    struct State {
        std::mutex mutex; std::vector<Slot> slots; size_t bytes; PoolStats stats;
        State(size_t count, size_t size) : slots(count), bytes(size) {
            for (auto& slot : slots) slot.data = std::make_unique<std::byte[]>(bytes);
            stats.bytes = count * size; live_pool_bytes += stats.bytes;
        }
        ~State() { live_pool_bytes -= stats.bytes; }
    };
    std::shared_ptr<State> state_;
public:
    class Lease final {
        std::shared_ptr<State> state_; size_t index_{}; bool sealed_{};
        friend class FramePool;
        Lease(std::shared_ptr<State> state, size_t index) : state_(std::move(state)), index_(index) { ++live_frame_leases; }
    public:
        Lease() = default;
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
        Lease(Lease&& other) noexcept : state_(std::move(other.state_)), index_(other.index_), sealed_(other.sealed_) {}
        Lease& operator=(Lease&& other) noexcept { if (this != &other) { release(); state_ = std::move(other.state_); index_ = other.index_; sealed_ = other.sealed_; } return *this; }
        ~Lease() { release(); }
        explicit operator bool() const noexcept { return static_cast<bool>(state_); }
        std::span<std::byte> writable() { if (!state_ || sealed_) throw std::logic_error("Frame lease is not writable"); return {state_->slots[index_].data.get(), state_->bytes}; }
        void seal() { if (!state_ || sealed_) throw std::logic_error("Frame already sealed or released"); sealed_ = true; }
        std::span<const std::byte> view() const { if (!state_ || !sealed_) throw std::logic_error("Frame is not published"); return {state_->slots[index_].data.get(), state_->bytes}; }
        void release() noexcept {
            if (!state_) return;
            { std::lock_guard lock(state_->mutex); state_->slots[index_].borrowed = false; --state_->stats.in_use; ++state_->stats.released; }
            state_.reset(); --live_frame_leases;
        }
    };
    FramePool(size_t count, size_t bytes) {
        if (!count || count > 8 || !bytes || bytes > 64 * 1024 * 1024 / count) throw std::length_error("Frame pool exceeds slot/byte budget");
        state_ = std::make_shared<State>(count, bytes);
    }
    static size_t nv12_bytes(uint32_t width, uint32_t height, uint32_t stride) {
        if (!width || !height || (width & 1) || (height & 1) || stride < width || stride > 16384 || height > 16384) throw std::invalid_argument("Invalid CPU NV12 geometry");
        return static_cast<size_t>(stride) * height * 3 / 2;
    }
    std::optional<Lease> acquire() {
        std::lock_guard lock(state_->mutex);
        for (size_t i = 0; i < state_->slots.size(); ++i) if (!state_->slots[i].borrowed) {
            state_->slots[i].borrowed = true; ++state_->stats.in_use; ++state_->stats.acquired;
            state_->stats.peak = (std::max)(state_->stats.peak, state_->stats.in_use); return Lease(state_, i);
        }
        ++state_->stats.exhausted; return {};
    }
    PoolStats stats() const { std::lock_guard lock(state_->mutex); return state_->stats; }
};
enum class PressurePolicy { DropOldest, DropNewest, Fail };
struct QueueStats { uint64_t count{}, bytes{}, duration_hns{}, peak_count{}, peak_bytes{}, peak_duration_hns{}, pushed{}, popped{}, dropped_old{}, dropped_new{}, rejected{}; };
template<class T> class BoundedQueue final {
    struct Entry { T value; uint64_t bytes, duration; };
    std::vector<std::optional<Entry>> ring_;
    size_t head_{}; uint64_t max_bytes_, max_duration_;
    PressurePolicy policy_; QueueStats stats_; std::optional<int64_t> saturated_since_;
    mutable std::mutex mutex_; bool closed_{};
    static size_t checked_count(size_t count) { if (!count || count > 4096) throw std::length_error("Invalid queue slot budget"); return count; }
    void remove_head(bool dropped) {
        const auto& entry = *ring_[head_]; stats_.bytes -= entry.bytes; stats_.duration_hns -= entry.duration;
        ring_[head_].reset(); head_ = (head_ + 1) % ring_.size(); --stats_.count;
        if (dropped) ++stats_.dropped_old; else ++stats_.popped;
    }
public:
    BoundedQueue(size_t count, uint64_t bytes, uint64_t duration, PressurePolicy policy) : ring_(checked_count(count)), max_bytes_(bytes), max_duration_(duration), policy_(policy) {
        if (!bytes || !duration) throw std::length_error("Invalid queue budget");
    }
    bool push(T item, uint64_t bytes, uint64_t duration, int64_t now) {
        std::lock_guard lock(mutex_);
        if (closed_ || bytes > max_bytes_ || duration > max_duration_) { ++stats_.rejected; return false; }
        auto fits = [&] { return stats_.count < ring_.size() && bytes <= max_bytes_ - stats_.bytes && duration <= max_duration_ - stats_.duration_hns; };
        if (!fits()) {
            if (!saturated_since_) saturated_since_ = now;
            if (policy_ == PressurePolicy::Fail) { ++stats_.rejected; return false; }
            if (policy_ == PressurePolicy::DropNewest) { ++stats_.dropped_new; return false; }
            while (!fits()) remove_head(true);
        } else saturated_since_.reset();
        const auto tail = (head_ + static_cast<size_t>(stats_.count)) % ring_.size();
        ring_[tail].emplace(Entry{std::move(item), bytes, duration}); ++stats_.count; ++stats_.pushed;
        stats_.bytes += bytes; stats_.duration_hns += duration;
        stats_.peak_count = (std::max)(stats_.peak_count, stats_.count); stats_.peak_bytes = (std::max)(stats_.peak_bytes, stats_.bytes);
        stats_.peak_duration_hns = (std::max)(stats_.peak_duration_hns, stats_.duration_hns); return true;
    }
    std::optional<T> pop() {
        std::lock_guard lock(mutex_); if (!stats_.count) return {};
        T item = std::move(ring_[head_]->value); remove_head(false); return item;
    }
    void close() { std::lock_guard lock(mutex_); closed_ = true; }
    bool saturation_exceeded(int64_t now) const { std::lock_guard lock(mutex_); return saturated_since_ && now >= *saturated_since_ && now - *saturated_since_ >= 20000000; }
    QueueStats stats() const { std::lock_guard lock(mutex_); return stats_; }
};
// GPU backends must implement completion before recycling resources; COM lifetime alone is insufficient.
struct Completion {
    virtual bool ready() const noexcept = 0;
    virtual ~Completion() = default;
};
enum class CpuPixelFormat : uint32_t { Nv12, Bgra8Opaque };
struct CpuFrameMetadata {
    uint64_t session_id{}, source_id{}, generation{}, frame_id{};
    int64_t acquisition_begin_hns{}, acquisition_end_hns{}, pts_hns{}, duration_hns{};
    uint32_t width{}, height{}, stride{};
    CpuPixelFormat format{CpuPixelFormat::Nv12};
    uint64_t required_bytes() const {
        if (!width || !height || width > 16384 || height > 16384 || acquisition_begin_hns < 0 || acquisition_end_hns < acquisition_begin_hns || duration_hns < 0) throw std::invalid_argument("Invalid CPU frame metadata");
        if (format == CpuPixelFormat::Nv12) return FramePool::nv12_bytes(width, height, stride);
        if (format != CpuPixelFormat::Bgra8Opaque || stride < static_cast<uint64_t>(width) * 4 || stride > 65536) throw std::invalid_argument("Invalid CPU BGRA layout");
        return static_cast<uint64_t>(stride) * height;
    }
};
struct CpuFrame {
    FramePool::Lease lease; CpuFrameMetadata metadata;
    std::span<const std::byte> pixels() const { const auto view = lease.view(); if (view.size() < metadata.required_bytes()) throw std::length_error("CPU frame buffer is too short"); return view; }
};
}
