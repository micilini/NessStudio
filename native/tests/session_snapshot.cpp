#include "runtime/session.hpp"
#include <chrono>
#include <iostream>
#include <stdexcept>

using namespace std::chrono_literals;
namespace {
void require(bool value, const char *message) {
    if (!value) throw std::runtime_error(message);
}
enum class Reader { Metrics, Stages, Assets };
struct State {
    std::atomic<uint64_t> ticks{};
    std::atomic<bool> enabled{};
    std::mutex mutex;
    std::condition_variable changed;
    bool entered{}, released{};
    void read() {
        if (!enabled) return;
        std::unique_lock lock(mutex);
        entered = true;
        changed.notify_all();
        if (!changed.wait_for(lock, 5s, [&] { return released; }))
            throw std::runtime_error("Test reader was not released");
    }
    void release() {
        std::lock_guard lock(mutex);
        released = true;
        changed.notify_all();
    }
};
class Backend final : public nss::Backend {
    std::shared_ptr<State> state_;
    Reader reader_;
public:
    Backend(std::shared_ptr<State> state, Reader reader) : state_(std::move(state)), reader_(reader) {}
    nss::Fault prepare(std::stop_token) override { return {}; }
    nss::Fault start(std::stop_token, int64_t, nss::SessionClock&) override { return {}; }
    nss::Fault pause(std::stop_token) override { return {}; }
    nss::Fault resume(std::stop_token, nss::SessionClock&) override { return {}; }
    nss::Fault tick(nss::SessionClock&) override {
        ++state_->ticks;
        std::this_thread::sleep_for(1ms);
        return {};
    }
    nss::ShutdownProgress shutdown(nss::ShutdownStage) override { return {}; }
    bool self_paced() const noexcept override { return true; }
    bool first_content_admitted() const noexcept override { return true; }
    nss::Json metrics() const override {
        if (reader_ == Reader::Metrics) state_->read();
        return {{"ticks", state_->ticks.load()}};
    }
    nss::Json stages() const override {
        if (reader_ == Reader::Stages) state_->read();
        return nss::Json::array();
    }
    nss::Json assets() const override {
        if (reader_ == Reader::Assets) state_->read();
        return nss::Json::array();
    }
};
void command(nss::Session &session, uint32_t kind, uint32_t expected) {
    uint64_t id{}; nss::Fault error;
    require(session.command(kind, id, error) == NSS_ACCEPTED, "Command rejected");
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    do {
        nss_event_v1 event{};
        const auto status = session.poll(100, event, error);
        require(status == NSS_OK || status == NSS_TIMEOUT, "Event poll failed");
        if (status == NSS_OK && event.command_id == id && event.kind == NSS_EVENT_RESULT) {
            require(event.status == NSS_OK && event.state == expected, "Command did not complete");
            return;
        }
    } while (std::chrono::steady_clock::now() < deadline);
    throw std::runtime_error("Command timed out");
}
void verify(Reader reader) {
    auto state = std::make_shared<State>();
    auto budget = std::make_shared<nss::EngineBudget>(); budget->active_session = true;
    nss::Session session(budget, std::make_unique<Backend>(state, reader), nss::Json::object());
    session.start_worker();
    command(session, NSS_COMMAND_PREPARE, NSS_READY);
    command(session, NSS_COMMAND_START, NSS_RECORDING);
    state->enabled = true;
    std::exception_ptr failure;
    nss::Json snapshot;
    std::thread observer([&] {
        try { snapshot = session.snapshot(); }
        catch (...) { failure = std::current_exception(); }
    });
    // Release before joining even when an assertion throws: a failing test
    // must not strand its observer or the Session destructor.
    struct Finish {
        State &state; std::thread &observer;
        ~Finish() { state.release(); if (observer.joinable()) observer.join(); }
    } finish{*state, observer};
    {
        std::unique_lock lock(state->mutex);
        require(state->changed.wait_for(lock, 2s, [&] { return state->entered; }), "Reader did not enter");
    }
    const auto before = state->ticks.load();
    const auto deadline = std::chrono::steady_clock::now() + 1s;
    while (state->ticks.load() < before + 4 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(1ms);
    const bool progressed = state->ticks.load() >= before + 4;
    state->release(); observer.join();
    if (failure) std::rethrow_exception(failure);
    require(progressed, "A slow diagnostic reader stalled the recording worker");
    require(snapshot.at("state") == NSS_RECORDING && !snapshot.at("mediaComplete").get<bool>(),
            "Snapshot control state changed while materializing backend data");
    command(session, NSS_COMMAND_STOP, NSS_COMPLETED);
}
}
int main() {
    try {
        for (auto reader : {Reader::Metrics, Reader::Stages, Reader::Assets}) verify(reader);
        require(nss::live_sessions == 0 && nss::live_workers == 0, "Test session leaked");
        std::cout << "{\"status\":\"PASS\",\"blockedReaderCases\":3}\n";
        return 0;
    } catch (const std::exception &error) {
        std::cout << nss::Json{{"status", "FAIL"}, {"message", error.what()}}.dump() << '\n';
        return 1;
    }
}
