#include "runtime/session.hpp"
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace std::chrono_literals;
class SlowDurableClose final : public nss::Backend {
  bool completed_{};
  std::chrono::milliseconds delay_;
public:
  explicit SlowDurableClose(std::chrono::milliseconds delay) : delay_(delay) {}
  nss::Fault prepare(std::stop_token) override { return {}; }
  nss::Fault start(std::stop_token,int64_t,nss::SessionClock&) override { return {}; }
  nss::Fault pause(std::stop_token) override { return {}; }
  nss::Fault resume(std::stop_token,nss::SessionClock&) override { return {}; }
  nss::Fault tick(nss::SessionClock&) override { return {}; }
  nss::ShutdownProgress shutdown(nss::ShutdownStage stage) override {
    if(stage==nss::ShutdownStage::Sources) std::this_thread::sleep_for(delay_);
    if(stage==nss::ShutdownStage::Output) completed_=true;
    return {};
  }
  nss::Json metrics() const override { return nss::Json::object(); }
  bool media_complete() const noexcept override { return completed_; }
  bool first_content_admitted() const noexcept override { return true; }
};

int main() {
  try {
    for(bool interactive:{true,false}) {
      auto session=std::make_unique<nss::Session>(std::make_shared<nss::EngineBudget>(),
        std::make_unique<SlowDurableClose>(interactive ? 67000ms : 6200ms),nss::Json{{"interactive",interactive}});
      session->start_worker();
      bool slow_stop_observed{};
      auto command=[&](uint32_t kind) {
        uint64_t id{};nss::Fault fault;
        if(session->command(kind,id,fault)!=NSS_ACCEPTED) throw std::runtime_error("Command rejected");
        const auto until=std::chrono::steady_clock::now()+90s;
        while(std::chrono::steady_clock::now()<until) {
          nss_event_v1 event{};
          if(session->poll(100,event,fault)==NSS_OK && event.command_id==id && event.kind==NSS_EVENT_RESULT)
            return event.status;
          if(interactive && kind==NSS_COMMAND_STOP) {
            const auto snapshot=session->snapshot();
            const auto &watchdog=snapshot.at("watchdog");
            if(!watchdog.at("diagnosticOnly").get<bool>() || watchdog.at("stopDeadlineMs")!=0)
              throw std::runtime_error("Interactive shutdown still has a fatal elapsed-time deadline");
            if(watchdog.at("slowStop").get<bool>()) {
              slow_stop_observed=true;
              if(!snapshot.at("firstError").is_null() || !snapshot.at("cleanupPending").get<bool>())
                throw std::runtime_error("Slow shutdown was marked failed or released its resources prematurely");
            }
          }
        }
        throw std::runtime_error("Command did not finish");
      };
      if(command(NSS_COMMAND_PREPARE)!=NSS_OK || command(NSS_COMMAND_START)!=NSS_OK)
        throw std::runtime_error("Session did not start");
      const auto result=command(NSS_COMMAND_STOP);
      if(result!=(interactive?NSS_OK:NSS_E_TIMEOUT))
        throw std::runtime_error("Slow durable close used the wrong stop deadline");
      if(interactive && (!slow_stop_observed || !session->snapshot().at("mediaComplete").get<bool>()))
        throw std::runtime_error("A valid interactive recording was discarded during slow storage");
    }
    std::cout<<"{\"status\":\"PASS\",\"slowInteractiveCloseAccepted\":true,\"interactiveStorageDelayMs\":67000,\"slowWaitDiagnosticObserved\":true,\"shortProbeDeadlinePreserved\":true}\n";
    return 0;
  } catch(const std::exception &error) { std::cerr<<error.what()<<'\n';return 1; }
}
