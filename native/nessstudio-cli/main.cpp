#include "bootstrap.hpp"
#include "json.hpp"
#include <iostream>
#include <map>
#include <vector>
#include <chrono>
#include <fstream>
#include <psapi.h>

using json = nlohmann::json;
struct api final {
    nss_bootstrap::module library;
    decltype(&nss_get_version) version;
    decltype(&nss_engine_create) create;
    decltype(&nss_engine_release) release;
    decltype(&nss_engine_capabilities) capabilities;
    decltype(&nss_engine_devices) devices;
    decltype(&nss_blob_view) view;
    decltype(&nss_blob_release) release_blob;
    decltype(&nss_runtime_quiesce) quiesce;
    decltype(&nss_session_create) session;
    decltype(&nss_session_command) command;
    decltype(&nss_session_snapshot) session_snapshot;
    decltype(&nss_session_poll_event) session_poll;
    decltype(&nss_session_release) session_release;
    decltype(&nss_probe_software_encoder) software;
    decltype(&nss_probe_capture) capture;
    decltype(&nss_job_create) job_create;
    decltype(&nss_job_start) job_start;
    decltype(&nss_job_snapshot) job_snapshot;
    decltype(&nss_job_poll_event) job_poll;
    decltype(&nss_job_release) job_release;
    uint64_t last_blob_bytes{};
    explicit api(const std::filesystem::path& path) : library(path),
        version(library.get<decltype(version)>("nss_get_version")), create(library.get<decltype(create)>("nss_engine_create")),
        release(library.get<decltype(release)>("nss_engine_release")), capabilities(library.get<decltype(capabilities)>("nss_engine_capabilities")),
        devices(library.get<decltype(devices)>("nss_engine_devices")), view(library.get<decltype(view)>("nss_blob_view")),
        release_blob(library.get<decltype(release_blob)>("nss_blob_release")), quiesce(library.get<decltype(quiesce)>("nss_runtime_quiesce")),
        session(library.get<decltype(session)>("nss_session_create")), command(library.get<decltype(command)>("nss_session_command")),
        session_snapshot(library.get<decltype(session_snapshot)>("nss_session_snapshot")), session_poll(library.get<decltype(session_poll)>("nss_session_poll_event")), session_release(library.get<decltype(session_release)>("nss_session_release")),
        software(library.get<decltype(software)>("nss_probe_software_encoder")), capture(library.get<decltype(capture)>("nss_probe_capture")),
        job_create(library.get<decltype(job_create)>("nss_job_create")),job_start(library.get<decltype(job_start)>("nss_job_start")),
        job_snapshot(library.get<decltype(job_snapshot)>("nss_job_snapshot")),job_poll(library.get<decltype(job_poll)>("nss_job_poll_event")),
        job_release(library.get<decltype(job_release)>("nss_job_release")) {}
    json consume(nss_blob* blob) {
        if (!blob) throw std::runtime_error("Native operation returned a null blob");
        struct owner { api& api_; nss_blob* blob_; ~owner() { api_.release_blob(blob_); } } owned{*this, blob};
        nss_utf8_view data{};
        if (view(blob, &data) != NSS_OK || data.bytes > NSS_MAX_JSON_BYTES || !data.data) throw std::runtime_error("Invalid native blob view");
        last_blob_bytes=data.bytes;
        return json::parse(data.data, data.data + data.bytes);
    }
};
static void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
static nss_error_v1 error() { nss_error_v1 value{}; value.size = sizeof(value); value.version = NSS_STRUCT_VERSION; return value; }
static nss_json_v1 control(const std::string& bytes) { return {sizeof(nss_json_v1), NSS_STRUCT_VERSION, {reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()}}; }
static json session_test(api& native, nss_engine* engine) {
    nss_blob* blob{}; auto diagnostic = error();
    check(native.capabilities(engine, &blob, &diagnostic) == NSS_OK, "Capabilities failed");
    check(native.consume(blob).value("syntheticTestBackend", false), "Session self-test requires the isolated test DLL; the product does not provide a synthetic capture route");
    const auto text = json{{"version",1},{"testFixture",json::object()}}.dump(); auto request = control(text); nss_session* handle{};
    check(native.session(engine, &request, &handle, &diagnostic) == NSS_OK, "Test session creation failed");
    struct release { api& native; nss_session* handle; ~release(){native.session_release(handle);} } owned{native,handle};
    uint64_t sequence{}, object{}; json states=json::array(), events=json::array();
    for (uint32_t kind = NSS_COMMAND_PREPARE; kind <= NSS_COMMAND_STOP; ++kind) {
        uint64_t id{}; check(native.command(handle,kind,&id,&diagnostic)==NSS_ACCEPTED && id,"Session command was not accepted");
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
        for (;;) {
            nss_event_v1 value{};value.size=sizeof(value);value.version=1;
            const auto result=native.session_poll(handle,100,&value,&diagnostic);
            check(result==NSS_OK||result==NSS_TIMEOUT,"Session poll failed");
            if(result==NSS_OK){
                if(!object)object=value.object_id;
                check(value.object_id==object && value.sequence==++sequence,"Session events are foreign or out of order");
                events.push_back({{"id",value.command_id},{"sequence",value.sequence},{"state",value.state},{"status",value.status}});
                if(value.command_id==id && value.kind==NSS_EVENT_RESULT){check(value.status==NSS_OK,"Session command failed");states.push_back(value.state);break;}
            }
            check(std::chrono::steady_clock::now()<deadline,"Session command result timed out");
        }
    }
    check(native.session_snapshot(handle,&blob,&diagnostic)==NSS_OK,"Session snapshot failed");auto snapshot=native.consume(blob);
    check(states==json::array({NSS_READY,NSS_RECORDING,NSS_PAUSED,NSS_RECORDING,NSS_COMPLETED}),"Session state sequence mismatch");
    check(!snapshot["mediaComplete"].get<bool>()&&!snapshot["cleanupPending"].get<bool>(),"Synthetic session misreported media or cleanup");
    return {{"status","PASS"},{"scope","session"},{"synthetic",true},{"states",states},{"events",events},{"snapshot",snapshot}};
}
static json record(api& native,nss_engine* engine,const json& config) {
    const auto bytes=config.dump();auto request=control(bytes);auto diagnostic=error();nss_session* session{};
    auto result=native.session(engine,&request,&session,&diagnostic);
    if(result!=NSS_OK)return {{"status","FAIL"},{"code",result},{"stage",diagnostic.stage},{"hresult",diagnostic.hresult},{"message",reinterpret_cast<const char*>(diagnostic.message_utf8)}};
    struct Owner {api& native;nss_session* session;~Owner(){native.session_release(session);}} owner{native,session};
    json events=json::array(),snapshot,commands=json::array();
    const auto pauses=config.value("pauseScript",json::array());size_t next_pause{};
    const auto duration=config.value("durationMs",uint64_t{5000})*10000;
    json process_samples=json::array();
    const auto sample_epoch=std::chrono::steady_clock::now();auto sample_previous=sample_epoch;
    auto inspected_at=sample_epoch-std::chrono::seconds(1);
    uint64_t snapshot_count{},snapshot_bytes{};
    bool refresh_after_event{};
    FILETIME sample_created{},sample_exit{},sample_kernel{},sample_user{};
    GetProcessTimes(GetCurrentProcess(),&sample_created,&sample_exit,&sample_kernel,&sample_user);
    auto file_ticks=[](FILETIME t){return (uint64_t(t.dwHighDateTime)<<32)|t.dwLowDateTime;};
    uint64_t sample_cpu_previous=file_ticks(sample_kernel)+file_ticks(sample_user);
    SYSTEM_INFO sample_system{};GetSystemInfo(&sample_system);
    uint64_t peak_private{};
    auto inspect=[&](bool force=false) {
        const auto inspect_now=std::chrono::steady_clock::now();
        if(!force&&!refresh_after_event&&!snapshot.is_null()) {
            int64_t interval_hns=10000000;
            if(snapshot.at("state")==NSS_RECORDING) {
                uint64_t boundary=duration;
                if(next_pause<pauses.size())boundary=(std::min)(boundary,pauses[next_pause].at("atMs").get<uint64_t>()*10000);
                const auto active=snapshot.at("activeTimeHns").get<uint64_t>();
                const auto remaining=boundary>active?boundary-active:0;
                // The cached native clock only schedules the next authoritative
                // read. Pause/Stop decisions still use a fresh native snapshot.
                interval_hns=(std::min)(interval_hns,remaining>200000?static_cast<int64_t>(remaining-200000):int64_t{0});
            }
            if(inspect_now-inspected_at<std::chrono::nanoseconds(interval_hns*100))return;
        }
        nss_blob* blob{};check(native.session_snapshot(session,&blob,&diagnostic)==NSS_OK,"Recording snapshot failed");
        snapshot=native.consume(blob);
        inspected_at=inspect_now;refresh_after_event=false;++snapshot_count;snapshot_bytes+=native.last_blob_bytes;
        PROCESS_MEMORY_COUNTERS_EX memory{};memory.cb=sizeof(memory);
        if(GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),sizeof(memory)))
            peak_private=(std::max)({peak_private,uint64_t(memory.PrivateUsage),uint64_t(memory.PeakPagefileUsage)});
        const auto now=std::chrono::steady_clock::now();
        if(now-sample_previous>=std::chrono::seconds(1)) {
            GetProcessTimes(GetCurrentProcess(),&sample_created,&sample_exit,&sample_kernel,&sample_user);
            const auto cpu_now=file_ticks(sample_kernel)+file_ticks(sample_user);
            DWORD handles{};GetProcessHandleCount(GetCurrentProcess(),&handles);
            if(process_samples.size()<600)process_samples.push_back({{"seconds",std::chrono::duration<double>(now-sample_epoch).count()},
                {"cpuNormalizedPercent",double(cpu_now-sample_cpu_previous)/10000000/std::chrono::duration<double>(now-sample_previous).count()/sample_system.dwNumberOfProcessors*100},
                {"privateBytes",memory.PrivateUsage},{"workingSetBytes",memory.WorkingSetSize},{"handles",handles}});
            sample_previous=now;sample_cpu_previous=cpu_now;
        }
    };
    auto poll=[&](uint32_t timeout) {
        nss_event_v1 event{};event.size=sizeof(event);event.version=1;
        const auto code=native.session_poll(session,timeout,&event,&diagnostic);
        check(code==NSS_OK||code==NSS_TIMEOUT,"Recording event poll failed");
        if(code==NSS_OK)events.push_back({{"id",event.command_id},{"sequence",event.sequence},{"kind",event.kind},
            {"state",event.state},{"status",event.status},{"stage",event.stage},{"hresult",event.hresult},
            {"message",reinterpret_cast<const char*>(event.message_utf8)}});
        if(code==NSS_OK&&event.state==NSS_FAILED)refresh_after_event=true;
        return std::pair{code,event};
    };
    auto command=[&](uint32_t kind) {
        uint64_t id{};
        const auto begin=std::chrono::steady_clock::now();
        const auto code=native.command(session,kind,&id,&diagnostic);
        if(code==NSS_ALREADY)return true;
        if(code!=NSS_ACCEPTED)return false;
        bool ok=false;
        while(std::chrono::steady_clock::now()-begin<std::chrono::seconds(8)) {
            const auto [received,event]=poll(100);
            if(received==NSS_OK&&event.command_id==id&&event.kind==NSS_EVENT_RESULT){ok=event.status==NSS_OK;break;}
        }
        commands.push_back({{"kind",kind},{"id",id},{"status",ok?"PASS":"FAIL"},
            {"latencyMs",std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count()}});
        inspect(true);return ok;
    };
    FILETIME created{},exit{},kernel_before{},user_before{},kernel_after{},user_after{};
    GetProcessTimes(GetCurrentProcess(),&created,&exit,&kernel_before,&user_before);
    const auto begin=std::chrono::steady_clock::now();
    bool ok=command(NSS_COMMAND_PREPARE)&&command(NSS_COMMAND_START);
    while(ok) {
        inspect();
        if(snapshot.at("state")==NSS_FAILED){ok=false;break;}
        const auto active=snapshot.at("activeTimeHns").get<uint64_t>();
        if(active>=duration)break;
        if(next_pause<pauses.size()&&active>=pauses[next_pause].at("atMs").get<uint64_t>()*10000) {
            ok=command(NSS_COMMAND_PAUSE);
            const auto resume_at=std::chrono::steady_clock::now()+std::chrono::milliseconds(pauses[next_pause].at("durationMs").get<uint64_t>());
            while(ok&&std::chrono::steady_clock::now()<resume_at){poll(20);inspect();if(snapshot.at("state")==NSS_FAILED)ok=false;}
            if(ok)ok=command(NSS_COMMAND_RESUME);
            ++next_pause;
        }
        poll(10);
    }
    const bool stopped=command(NSS_COMMAND_STOP);inspect(true);
    const auto cleanup_limit=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while(snapshot.value("cleanupPending",false)&&std::chrono::steady_clock::now()<cleanup_limit){poll(10);inspect(true);}
    GetProcessTimes(GetCurrentProcess(),&created,&exit,&kernel_after,&user_after);
    auto ticks=[](FILETIME t){return (uint64_t(t.dwHighDateTime)<<32)|t.dwLowDateTime;};
    const auto elapsed=std::chrono::duration<double>(std::chrono::steady_clock::now()-begin).count();
    SYSTEM_INFO system{};GetSystemInfo(&system);
    const auto cpu=(ticks(kernel_after)-ticks(kernel_before))+(ticks(user_after)-ticks(user_before));
    return {{"status",ok&&stopped&&snapshot.value("mediaComplete",false)?"PASS":"FAIL"},{"scope","physical-recording"},
        {"snapshot",snapshot},{"events",events},{"commands",commands},{"elapsedSeconds",elapsed},
        {"peakPrivateBytes",peak_private},{"processSamples",process_samples},
        {"snapshotSampling",{{"nativeSnapshotCount",snapshot_count},{"nativeSnapshotBytes",snapshot_bytes},
            {"maxIntervalMs",1000},{"nearControlBoundaryMs",20},{"failureEventForcesRefresh",true},
            {"privatePeakMethod","Windows peak process commit plus sampled PrivateUsage"}}},
        {"processCpuNormalizedPercent",double(cpu)/10000000/elapsed/system.dwNumberOfProcessors*100}};
}
static void write_report(const std::filesystem::path& path,const json& report) {
    const auto bytes=report.dump(2)+"\n";
    HANDLE file=CreateFileW(path.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE)throw std::runtime_error("Report requires a new writable path");
    struct Close {HANDLE h;~Close(){CloseHandle(h);}} close{file};
    DWORD written{};
    if(!WriteFile(file,bytes.data(),static_cast<DWORD>(bytes.size()),&written,nullptr)||written!=bytes.size()||!FlushFileBuffers(file))
        throw std::runtime_error("Report write/flush failed");
}
int wmain(int argc, wchar_t** argv) {
    // The JSON protocol must reach redirected stdout before native teardown.
    std::cout << std::unitbuf;
    try {
        if (argc < 2) { std::cout << json{{"status", "error"}, {"error", "Expected version, capabilities, devices or self-test"}}.dump() << '\n'; return 64; }
        const auto verb = nss_bootstrap::utf8(argv[1]);
        auto path = nss_bootstrap::directory() / L"libnessstudio.dll";
        std::string scope = "abi", client = "nessstudio-cli", output,profile="av-basic";
        std::filesystem::path session_manifest,ffmpeg=nss_bootstrap::directory()/L"Native/FFmpeg/ffmpeg.exe";
        std::filesystem::path capture_config,report_path;
        for (int i = 2; i < argc; ++i) {
            const std::wstring argument(argv[i]);
            if (argument == L"--json") continue;
            if (++i >= argc) throw std::runtime_error("Missing option value");
            if (argument == L"--library") path = argv[i];
            else if (argument == L"--scope") scope = nss_bootstrap::utf8(argv[i]);
            else if (argument == L"--client-name") client = nss_bootstrap::utf8(argv[i]);
            else if (argument == L"--output") output = nss_bootstrap::utf8(argv[i]);
            else if (argument == L"--config") capture_config = argv[i];
            else if (argument == L"--report") report_path = argv[i];
            else if (argument == L"--session") session_manifest=argv[i];
            else if (argument == L"--profile") profile=nss_bootstrap::utf8(argv[i]);
            else if (argument == L"--ffmpeg") ffmpeg=argv[i];
            else throw std::runtime_error("Unknown command option");
        }
        api native(path);
        nss_version_v1 version{}; version.size = sizeof(version); version.version = NSS_STRUCT_VERSION;
        check(native.version(&version) == NSS_OK, "Version call failed");
        const std::string build(reinterpret_cast<const char*>(version.build_utf8));
        if (verb == "version") { std::cout << json{{"status", "PASS"}, {"abiMajor", version.abi_major}, {"abiMinor", version.abi_minor}, {"pointerBits", version.pointer_bits}, {"contractRevision", version.contract_revision}, {"build", build}}.dump() << '\n'; return 0; }
        const std::string config = json{{"version", 1}, {"clientName", client}}.dump();
        auto request = control(config); auto diagnostic = error(); nss_engine* engine = nullptr;
        check(native.create(&request, &engine, &diagnostic) == NSS_OK && engine, "Engine creation failed");
        struct owner { api& api_; nss_engine*& value_; ~owner() { api_.release(value_); } } owned{native, engine};
        nss_blob* blob = nullptr;
        if(verb=="export"){
            if(report_path.empty())throw std::runtime_error("Export requires --report");
            report_path=std::filesystem::absolute(report_path).lexically_normal();
            if(std::filesystem::exists(report_path)||!std::filesystem::is_directory(report_path.parent_path()))throw std::runtime_error("Export report requires a new file");
            const auto export_path=std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(output.data()),output.size()));
            for(const auto &candidate:{export_path.native(),export_path.native()+L".partial.mkv",export_path.native()+L".export.json",export_path.native()+L".export.json.pending"})
                if(_wcsicmp(candidate.c_str(),report_path.c_str())==0)throw std::runtime_error("Export report and media assets require distinct paths");
            const auto description=json{{"version",1},{"type","export"},{"profile",profile},{"sessionManifestPath",nss_bootstrap::utf8(std::filesystem::absolute(session_manifest).native().c_str())},{"ffmpegPath",nss_bootstrap::utf8(std::filesystem::absolute(ffmpeg).native().c_str())},{"outputPath",output}}.dump();
            auto job_request=control(description);nss_job *job{};
            const auto created=native.job_create(engine,&job_request,&job,&diagnostic);
            if(created!=NSS_OK){const auto report=json{{"status","FAIL"},{"build",build},{"code",created},{"stage",diagnostic.stage},{"message",reinterpret_cast<const char*>(diagnostic.message_utf8)}};write_report(report_path,report);std::cout<<report.dump()<<'\n';return 1;}
            struct JobOwner{api &native;nss_job *job;~JobOwner(){native.job_release(job);}} job_owner{native,job};
            uint64_t command{};check(native.job_start(job,&command,&diagnostic)==NSS_ACCEPTED,"Native export start failed");
            json events=json::array();
            for(;;){
                nss_event_v1 event{};event.size=sizeof(event);event.version=1;
                const auto code=native.job_poll(job,1000,&event,&diagnostic);check(code==NSS_OK||code==NSS_TIMEOUT,"Native job poll failed");
                if(code==NSS_OK){events.push_back({{"sequence",event.sequence},{"commandId",event.command_id},{"state",event.state},{"status",event.status}});if(event.state==NSS_JOB_COMPLETED||event.state==NSS_JOB_FAILED||event.state==NSS_JOB_CANCELLED)break;}
            }
            check(native.job_snapshot(job,&blob,&diagnostic)==NSS_OK,"Native job snapshot failed");
            auto report=native.consume(blob);report["status"]=report.at("mediaComplete")==true?"PASS":"FAIL";report["events"]=events;report["build"]=build;
            write_report(report_path,report);std::cout<<report.dump()<<'\n';return report.at("status")=="PASS"?0:1;
        }
        if(verb=="record") {
            if(!report_path.empty())report_path=std::filesystem::absolute(report_path).lexically_normal();
            if(report_path.empty()||!report_path.is_absolute()||std::filesystem::exists(report_path)||!std::filesystem::is_directory(report_path.parent_path()))
                throw std::runtime_error("Record requires a new absolute --report path");
            std::ifstream input(capture_config,std::ios::binary);
            if(!input)throw std::runtime_error("Record requires a readable --config file");
            std::string text(NSS_MAX_JSON_BYTES+1,'\0');input.read(text.data(),static_cast<std::streamsize>(text.size()));text.resize(static_cast<size_t>(input.gcount()));
            if(text.size()>NSS_MAX_JSON_BYTES)throw std::runtime_error("Recording config exceeds its byte budget");
            const auto config_value=json::parse(text);
            if(config_value.contains("audio")&&config_value["audio"].is_array())
                for(const auto &track:config_value["audio"]){
                    if(track.contains("path")&&track["path"].is_string()){
                        const auto audio_text=track["path"].get<std::string>();
                        const std::filesystem::path audio_path(std::u8string(reinterpret_cast<const char8_t*>(audio_text.data()),audio_text.size()));
                        if(_wcsicmp(audio_path.c_str(),report_path.c_str())==0)throw std::runtime_error("Report and WAV assets require distinct paths");
                    }
                }
            if(config_value.contains("outputPath")&&config_value["outputPath"].is_string()) {
                const auto asset_utf8=config_value["outputPath"].get<std::string>();
                const std::filesystem::path asset(std::u8string(reinterpret_cast<const char8_t*>(asset_utf8.data()),asset_utf8.size()));
                for(const auto& candidate:{asset.native(),asset.native()+L".manifest.json",asset.native()+L".manifest.pending"})
                    if(_wcsicmp(candidate.c_str(),report_path.native().c_str())==0)
                        throw std::runtime_error("Report and recording assets require distinct paths");
            }
            auto report=record(native,engine,config_value);report["build"]=build;
            write_report(report_path,report);std::cout<<report.dump()<<'\n';return report["status"]=="PASS"?0:1;
        }
        if (verb == "capture-probe") {
            std::ifstream input(capture_config, std::ios::binary);
            if(!input)throw std::runtime_error("Capture probe requires a readable --config file");
            std::string text(NSS_MAX_JSON_BYTES+1,'\0');input.read(text.data(),static_cast<std::streamsize>(text.size()));text.resize(static_cast<size_t>(input.gcount()));
            if(text.size()>NSS_MAX_JSON_BYTES)throw std::runtime_error("Capture probe config exceeds its byte budget");
            auto control_request=control(text);const auto result=native.capture(engine,&control_request,&blob,&diagnostic);
            if(result!=NSS_OK){std::cout<<json{{"status","FAIL"},{"code",result},{"stage",diagnostic.stage},{"hresult",diagnostic.hresult},{"message",reinterpret_cast<const char*>(diagnostic.message_utf8)}}.dump()<<'\n';return 1;}
            auto report=native.consume(blob);report["build"]=build;std::cout<<report.dump()<<'\n';return 0;
        }
        if (verb == "self-test" && scope == "session") { auto report=session_test(native,engine);report["build"]=build;std::cout<<report.dump()<<'\n';return 0; }
        if (verb == "capabilities") {
            check(native.capabilities(engine, &blob, &diagnostic) == NSS_OK, "Capabilities failed");
            std::cout << native.consume(blob).dump(-1, ' ', true) << '\n'; return 0;
        }
        if (verb == "devices") {
            json categories = json::array(), screens=json::array();
            for (uint32_t kind = NSS_SOURCE_SCREEN; kind <= NSS_SOURCE_LOOPBACK; ++kind) {
                const auto result = native.devices(engine, kind, &blob, &diagnostic);
                if (result == NSS_OK && blob) {
                    auto values = native.consume(blob).at("devices");
                    if (kind == NSS_SOURCE_SCREEN) screens = values;
                    categories.push_back({{"kind", kind}, {"status", "available"}, {"devices", values}});
                } else if (kind == NSS_SOURCE_CAMERA && result == NSS_E_UNAVAILABLE && !blob) {
                    categories.push_back({{"kind", kind}, {"status", "unavailable"}, {"code", result}});
                } else {
                    check(false, "Source enumeration failed");
                }
            }
            std::cout << json{{"status", "PASS"}, {"build", build}, {"devices", screens}, {"categories", categories}}.dump() << '\n'; return 0;
        }
        if (verb == "self-test" && scope == "software-encoder") {
            nss_utf8_view destination{reinterpret_cast<const uint8_t*>(output.data()), output.size()};
            const auto result = native.software(engine, destination, &blob, &diagnostic);
            if (result != NSS_OK) { std::cout << json{{"status", "FAIL"}, {"code", result}, {"message", reinterpret_cast<const char*>(diagnostic.message_utf8)}}.dump() << '\n'; return 1; }
            auto report = native.consume(blob); report["status"] = "PASS"; std::cout << report.dump() << '\n'; return 0;
        }
        if (verb != "self-test" || scope != "abi") throw std::runtime_error("Unsupported command/scope");
        check(native.version(nullptr) == NSS_E_ARGUMENT, "Null version was accepted");
        version.size = 8; check(native.version(&version) == NSS_E_VERSION, "Wrong envelope size was accepted");
        nss_session* session = nullptr; uint64_t command = 99;
        check(native.session(engine, &request, &session, &diagnostic) == NSS_E_UNAVAILABLE && !session, "Unavailable capture created a session");
        check(native.command(nullptr, NSS_COMMAND_START, &command, &diagnostic) == NSS_E_ARGUMENT && command == 0, "Null session start was accepted");
        check(native.capabilities(engine, &blob, &diagnostic) == NSS_OK, "Blob creation failed");
        native.release(engine); engine = nullptr;
        check(native.consume(blob)["clientName"] == client, "Copied Unicode control/blob lifetime failed");
        check(native.quiesce(0, &diagnostic) == NSS_OK, "Owned contexts/blobs leaked");
        std::cout << json{{"status", "PASS"}, {"scope", "abi"}, {"build", build}, {"actualContextAndBlobLifetime", true}, {"captureImplemented", false}}.dump() << '\n';
        return 0;
    } catch (const std::exception& failure) {
        std::cout << json{{"status", "error"}, {"stage", "bootstrap"}, {"message", failure.what()}}.dump() << '\n'; return 2;
    }
}
