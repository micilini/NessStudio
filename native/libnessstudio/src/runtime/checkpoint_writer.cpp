#include "checkpoint_writer.hpp"
#include "time.hpp"
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace nss {
CheckpointWriter::CheckpointWriter(std::function<void(nlohmann::json)> store)
    : store_(std::move(store)) {
    if (!store_) throw std::invalid_argument("Checkpoint writer requires a durable sink");
    ++live_checkpoint_workers;
    try { worker_ = std::thread([this] { run(); --live_checkpoint_workers; }); }
    catch (...) { --live_checkpoint_workers; throw; }
}
CheckpointWriter::~CheckpointWriter() { close(); }
void CheckpointWriter::submit(nlohmann::json value, bool barrier) {
    // A barrier cannot be replaced by another producer's periodic snapshot.
    // The recording Session is normally the only producer.
    std::lock_guard producer(producer_mutex_);
    std::unique_lock lock(mutex_);
    if (failure_) std::rethrow_exception(failure_);
    if (closing_) throw std::logic_error("Checkpoint writer is closed");
    if (submitted_ == (std::numeric_limits<uint64_t>::max)())
        throw std::overflow_error("Checkpoint submission sequence exhausted");
    const auto sequence = ++submitted_;
    if (pending_) ++coalesced_;
    pending_ = Pending{sequence, std::move(value)};
    changed_.notify_one();
    if (barrier) {
        changed_.wait(lock, [&] { return failure_ || completed_sequence_ >= sequence; });
        if (failure_) std::rethrow_exception(failure_);
    }
}
void CheckpointWriter::run() noexcept {
    try {
        for (;;) {
            std::optional<Pending> current;
            {
                std::unique_lock lock(mutex_);
                changed_.wait(lock, [&] { return closing_ || pending_.has_value(); });
                if (!pending_) return;
                current = std::move(pending_);
                pending_.reset();
                active_ = true;
            }
            const auto begin = qpc_hns();
            store_(std::move(current->value));
            const auto elapsed = qpc_hns() - begin;
            {
                std::lock_guard lock(mutex_);
                completed_sequence_ = current->sequence;
                ++completed_count_;
                total_write_hns_ += elapsed;
                maximum_write_hns_ = (std::max)(maximum_write_hns_, elapsed);
                active_ = false;
            }
            changed_.notify_all();
        }
    } catch (...) {
        {
            std::lock_guard lock(mutex_);
            failure_ = std::current_exception();
            pending_.reset();
            active_ = false;
        }
        changed_.notify_all();
    }
}
void CheckpointWriter::close() {
    std::lock_guard producer(producer_mutex_);
    {
        std::lock_guard lock(mutex_);
        closing_ = true;
    }
    changed_.notify_all();
    if (worker_.joinable()) worker_.join();
}
nlohmann::json CheckpointWriter::metrics() const {
    std::lock_guard lock(mutex_);
    return {{"submitted", submitted_}, {"durablyStored", completed_count_},
        {"completedSubmissionSequence", completed_sequence_}, {"coalescedPeriodicSnapshots", coalesced_},
        {"pendingSnapshotLimit", 1}, {"pending", pending_.has_value()}, {"active", active_},
        {"failed", bool(failure_)}, {"closed", closing_},
        {"totalDurableWriteMs", double(total_write_hns_) / 10000},
        {"maximumDurableWriteMs", double(maximum_write_hns_) / 10000},
        {"durability", "Flushed atomic journal; mandatory transition and finalization barriers are not coalesced"}};
}
} // namespace nss
