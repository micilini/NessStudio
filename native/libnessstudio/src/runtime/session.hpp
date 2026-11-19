#pragma once
#include "time.hpp"
#include "buffers.hpp"
#include "journal.hpp"
#include "checkpoint_writer.hpp"
#include <nessstudio/nessstudio.h>
#include "json.hpp"
#include <array>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <stop_token>
#include <string>
#include <thread>

namespace nss {
namespace preview { class Hub; }
using Json = nlohmann::json;
inline std::atomic<uint64_t> live_sessions{}, live_workers{}, pending_reapers{}, session_client_refs{};
struct EngineBudget { std::atomic<bool> active_session{}; };
struct SessionBudgetExceeded {};
struct Fault {
    nss_result code{}; uint32_t stage{}; int32_t hresult{}; std::string message;
    explicit operator bool() const noexcept { return code < 0; }
};
enum class ShutdownStage { Sources, Inflight, EncoderEos, Packets, Output, Resources, Done };
struct ShutdownProgress { bool pending{}; Fault error; };
// Implementations own their capture/process context on the Session worker. Cross-thread
// acquisition uses bounded queues; callbacks never invoke managed code.
class Backend {
    std::atomic<uint64_t> session_id_{};
public:
    void bind_session(uint64_t id) noexcept { session_id_ = id; }
    uint64_t session_id() const noexcept { return session_id_.load(); }
    virtual Fault prepare(std::stop_token) = 0;
    virtual Fault start(std::stop_token, int64_t epoch, SessionClock&) = 0;
    virtual Fault pause(std::stop_token) = 0;
    virtual Fault resume(std::stop_token, SessionClock&) = 0;
    virtual Fault tick(SessionClock&) = 0;
    virtual ShutdownProgress shutdown(ShutdownStage) = 0;
    virtual Json metrics() const = 0;
    virtual bool self_paced() const noexcept { return false; }
    virtual Json stages() const { return Json::array({{{"implementationId", "synthetic-test-only"}, {"software", true}, {"hardware", false}}}); }
    virtual Json assets() const { return Json::array(); }
    virtual std::shared_ptr<preview::Hub> preview(uint64_t) { return {}; }
    virtual uint64_t update_scene(const Json&) { return 0; }
    virtual bool media_complete() const noexcept { return false; }
    // Thread-safe notification: abandonment/failure must not promote partial media.
    virtual void cancel_media_completion() noexcept {}
    virtual bool first_content_admitted() const noexcept = 0;
    virtual ~Backend() = default;
};
// Production and the isolated test DLL link different factories. No synthetic backend
// or fault-injection controls are linked into the shipped libnessstudio.
std::unique_ptr<Backend> make_backend(const Json& request, Fault& error);
bool test_backend_build() noexcept;

class Session final {
    struct Operation { uint64_t id{}; uint32_t kind{}; std::stop_source cancel; bool result_emitted{}; };
    struct Event { nss_event_v1 value{}; bool ordinary{}; };
    std::shared_ptr<EngineBudget> budget_;
    std::unique_ptr<Backend> backend_;
    Json requested_;
    uint64_t id_;
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::thread worker_;
    SessionClock clock_;
    std::optional<Operation> operation_;
    uint64_t stop_id_{}, next_command_{1}, sequence_{}, command_count_{};
    std::array<Event, 16> events_{};
    size_t head_{}, count_{}, ordinary_reservations_{};
    uint32_t state_{NSS_CREATED};
    nss_result result_{};
    std::optional<Fault> first_error_;
    std::array<Fault, 8> secondary_{};
    size_t secondary_count_{}; uint64_t secondary_suppressed_{};
    bool stop_requested_{}, started_{}, closing_{}, cleanup_pending_{}, poll_busy_{}, poll_interrupted_{}, stop_result_emitted_{}, fault_emitted_{};
    int64_t stop_started_{};
    int64_t stop_timeout_hns_{};
    std::atomic<bool> budget_released_{};
    std::filesystem::path journal_path_;
    std::unique_ptr<Journal> journal_;
    std::unique_ptr<CheckpointWriter> checkpoint_writer_;
    std::atomic<int64_t> worker_heartbeat_{};
    int64_t last_checkpoint_{};
    struct CheckpointTiming { int64_t begin_hns{}, end_hns{}; uint32_t state{}; bool forced{}; };
    std::array<CheckpointTiming, 128> checkpoint_timings_{};
    uint64_t checkpoint_count_{};
    int64_t max_checkpoint_hns_{};
    void checkpoint(bool force = false, bool cleanup_done = false);
    void release_budget() noexcept;
    void emit(uint64_t command, nss_result status, uint32_t kind, const Fault& error = {}, bool ordinary = false);
    void latch(Fault error);
    void fail_operation(Fault error);
    void observe_stop_deadline();
    void run() noexcept;
    void execute(Operation op);
    void stop_pipeline();
public:
    Session(std::shared_ptr<EngineBudget>, std::unique_ptr<Backend>, Json request);
    ~Session();
    void start_worker();
    nss_result command(uint32_t command, uint64_t& id, Fault& error);
    nss_result poll(uint32_t timeout_ms, nss_event_v1& event, Fault& error);
    void interrupt() noexcept;
    void abandon() noexcept;
    Json snapshot(bool include_metrics = true) const;
    std::shared_ptr<preview::Hub> preview(uint64_t source);
    uint64_t update_scene(const Json& scene);
};
}
