#include "runtime/checkpoint_writer.hpp"
#include "runtime/journal.hpp"
#include "runtime/session.hpp"
#include <chrono>
#include <fstream>
#include <iostream>
#include <vector>

using Json = nlohmann::json;
using namespace std::chrono_literals;
static void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}
struct Gate {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered{}, released{};
    void block() {
        std::unique_lock lock(mutex);
        entered = true;
        changed.notify_all();
        changed.wait(lock, [&] { return released; });
    }
    void wait_entered() {
        std::unique_lock lock(mutex);
        require(changed.wait_for(lock, 3s, [&] { return entered; }), "Writer did not enter the controlled sink");
    }
    void release() {
        std::lock_guard lock(mutex);
        released = true;
        changed.notify_all();
    }
};
struct ReleaseGuard { Gate& gate; ~ReleaseGuard() { gate.release(); } };
class CountingBackend final : public nss::Backend {
public:
    std::atomic<uint64_t> prepares{};
    nss::Fault prepare(std::stop_token) override { ++prepares; return {}; }
    nss::Fault start(std::stop_token, int64_t, nss::SessionClock&) override { return {}; }
    nss::Fault pause(std::stop_token) override { return {}; }
    nss::Fault resume(std::stop_token, nss::SessionClock&) override { return {}; }
    nss::Fault tick(nss::SessionClock&) override { return {}; }
    nss::ShutdownProgress shutdown(nss::ShutdownStage) override { return {}; }
    Json metrics() const override { return Json::object(); }
    bool first_content_admitted() const noexcept override { return true; }
};

