#include "session.hpp"
#include <chrono>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace nss {
static std::atomic<uint64_t> next_session{1};
static uint64_t session_id() {
    auto value = next_session.load();
    do { if (value == (std::numeric_limits<uint64_t>::max)()) throw std::overflow_error("Session identifier exhausted"); }
    while (!next_session.compare_exchange_weak(value, value + 1));
    return value;
}
static Fault fault(nss_result code, uint32_t stage, const char* message) { return {code, stage, 0, message}; }
Session::Session(std::shared_ptr<EngineBudget> budget, std::unique_ptr<Backend> backend, Json request)
    : budget_(std::move(budget)), backend_(std::move(backend)), requested_(std::move(request)), id_(session_id()) {
    if (live_sessions.fetch_add(1) >= 64) { --live_sessions; throw SessionBudgetExceeded{}; }
    backend_->bind_session(id_);
    // Interactive Stop includes durable journal/media flushes on the user's
    // storage volume. A transient disk stall must not cancel otherwise valid
    // media merely because Windows has not finished an accepted write. For
    // interactive sessions elapsed time is diagnostic, not proof of failure.
    // Keep the finite control/probe deadline for noninteractive sessions.
    stop_timeout_hns_ = requested_.value("interactive", false) ? 0 : 5 * hns_per_second;
    journal_path_ = Journal::recording_path(requested_, id_);
    worker_heartbeat_ = qpc_hns();
}
void Session::release_budget() noexcept { if (!budget_released_.exchange(true)) budget_->active_session = false; }
Session::~Session() { abandon(); if (worker_.joinable()) worker_.join(); if (checkpoint_writer_) checkpoint_writer_->close(); release_budget(); --live_sessions; }
void Session::start_worker() { worker_ = std::thread([this] { ++live_workers; run(); release_budget(); --live_workers; }); }
void Session::emit(uint64_t command, nss_result status, uint32_t kind, const Fault& error, bool ordinary) {
    if (count_ == events_.size() || sequence_ == (std::numeric_limits<uint64_t>::max)()) throw std::logic_error("Reserved event invariant violated");
    Event item{}; auto& value = item.value;
    value.size = sizeof(value); value.version = NSS_STRUCT_VERSION;
    value.sequence = ++sequence_; value.command_id = command; value.object_id = id_;
    value.active_time_hns = clock_.active_time(qpc_hns()); value.kind = kind; value.state = state_; value.status = status;
    value.stage = error.stage; value.hresult = error.hresult;
    const auto message = error.message.empty() ? (status == NSS_OK ? std::string{"Operation completed"} : std::string{"Operation did not complete successfully"}) : error.message;
    const auto length = (std::min)(message.size(), sizeof(value.message_utf8) - 1);
    std::memcpy(value.message_utf8, message.data(), length); item.ordinary = ordinary;
    events_[(head_ + count_) % events_.size()] = std::move(item); ++count_; changed_.notify_all();
}
void Session::latch(Fault error) {
    if (!error) return;
    backend_->cancel_media_completion();
    clock_.stop(qpc_hns());
    if (!first_error_) first_error_ = std::move(error);
    else if (secondary_count_ < secondary_.size()) secondary_[secondary_count_++] = std::move(error);
    else ++secondary_suppressed_;
    state_ = NSS_FAILED; result_ = first_error_->code; cleanup_pending_ = true;
    if (!fault_emitted_) { emit(0, result_, NSS_EVENT_FAULT, *first_error_); fault_emitted_ = true; }
}
nss_result Session::command(uint32_t kind, uint64_t& id, Fault& error) {
    id = 0; std::optional<std::stop_source> cancel;
    std::unique_lock lock(mutex_);
    auto reject = [&](nss_result code, const char* text) { error = fault(code, NSS_STAGE_CONTROL, text); return code; };
    if (closing_) return reject(NSS_E_STATE, "Session is being released");
    if (kind < NSS_COMMAND_PREPARE || kind > NSS_COMMAND_STOP) return reject(NSS_E_ARGUMENT, "Unknown session command");
    if (kind == NSS_COMMAND_STOP) {
        if (state_ == NSS_STOPPING || state_ == NSS_FINALIZING) { id = stop_id_; return NSS_ALREADY; }
        if (state_ == NSS_COMPLETED || state_ == NSS_FAILED) return NSS_ALREADY;
    } else {
        const uint32_t stable[] = {NSS_READY, NSS_RECORDING, NSS_PAUSED, NSS_RECORDING};
        const uint32_t transient[] = {NSS_PREPARING, NSS_STARTING, NSS_PAUSING, NSS_RESUMING};
        const uint32_t origin[] = {NSS_CREATED, NSS_READY, NSS_RECORDING, NSS_PAUSED};
        if (state_ == stable[kind - 1]) return NSS_ALREADY;
        if (state_ == transient[kind - 1]) return reject(NSS_E_BUSY, "This operation is pending");
        if (state_ != origin[kind - 1]) return reject(NSS_E_STATE, "Command is invalid in this state");
        if (ordinary_reservations_ >= 8) return reject(NSS_E_EVENT_BACKPRESSURE, "Consume pending results before submitting another command");
        if (kind == NSS_COMMAND_RESUME && !clock_.can_resume()) return reject(NSS_E_BACKPRESSURE, "Pause history budget exhausted");
    }
    if (next_command_ == (std::numeric_limits<uint64_t>::max)() || sequence_ > (std::numeric_limits<uint64_t>::max)() - ordinary_reservations_ - 4)
        return reject(NSS_E_STATE, "Session identifier/sequence budget exhausted");
    const auto now = qpc_hns();
    if (kind == NSS_COMMAND_PAUSE) clock_.pause(now);
    if (kind == NSS_COMMAND_STOP) clock_.stop(now);
    id = next_command_++; ++command_count_;
    if (kind == NSS_COMMAND_STOP) {
        stop_id_ = id; stop_requested_ = true; stop_started_ = now; state_ = NSS_STOPPING; cleanup_pending_ = true;
        if (operation_) cancel = operation_->cancel;
    } else {
        ++ordinary_reservations_; operation_.emplace(Operation{id, kind, {}});
        const uint32_t transient[] = {NSS_PREPARING, NSS_STARTING, NSS_PAUSING, NSS_RESUMING}; state_ = transient[kind - 1];
    }
    lock.unlock(); if (cancel) cancel->request_stop(); changed_.notify_all(); return NSS_ACCEPTED;
}
void Session::execute(Operation op) {
    Fault error;
    bool skip{};
    { std::lock_guard lock(mutex_); skip = stop_requested_ || closing_ || first_error_.has_value(); }
    if (!skip) switch (op.kind) {
    case NSS_COMMAND_PREPARE: error = backend_->prepare(op.cancel.get_token()); break;
    case NSS_COMMAND_START: {
        const auto epoch = qpc_hns();
        { std::lock_guard lock(mutex_); if (!stop_requested_ && !closing_) clock_.start(epoch); }
        if (!op.cancel.stop_requested()) error = backend_->start(op.cancel.get_token(), epoch, clock_);
        break;
    }
    case NSS_COMMAND_PAUSE: error = backend_->pause(op.cancel.get_token()); break;
    case NSS_COMMAND_RESUME: {
        { std::lock_guard lock(mutex_); if (!stop_requested_ && !closing_) clock_.resume(qpc_hns()); }
        if (!op.cancel.stop_requested()) error = backend_->resume(op.cancel.get_token(), clock_); break;
    }
    default: throw std::logic_error("Invalid queued command");
    }
    std::lock_guard lock(mutex_);
    if (op.kind == NSS_COMMAND_START && !error && !stop_requested_ && !closing_ && !backend_->first_content_admitted()) error = fault(NSS_E_STATE, NSS_STAGE_CAPTURE, "Start did not admit first content from its required sources");
    if (error && error.code != NSS_E_CANCELLED) latch(std::move(error));
    if (operation_->result_emitted) { operation_.reset(); return; }
    if (first_error_) emit(op.id, first_error_->code, NSS_EVENT_RESULT, *first_error_, true);
    else if (stop_requested_ || closing_ || op.cancel.stop_requested()) emit(op.id, NSS_E_CANCELLED, NSS_EVENT_RESULT, fault(NSS_E_CANCELLED, NSS_STAGE_CONTROL, "Operation interrupted by Stop/release"), true);
    else {
        const uint32_t stable[] = {NSS_READY, NSS_RECORDING, NSS_PAUSED, NSS_RECORDING}; state_ = stable[op.kind - 1];
        if (op.kind == NSS_COMMAND_START) started_ = true;
        result_ = NSS_OK; emit(op.id, NSS_OK, NSS_EVENT_RESULT, {}, true);
    }
    operation_.reset();
}
void Session::observe_stop_deadline() {
    if (!stop_timeout_hns_ || !stop_requested_ || stop_result_emitted_ || qpc_hns() - stop_started_ < stop_timeout_hns_) return;
    if (operation_ && !operation_->result_emitted) {
        emit(operation_->id, NSS_E_CANCELLED, NSS_EVENT_RESULT, fault(NSS_E_CANCELLED, NSS_STAGE_CONTROL, "Operation interrupted; native cleanup is still pending"), true);
        operation_->result_emitted = true;
    }
    latch(fault(NSS_E_TIMEOUT, NSS_STAGE_CONTROL, "Recording shutdown exceeded its deadline; cleanup remains pending and partial files were preserved"));
    emit(stop_id_, first_error_->code, NSS_EVENT_RESULT, *first_error_); stop_result_emitted_ = true;
}
void Session::stop_pipeline() {
    { std::lock_guard lock(mutex_); cleanup_pending_ = true; if (!stop_started_) stop_started_ = qpc_hns(); clock_.stop(qpc_hns()); }
    checkpoint(true);
    for (auto stage = ShutdownStage::Sources; stage != ShutdownStage::Done; stage = static_cast<ShutdownStage>(static_cast<int>(stage) + 1)) {
        { std::lock_guard lock(mutex_); shutdown_stage_ = stage; }
        if (stage == ShutdownStage::Output) { std::lock_guard lock(mutex_); if (!first_error_) state_ = NSS_FINALIZING; }
        worker_heartbeat_ = qpc_hns();
        checkpoint(true);
        for (;;) {
            const auto progress = backend_->shutdown(stage);
            {
                std::lock_guard lock(mutex_);
                if (progress.error) latch(progress.error);
                if (stop_timeout_hns_ && qpc_hns() - stop_started_ >= stop_timeout_hns_ && !stop_result_emitted_) {
                    latch(fault(NSS_E_TIMEOUT, NSS_STAGE_CONTROL, "Recording shutdown exceeded its deadline; cleanup remains pending and partial files were preserved"));
                    if (stop_id_) { emit(stop_id_, first_error_->code, NSS_EVENT_RESULT, *first_error_); stop_result_emitted_ = true; }
                }
            }
            if (!progress.pending) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    {
        std::lock_guard lock(mutex_);
        if (!first_error_) { state_ = NSS_COMPLETED; result_ = started_ && !closing_ ? NSS_OK : NSS_E_CANCELLED; }
    }
    checkpoint(true, true);
    if (checkpoint_writer_) checkpoint_writer_->close();
    {
        std::lock_guard lock(mutex_);
        // Cleanup includes the durable writer. A concurrent snapshot must
        // not invite CloseAsync or another session before that owner joins.
        cleanup_pending_ = false; shutdown_stage_ = ShutdownStage::Done; release_budget();
        if (stop_id_ && !stop_result_emitted_) { emit(stop_id_, result_, NSS_EVENT_RESULT, first_error_ ? *first_error_ : Fault{}); stop_result_emitted_ = true; }
        changed_.notify_all();
    }
}
void Session::checkpoint(bool force, bool cleanup_done) {
    const auto now = qpc_hns();
    if (journal_path_.empty() || (!force && now - last_checkpoint_ < hns_per_second)) return;
    last_checkpoint_ = now;
    try {
        if (!journal_) {
            auto journal = std::make_unique<Journal>(journal_path_, id_, "recording");
            auto writer = std::make_unique<CheckpointWriter>([sink = journal.get()](Json value) {
                sink->store(std::move(value));
            });
            // Publish ownership together; snapshots read it under this mutex.
            std::lock_guard lock(mutex_);
            journal_ = std::move(journal);
            checkpoint_writer_ = std::move(writer);
        }
        // Recovery checkpoints never persisted runtime metrics. Avoid building
        // and copying that potentially large diagnostic tree just to erase it.
        auto value = snapshot(false);
        // The final durable document describes the completed cleanup, while
        // the observable live state remains pending until the writer joins.
        if (cleanup_done) value["cleanupPending"] = false;
        if (force) checkpoint_writer_->store_barrier(std::move(value));
        else checkpoint_writer_->store_periodic(std::move(value));
    } catch (const JournalError& error) {
        std::lock_guard lock(mutex_);
        latch({NSS_E_IO, NSS_STAGE_STORAGE, error.native, error.what()});
    }
    const auto finished = qpc_hns();
    std::lock_guard lock(mutex_);
    checkpoint_timings_[checkpoint_count_ % checkpoint_timings_.size()] =
        {now, finished, state_, force};
    ++checkpoint_count_;
    max_checkpoint_hns_ = (std::max)(max_checkpoint_hns_, finished - now);
}
void Session::fail_operation(Fault error) { std::lock_guard lock(mutex_); latch(std::move(error)); }
void Session::run() noexcept {
    try {
        checkpoint(true);
        for (;;) {
            std::optional<Operation> op; bool stop{}, tick{};
            {
                std::unique_lock lock(mutex_);
                if (!(backend_->self_paced() && (state_ == NSS_RECORDING || state_ == NSS_PAUSED)))
                    changed_.wait_for(lock, std::chrono::milliseconds(1), [&] { return operation_ || stop_requested_ || closing_ || first_error_; });
                op = operation_; stop = stop_requested_ || closing_ || first_error_.has_value();
                tick = state_ == NSS_RECORDING || state_ == NSS_PAUSED;
            }
            worker_heartbeat_ = qpc_hns();
            if (op) { execute(*op); checkpoint(true); }
            else if (stop) { stop_pipeline(); break; }
            else if (tick) { auto error = backend_->tick(clock_); if (error) fail_operation(std::move(error)); checkpoint(); }
        }
    } catch (const std::exception& error) {
        try {
            std::lock_guard lock(mutex_); latch(fault(NSS_E_INTERNAL, NSS_STAGE_CONTROL, error.what()));
            if (operation_) { if (!operation_->result_emitted) emit(operation_->id, first_error_->code, NSS_EVENT_RESULT, *first_error_, true); operation_.reset(); }
        } catch (...) { }
        try { stop_pipeline(); } catch (...) { std::lock_guard lock(mutex_); cleanup_pending_ = true; }
    } catch (...) { std::lock_guard lock(mutex_); state_ = NSS_FAILED; result_ = NSS_E_INTERNAL; cleanup_pending_ = true; }
}
nss_result Session::poll(uint32_t timeout, nss_event_v1& output, Fault& error) {
    std::unique_lock lock(mutex_);
    if (timeout > 1000) { error = fault(NSS_E_ARGUMENT, NSS_STAGE_CONTROL, "Poll timeout must be between 0 and 1000 ms"); return error.code; }
    if (poll_busy_) { error = fault(NSS_E_BUSY, NSS_STAGE_CONTROL, "Only one event consumer is supported"); return error.code; }
    poll_busy_ = true;
    struct reset { bool& value; ~reset() { value = false; } } done{poll_busy_};
    observe_stop_deadline();
    uint32_t wait_ms = timeout;
    if (stop_timeout_hns_ && stop_requested_ && !stop_result_emitted_) {
        const auto remaining = stop_timeout_hns_ - (qpc_hns() - stop_started_);
        wait_ms = (std::min)(wait_ms, static_cast<uint32_t>((std::max)(int64_t{0}, (remaining + 9999) / 10000)));
    }
    changed_.wait_for(lock, std::chrono::milliseconds(wait_ms), [&] { return count_ || poll_interrupted_ || closing_; });
    observe_stop_deadline();
    if (poll_interrupted_ || closing_) { poll_interrupted_ = false; return NSS_E_CANCELLED; }
    if (!count_) return NSS_TIMEOUT;
    auto& entry = events_[head_]; output = entry.value;
    if (entry.ordinary) --ordinary_reservations_;
    head_ = (head_ + 1) % events_.size(); --count_; return NSS_OK;
}
void Session::interrupt() noexcept { std::lock_guard lock(mutex_); poll_interrupted_ = true; changed_.notify_all(); }
void Session::abandon() noexcept {
    std::optional<std::stop_source> cancel;
    { std::lock_guard lock(mutex_); if (closing_) return; closing_ = true; backend_->cancel_media_completion(); if (operation_) cancel = operation_->cancel; }
    if (cancel) cancel->request_stop(); changed_.notify_all();
}
std::shared_ptr<preview::Hub> Session::preview(uint64_t source) { std::lock_guard lock(mutex_); return backend_->preview(source); }
uint64_t Session::update_scene(const Json& scene) {
    std::lock_guard lock(mutex_);
    if (state_ >= NSS_STOPPING || closing_ || stop_requested_) throw std::invalid_argument("Scene updates require a live session");
    return backend_->update_scene(scene);
}
Json Session::snapshot(bool include_metrics) const {
    Json result;
    std::array<CheckpointTiming, 128> checkpoint_samples{};
    uint64_t checkpoint_count{};
    int64_t max_checkpoint_hns{};
    CheckpointWriter *writer{};
    {
    std::lock_guard lock(mutex_);
    auto diagnostic = [&](const Fault& value) { return Json{{"code", value.code}, {"stage", value.stage}, {"hresult", value.hresult}, {"sessionId", id_}, {"message", value.message}}; };
    Json secondary = Json::array(); for (size_t i = 0; i < secondary_count_; ++i) secondary.push_back(diagnostic(secondary_[i]));
    const auto now = qpc_hns();
    const auto journal_text = journal_path_.u8string();
    const char *shutdown_names[] = {"Sources", "Inflight", "EncoderEos", "Packets", "Output", "Resources", "Done"};
    result = {{"version", 1}, {"objectId", id_}, {"state", state_}, {"lastSequence", sequence_}, {"pendingCommandId", stop_requested_ && !stop_result_emitted_ ? stop_id_ : operation_ ? operation_->id : 0},
        {"operationResult", result_}, {"mediaComplete", state_ == NSS_COMPLETED && !first_error_ && backend_->media_complete()}, {"cleanupPending", cleanup_pending_},
        {"firstError", first_error_ ? diagnostic(*first_error_) : Json(nullptr)}, {"secondaryErrors", secondary}, {"secondaryErrorsSuppressed", secondary_suppressed_},
        {"requested", requested_}, {"shutdownStage", shutdown_stage_ ? Json(shutdown_names[static_cast<size_t>(*shutdown_stage_)]) : Json(nullptr)},
        {"journalPath", std::string(reinterpret_cast<const char*>(journal_text.data()), journal_text.size())},
        {"watchdog", {{"diagnosticOnly", stop_timeout_hns_ == 0},
            {"workerHeartbeatAgeHns", (std::max)(int64_t{}, now - worker_heartbeat_.load())},
            {"stopElapsedMs", stop_started_ ? (std::max)(int64_t{}, now - stop_started_) / 10000 : 0},
            {"slowStop", stop_started_ && !stop_result_emitted_ && now - stop_started_ >= 60 * hns_per_second},
            {"slowStopThresholdMs", 60000}, {"stopDeadlineMs", stop_timeout_hns_ / 10000},
            {"timeoutPolicy", stop_timeout_hns_ == 0
                ? "Interactive shutdown waits for accepted writes and native cleanup; elapsed time alone does not invalidate media"
                : "Noninteractive shutdown has a finite control deadline; retain resources until native cleanup actually completes"}}},
        {"activeTimeHns", clock_.active_time(now)}, {"wallTimeHns", clock_.wall_time(now)},
        {"acceptedCommands", command_count_}, {"eventsQueued", count_}, {"ordinaryResultReservations", ordinary_reservations_}};
    if (include_metrics) {
        checkpoint_samples = checkpoint_timings_;
        checkpoint_count = checkpoint_count_;
        max_checkpoint_hns = max_checkpoint_hns_;
        writer = checkpoint_writer_.get();
    }
    }
    // Backend publications have their own synchronization and live as long
    // as this Session. Copying or formatting them must not hold the control
    // mutex needed by every recording tick, Stop and event consumer. Control
    // fields above describe one instant; backend metrics are observational.
    result["effectiveStages"] = backend_->stages();
    result["assets"] = backend_->assets();
    if (include_metrics) {
        result["metrics"] = backend_->metrics();
        Json timings = Json::array();
        const auto count = (std::min)(checkpoint_count, uint64_t(checkpoint_samples.size()));
        const auto first = checkpoint_count >= checkpoint_samples.size()
                               ? checkpoint_count % checkpoint_samples.size() : 0;
        for (uint64_t i = 0; i < count; ++i) {
            const auto &sample = checkpoint_samples[(first + i) % checkpoint_samples.size()];
            timings.push_back({{"beginQpcHns", sample.begin_hns}, {"endQpcHns", sample.end_hns},
                {"workerDurationMs", double(sample.end_hns - sample.begin_hns) / 10000},
                {"state", sample.state}, {"forced", sample.forced}});
        }
        result["journalDiagnostics"] = {{"checkpointCount", checkpoint_count},
            {"maxWorkerDurationMs", double(max_checkpoint_hns) / 10000},
            {"sampleLimit", checkpoint_samples.size()}, {"recentCheckpoints", std::move(timings)},
            {"scope", "Snapshot preparation and periodic handoff or mandatory durable barrier on the session worker"},
            {"durableWriter", writer ? writer->metrics() : Json(nullptr)}};
    }
    return result;
}
}
