#include <nessstudio/nessstudio.h>
#include "json.hpp"
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>
#include <set>
using Json=nlohmann::json;
static void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
static nss_error_v1 error(){nss_error_v1 e{};e.size=sizeof(e);e.version=1;return e;}
static nss_event_v1 event(){nss_event_v1 e{};e.size=sizeof(e);e.version=1;return e;}
static nss_json_v1 control(const std::string& value){return {sizeof(nss_json_v1),1,{reinterpret_cast<const uint8_t*>(value.data()),value.size()}};}
struct Engine{nss_engine* value{};Engine(){auto text=std::string{"{\"version\":1}"};auto c=control(text);auto e=error();check(nss_engine_create(&c,&value,&e)==0,"engine create");}~Engine(){nss_engine_release(value);}};
struct Session{
    nss_session* value{};uint64_t sequence{},object{};std::vector<nss_event_v1> observed;
    Session(Engine& engine,Json options=Json::object()){
        auto text=Json{{"version",1},{"testFixture",options}}.dump();auto c=control(text);auto e=error();
        check(nss_session_create(engine.value,&c,&value,&e)==0,"session create");object=snapshot()["objectId"].get<uint64_t>();
    }
    ~Session(){nss_session_release(value);}
    Json snapshot(){nss_blob* blob{};auto e=error();check(nss_session_snapshot(value,&blob,&e)==0,"snapshot");nss_utf8_view view{};check(nss_blob_view(blob,&view)==0,"blob view");auto result=Json::parse(view.data,view.data+view.bytes);nss_blob_release(blob);return result;}
    uint64_t send(uint32_t kind,nss_result expected=NSS_ACCEPTED){auto e=error();uint64_t id{};const auto result=nss_session_command(value,kind,&id,&e);check(result==expected,"unexpected command submission result");return id;}
    nss_event_v1 wait(uint64_t id){const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(9);for(;;){auto v=event();auto e=error();const auto code=nss_session_poll_event(value,100,&v,&e);check(code==0||code==NSS_TIMEOUT,"poll failed");if(code==0){check(v.object_id==object&&v.sequence==++sequence,"foreign or unordered event");observed.push_back(v);if(v.command_id==id&&v.kind==NSS_EVENT_RESULT)return v;}check(std::chrono::steady_clock::now()<deadline,"result timeout");}}
    void ready(){check(wait(send(NSS_COMMAND_PREPARE)).state==NSS_READY,"not ready");}
    void start(){ready();check(wait(send(NSS_COMMAND_START)).state==NSS_RECORDING,"not recording");}
    void cleanup(){const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(9);while(snapshot()["cleanupPending"].get<bool>()){check(std::chrono::steady_clock::now()<deadline,"cleanup timeout");std::this_thread::sleep_for(std::chrono::milliseconds(1));}}
};
int main(int argc,char** argv){
    try{
        const std::string scope=argc>1?argv[1]:"states";Json report{{"status","PASS"},{"scope",scope},{"synthetic",true}};
        {
            Engine engine;
            if(scope=="states"){
                Session s(engine);s.start();s.send(NSS_COMMAND_START,NSS_ALREADY);check(s.wait(s.send(NSS_COMMAND_PAUSE)).state==NSS_PAUSED,"pause");
                auto final=s.wait(s.send(NSS_COMMAND_STOP));check(final.state==NSS_COMPLETED&&final.status==0,"stop paused");s.cleanup();
                auto snap=s.snapshot();check(!snap["mediaComplete"].get<bool>()&&snap["metrics"]["testOnly"].get<bool>(),"synthetic media misrepresented");
                check(snap["metrics"]["shutdownOrder"]==Json::array({0,1,2,3,4,5}),"shutdown stage order");s.send(NSS_COMMAND_STOP,NSS_ALREADY);
                report["snapshot"]=snap;
            }else if(scope=="interrupt"){
                Session s(engine,{{"prepareDelayMs",1000}});const auto prepare=s.send(1);const auto stop=s.send(5);check(s.wait(prepare).status==NSS_E_CANCELLED,"prepare not cancelled");
                const auto done=s.wait(stop);check(done.state==NSS_COMPLETED&&done.status==NSS_E_CANCELLED,"pre-start stop became media success");s.cleanup();
                s.send(2,NSS_E_STATE);
            }else if(scope=="pressure"){
                Session s(engine);s.ready();s.wait(s.send(2));
                for(int i=0;i<8;++i){const uint32_t command=i%2?4u:3u;s.send(command);const auto goal=i%2?NSS_RECORDING:NSS_PAUSED;const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(2);while(s.snapshot()["state"]!=goal){check(std::chrono::steady_clock::now()<end,"state settle timeout");std::this_thread::yield();}}
                s.send(3,NSS_E_EVENT_BACKPRESSURE);const auto stop=s.send(5);check(s.wait(stop).status==0,"reserved Stop was blocked");s.cleanup();
                report["ordinary_reservation_limit"]=8;report["observed_events"]=s.observed.size();
            }else if(scope=="poll"){
                Session s(engine);std::atomic<bool> waiting{};std::atomic<nss_result> result{};
                std::thread reader([&]{auto v=event();auto e=error();waiting=true;result=nss_session_poll_event(s.value,1000,&v,&e);});
                while(!waiting)std::this_thread::yield();std::this_thread::sleep_for(std::chrono::milliseconds(20));
                auto v=event();auto e=error();const auto second=nss_session_poll_event(s.value,0,&v,&e);nss_session_interrupt_poll(s.value);reader.join();
                check(second==NSS_E_BUSY&&result==NSS_E_CANCELLED,"poll ownership/interrupt");s.wait(s.send(5));s.cleanup();
            }else if(scope=="faults"){
                Json rows=Json::array();
                for(uint32_t stage=1;stage<=11;++stage){Session s(engine,{{"faultStage",stage},{"faultAt","start"},{"secondaryOutputFailure",true}});s.ready();const auto failed=s.wait(s.send(2));check(failed.state==NSS_FAILED&&failed.stage==stage&&failed.status==NSS_E_IO,"failure stage/result lost");s.cleanup();const auto snap=s.snapshot();check(snap["firstError"]["stage"]==stage&&snap["firstError"]["sessionId"]==s.object&&snap["secondaryErrors"].size()==1,"causal or secondary error lost");rows.push_back(snap["firstError"]);}
                report["injected_stages"]=rows;
            }else if(scope=="source-stop"){
                Session s(engine,{{"faultStage",NSS_STAGE_CAPTURE},{"faultAt","tick"},{"faultDelayMs",100},{"secondaryOutputFailure",true}});s.start();
                const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
                while(!s.snapshot()["metrics"]["faultEntered"].get<bool>()){check(std::chrono::steady_clock::now()<deadline,"source failure did not enter");std::this_thread::yield();}
                const auto done=s.wait(s.send(5));check(done.status==NSS_E_IO&&done.stage==NSS_STAGE_CAPTURE&&done.state==NSS_FAILED,"Concurrent Stop replaced causal source failure");s.cleanup();
                check(s.snapshot()["secondaryErrors"].size()==1,"Concurrent cleanup error lost");report["source_failure_overlapped_stop"]=true;
            }else if(scope=="timeout"){
                Session s(engine,{{"drainDelayMs",5500}});s.start();const auto stopped=s.wait(s.send(5));
                check(stopped.status==NSS_E_TIMEOUT&&stopped.state==NSS_FAILED&&s.snapshot()["cleanupPending"].get<bool>(),"Stop timeout did not latch failure while retaining resources");
                s.cleanup();check(s.snapshot()["state"]==NSS_FAILED,"late cleanup promoted timeout to success");report["deadline_ms"]=5000;
            }else if(scope=="blocked-prepare"){
                Session s(engine,{{"blockedPrepareMs",5500}});const auto preparing=s.send(1);
                const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
                while(!s.snapshot()["metrics"]["prepareEntered"].get<bool>()){check(std::chrono::steady_clock::now()<deadline,"prepare did not enter");std::this_thread::yield();}
                const auto stop=s.send(5);check(s.wait(preparing).status==NSS_E_CANCELLED,"blocked Prepare was not cancelled before Stop result");
                check(s.wait(stop).status==NSS_E_TIMEOUT&&s.snapshot()["cleanupPending"].get<bool>(),"blocked backend hid Stop deadline or freed live resources");s.cleanup();
                check(s.snapshot()["state"]==NSS_FAILED,"late blocked prepare cleanup promoted timeout");report["noncooperative_backend_watchdog"]=true;
            }else if(scope=="first-content"){
                Session s(engine,{{"emptyStart",true}});s.ready();const auto started=s.wait(s.send(2));check(started.status==NSS_E_STATE&&started.state==NSS_FAILED,"empty start announced recording");s.cleanup();
            }else if(scope=="lifetime"){
                uint64_t previous{};
                for(int i=0;i<64;++i){
                    {Session s(engine,{{"prepareDelayMs",20}});check(s.object>previous,"session id reused");previous=s.object;s.send(1);}
                    const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(2);
                    for(;;){auto text=std::string{"{\"version\":1,\"testFixture\":{}}"};auto c=control(text);auto e=error();nss_session* next{};const auto result=nss_session_create(engine.value,&c,&next,&e);if(result==0){nss_session_release(next);break;}check(result==NSS_E_BUSY&&std::chrono::steady_clock::now()<end,"abandoned session did not release engine budget");std::this_thread::sleep_for(std::chrono::milliseconds(2));}
                    std::this_thread::sleep_for(std::chrono::milliseconds(3));
                }
                report["abandoned_during_prepare"]=64;
            }else throw std::invalid_argument("unknown session test scope");
        }
        auto e=error();nss_result quiet=NSS_TIMEOUT;for(int i=0;i<10&&quiet==NSS_TIMEOUT;++i)quiet=nss_runtime_quiesce(1000,&e);
        check(quiet==NSS_OK,"native workers/pools/clients leaked");report["native_quiescence"]=true;std::cout<<report.dump()<<'\n';return 0;
    }catch(const std::exception& e){std::cout<<Json{{"status","FAIL"},{"message",e.what()}}.dump()<<'\n';return 1;}
}
