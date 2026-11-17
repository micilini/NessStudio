// Test-only source/encoder/sink. Never linked or copied into the shipped engine.
#include "runtime/session.hpp"
#include <chrono>
#include <cstring>

namespace nss {
namespace {
using Frame = CpuFrame;
class Synthetic final : public Backend {
    mutable std::mutex mutex_;
    std::unique_ptr<FramePool> pool_;
    BoundedQueue<Frame> capture_{3, 4096, 20000000, PressurePolicy::DropOldest};
    BoundedQueue<Frame> encoder_{3, 4096, 20000000, PressurePolicy::DropNewest};
    BoundedQueue<uint64_t> packets_{16, 16 * 1024 * 1024, 20000000, PressurePolicy::Fail};
    std::optional<std::thread::id> owner_;
    uint32_t prepare_delay_{}, drain_delay_{}, fault_stage_{}, consumer_ticks_{1}, fault_delay_{}, blocked_prepare_{};
    std::atomic<bool> fault_entered_{};
    std::atomic<bool> prepare_entered_{};
    std::string fault_at_;
    bool secondary_output_{}, empty_start_{}, first_{};
    int64_t drain_begin_{};
    uint64_t captured_{}, eligible_{}, encoded_{}, output_{}, paused_rejected_{}, ticks_{};
    std::vector<uint32_t> shutdown_;
    PoolStats last_pool_;
    void own_context() {
        if (!owner_) owner_ = std::this_thread::get_id();
        if (*owner_ != std::this_thread::get_id()) throw std::logic_error("Processing context accessed by a foreign thread");
    }
    Fault injection(const char* point) const {
        if (fault_stage_ && fault_at_ == point) return {NSS_E_IO, fault_stage_, static_cast<int32_t>(0x80004005U), std::string{"Injected test-only failure at "} + point};
        return {};
    }
    Fault produce(SessionClock& clock, int64_t now) {
        ++captured_;
        if (!clock.eligible(now, now)) { ++paused_rejected_; return {}; }
        auto lease = pool_->acquire(); if (!lease) return {};
        const auto pts = clock.map(now); if (!pts) return {};
        std::fill(lease->writable().begin(), lease->writable().end(), std::byte{0x31}); lease->seal();
        ++eligible_;
        CpuFrameMetadata metadata{}; metadata.session_id = session_id(); metadata.source_id = metadata.generation = 1; metadata.frame_id = captured_; metadata.acquisition_begin_hns = metadata.acquisition_end_hns = now;
        metadata.pts_hns = *pts; metadata.duration_hns = 333333; metadata.width = metadata.height = metadata.stride = 16;
        capture_.push(Frame{std::move(*lease), metadata}, 384, 333333, now);
        if (++ticks_ % consumer_ticks_) return {};
        if (auto frame = capture_.pop()) encoder_.push(std::move(*frame), 384, 333333, now);
        if (auto frame = encoder_.pop()) {
            if (frame->pixels().front() != std::byte{0x31}) throw std::logic_error("Published frame was overwritten");
            ++encoded_;
            if (!packets_.push(frame->metadata.frame_id, 16, 333333, now)) return {NSS_E_BACKPRESSURE, NSS_STAGE_OUTPUT, 0, "Synthetic packet output budget exceeded"};
        }
        if (packets_.pop()) ++output_;
        return {};
    }
public:
    explicit Synthetic(const Json& options) {
        auto number = [&](const char* name, uint32_t maximum, uint32_t fallback = 0) {
            if (!options.contains(name)) return fallback;
            if (!options[name].is_number_unsigned() || options[name].get<uint64_t>() > maximum) throw std::invalid_argument("Invalid synthetic fixture integer");
            return options[name].get<uint32_t>();
        };
        for (const auto& property : options.items()) if (property.key() != "prepareDelayMs" && property.key() != "drainDelayMs" && property.key() != "faultStage" && property.key() != "faultAt" && property.key() != "secondaryOutputFailure" && property.key() != "emptyStart" && property.key() != "consumerTicks" && property.key() != "faultDelayMs" && property.key() != "blockedPrepareMs") throw std::invalid_argument("Unknown synthetic fixture option");
        prepare_delay_ = number("prepareDelayMs", 3000); drain_delay_ = number("drainDelayMs", 6500); fault_stage_ = number("faultStage", 11);
        consumer_ticks_ = number("consumerTicks", 128, 1); if (!consumer_ticks_) throw std::invalid_argument("Consumer ticks must be positive");
        fault_delay_ = number("faultDelayMs", 100);
        blocked_prepare_ = number("blockedPrepareMs", 6500);
        fault_at_ = options.value("faultAt", std::string{"tick"});
        if (fault_at_ != "prepare" && fault_at_ != "start" && fault_at_ != "tick" && fault_at_ != "output") throw std::invalid_argument("Unknown synthetic failure point");
        secondary_output_ = options.value("secondaryOutputFailure", false); empty_start_ = options.value("emptyStart", false); shutdown_.reserve(6);
    }
    Fault prepare(std::stop_token stop) override {
        own_context();
        prepare_entered_ = true;
        if (blocked_prepare_) std::this_thread::sleep_for(std::chrono::milliseconds(blocked_prepare_));
        std::mutex mutex; std::condition_variable_any wait; std::unique_lock lock(mutex);
        wait.wait_for(lock, stop, std::chrono::milliseconds(prepare_delay_), [] { return false; });
        if (stop.stop_requested()) return {NSS_E_CANCELLED, NSS_STAGE_CONTROL, 0, "Synthetic prepare interrupted"};
        if (auto error = injection("prepare")) return error;
        std::lock_guard guard(mutex_); pool_ = std::make_unique<FramePool>(8, FramePool::nv12_bytes(16, 16, 16)); return {};
    }
    Fault start(std::stop_token stop, int64_t epoch, SessionClock& clock) override {
        own_context(); if (stop.stop_requested()) return {NSS_E_CANCELLED, NSS_STAGE_CONTROL, 0, "Synthetic start interrupted"};
        if (auto error = injection("start")) return error;
        std::lock_guard guard(mutex_);
        if (!empty_start_ && clock.eligible(epoch, epoch)) { auto error = produce(clock, epoch); first_ = !error && eligible_ > 0; return error; }
        return {};
    }
    Fault pause(std::stop_token) override { own_context(); return {}; }
    Fault resume(std::stop_token, SessionClock&) override { own_context(); return {}; }
    Fault tick(SessionClock& clock) override {
        own_context(); if (auto error = injection("tick")) { fault_entered_ = true; std::this_thread::sleep_for(std::chrono::milliseconds(fault_delay_)); return error; }
        std::lock_guard guard(mutex_); return produce(clock, qpc_hns());
    }
    ShutdownProgress shutdown(ShutdownStage stage) override {
        own_context(); std::lock_guard guard(mutex_);
        if (stage == ShutdownStage::Inflight && drain_delay_) {
            if (!drain_begin_) drain_begin_ = qpc_hns();
            if (qpc_hns() - drain_begin_ < static_cast<int64_t>(drain_delay_) * 10000) return {true, {}};
        }
        shutdown_.push_back(static_cast<uint32_t>(stage));
        if (stage == ShutdownStage::Sources) capture_.close();
        if (stage == ShutdownStage::EncoderEos) { while (capture_.pop()) { } encoder_.close(); while (encoder_.pop()) { } }
        if (stage == ShutdownStage::Packets) { packets_.close(); while (packets_.pop()) ++output_; }
        if (stage == ShutdownStage::Output) {
            if (auto error = injection("output")) return {false, error};
            if (secondary_output_) return {false, {NSS_E_IO, NSS_STAGE_OUTPUT, 0, "Injected secondary output cleanup failure"}};
        }
        if (stage == ShutdownStage::Resources && pool_) { last_pool_ = pool_->stats(); pool_.reset(); }
        return {};
    }
    bool first_content_admitted() const noexcept override { return first_; }
    Json metrics() const override {
        std::lock_guard guard(mutex_); const auto pool = pool_ ? pool_->stats() : last_pool_; const auto capture = capture_.stats();
        return {{"testOnly", true}, {"captured", captured_}, {"eligible", eligible_}, {"encodedSynthetic", encoded_}, {"outputSynthetic", output_}, {"pausedRejected", paused_rejected_},
            {"captureDroppedOld", capture.dropped_old}, {"capturePeak", capture.peak_count}, {"poolBytes", pool.bytes}, {"poolPeak", pool.peak}, {"faultEntered", fault_entered_.load()}, {"prepareEntered", prepare_entered_.load()},
            {"poolAcquired", pool.acquired}, {"poolReleased", pool.released}, {"shutdownOrder", shutdown_}, {"contextOwnerSerialized", true}};
    }
};
}
std::unique_ptr<Backend> make_backend(const Json& request, Fault& error) {
    if (!request.contains("testFixture") || !request["testFixture"].is_object()) { error = {NSS_E_UNAVAILABLE, NSS_STAGE_PLAN, 0, "This isolated test DLL accepts only an explicit synthetic testFixture request"}; return {}; }
    for (const auto& item : request.items()) if (item.key() != "version" && item.key() != "testFixture") throw std::invalid_argument("Unknown synthetic session request property");
    return std::make_unique<Synthetic>(request["testFixture"]);
}
bool test_backend_build() noexcept { return true; }
}