int wmain(int argc, wchar_t **argv) {
    try {
        if (argc != 3) return 2;
        const std::wstring scope(argv[1]);
        const auto root = std::filesystem::path(argv[2]) / (scope + L"-" + std::to_wstring(nss::qpc_hns()));
        std::filesystem::create_directories(root);
        std::vector<int> stored;
        if (scope == L"bounded") {
            Gate gate;
            nss::CheckpointWriter writer([&](Json value) {
                const auto id = value.at("id").get<int>();
                if (id == 1) gate.block();
                stored.push_back(id);
            });
            ReleaseGuard release{gate};
            writer.store_periodic({{"id", 1}});
            gate.wait_entered();
            for (int id = 2; id <= 50; ++id) writer.store_periodic({{"id", id}});
            const auto metrics = writer.metrics();
            require(metrics.at("active") == true && metrics.at("pending") == true &&
                metrics.at("durablyStored") == 0 && metrics.at("coalescedPeriodicSnapshots") == 48 &&
                metrics.at("pendingSnapshotLimit") == 1, "Periodic handoff blocked or its queue was not bounded");
            gate.release();
            writer.close();
            require(stored == std::vector<int>({1, 50}), "Coalesced periodic state was lost or reordered");
        } else if (scope == L"barrier") {
            Gate gate;
            nss::CheckpointWriter writer([&](Json value) {
                const auto id = value.at("id").get<int>();
                if (id == 1) gate.block();
                stored.push_back(id);
            });
            ReleaseGuard release{gate};
            writer.store_periodic({{"id", 1}});
            gate.wait_entered();
            writer.store_periodic({{"id", 2}});
            std::atomic<bool> barrier_observed{};
            std::jthread unlock([&] {
                const auto deadline = std::chrono::steady_clock::now() + 3s;
                while (std::chrono::steady_clock::now() < deadline) {
                    if (writer.metrics().at("submitted") == 3) { barrier_observed = true; break; }
                    std::this_thread::sleep_for(1ms);
                }
                gate.release();
            });
            writer.store_barrier({{"id", 3}});
            require(barrier_observed && writer.metrics().at("completedSubmissionSequence") == 3,
                "Mandatory barrier returned before its own durable write");
            writer.close();
            require(stored == std::vector<int>({1, 3}), "An older pending snapshot superseded a mandatory barrier");
        } else if (scope == L"failure") {
            Gate gate;
            nss::CheckpointWriter writer([&](Json) { gate.block(); throw nss::JournalError("Controlled durable sink failure", 42); });
            ReleaseGuard release{gate};
            writer.store_periodic({{"id", 1}});
            gate.wait_entered();
            writer.store_periodic({{"id", 2}});
            gate.release();
            bool barrier_failed{}, periodic_failed{};
            try { writer.store_barrier({{"id", 3}}); }
            catch (const nss::JournalError& error) { barrier_failed = error.native == 42; }
            try { writer.store_periodic({{"id", 4}}); }
            catch (const nss::JournalError& error) { periodic_failed = error.native == 42; }
            writer.close();
            const auto metrics = writer.metrics();
            require(barrier_failed && periodic_failed && metrics.at("failed") == true &&
                metrics.at("durablyStored") == 0 && metrics.at("pending") == false,
                "Asynchronous storage failure was hidden or reported as durable completion");
        } else if (scope == L"drain") {
            {
                nss::CheckpointWriter writer([&](Json value) { stored.push_back(value.at("id")); });
                writer.store_barrier({{"id", 1}});
                writer.store_periodic({{"id", 2}});
            }
            require(stored == std::vector<int>({1, 2}), "Destruction abandoned the latest pending checkpoint");
            nss::CheckpointWriter writer([](Json) {});
            writer.close();
            bool rejected{};
            try { writer.store_periodic(Json::object()); } catch (const std::logic_error&) { rejected = true; }
            require(rejected, "A closed writer accepted another snapshot");
        } else if (scope == L"files") {
            const auto path = root / L"owned-journal.json";
            nss::Journal journal(path, 42, "recording");
            nss::CheckpointWriter writer([&](Json value) { journal.store(std::move(value)); });
            writer.store_barrier({{"state", 2}, {"mediaComplete", false}});
            for (int index = 0; index < 20; ++index) writer.store_periodic({{"state", 4}, {"index", index}});
            writer.store_barrier({{"state", 10}, {"mediaComplete", true}, {"cleanupPending", false}});
            std::ifstream stream(path);
            const auto final = Json::parse(stream);
            require(final.at("snapshot").at("state") == 10 && final.at("snapshot").at("mediaComplete") == true &&
                final.at("sequence").get<uint64_t>() >= 2, "Final barrier did not publish the real flushed NTFS checkpoint");
            writer.close();
        } else if (scope == L"session-collision") {
            const auto output = root / L"owned-output.mkv";
            const auto utf8 = output.u8string();
            Json request{{"outputPath", std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size())}};
            auto backend = std::make_unique<CountingBackend>();
            auto *counter = backend.get();
            nss::Session session(std::make_shared<nss::EngineBudget>(), std::move(backend), request);
            const auto journal = nss::Journal::recording_path(request, session.snapshot().at("objectId"));
            { std::ofstream file(journal); file << "preserve foreign checkpoint"; }
            uint64_t command{};
            nss::Fault error;
            require(session.command(NSS_COMMAND_PREPARE, command, error) == NSS_ACCEPTED, "Could not queue Prepare before journal startup");
            session.start_worker();
            bool result{};
            const auto deadline = std::chrono::steady_clock::now() + 3s;
            while (std::chrono::steady_clock::now() < deadline) {
                nss_event_v1 event{};
                const auto code = session.poll(100, event, error);
                if (code == NSS_OK && event.command_id == command && event.kind == NSS_EVENT_RESULT) { result = true; break; }
            }
            require(result && counter->prepares == 0 && session.snapshot().at("firstError").at("code") == NSS_E_IO,
                "A queued Prepare ran after journal reservation had already failed");
            std::ifstream file(journal);
            const std::string text((std::istreambuf_iterator<char>(file)), {});
            require(text == "preserve foreign checkpoint", "Session journal replaced an unowned file");
        } else return 2;
        require(nss::live_checkpoint_workers == 0, "Checkpoint worker survived its owner");
        std::string label;
        for (const auto character : scope) label.push_back(static_cast<char>(character)); // Supported scopes above are ASCII.
        std::cout << Json{{"status", "PASS"}, {"scope", label},
            {"method", "Bounded queue, mandatory durable barriers, sticky I/O failure, owned worker drain and real NTFS/session collision checks"}}.dump() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
