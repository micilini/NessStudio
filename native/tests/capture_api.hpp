#pragma once
#include "../common/bootstrap.hpp"
#include "capture/capture.hpp"
#include <future>
#include <objbase.h>

inline nss::capture::Json
capture_api_probe(bool gpu, const std::filesystem::path &config_file = {}) {
  using Json = nss::capture::Json;
  nss_bootstrap::module library(nss_bootstrap::directory() /
                                L"libnessstudio.dll");
  const auto create =
      library.get<decltype(&nss_engine_create)>("nss_engine_create");
  const auto release =
      library.get<decltype(&nss_engine_release)>("nss_engine_release");
  const auto probe =
      library.get<decltype(&nss_probe_capture)>("nss_probe_capture");
  const auto devices =
      library.get<decltype(&nss_engine_devices)>("nss_engine_devices");
  const auto view = library.get<decltype(&nss_blob_view)>("nss_blob_view");
  const auto free_blob =
      library.get<decltype(&nss_blob_release)>("nss_blob_release");
  const auto quiesce =
      library.get<decltype(&nss_runtime_quiesce)>("nss_runtime_quiesce");
  auto error = nss_error_v1{sizeof(nss_error_v1), 1};
  auto envelope = [](const std::string &value) {
    return nss_json_v1{
        sizeof(nss_json_v1),
        1,
        {reinterpret_cast<const uint8_t *>(value.data()), value.size()}};
  };
  auto consume = [&](nss_blob *blob) {
    nss_utf8_view data{};
    fixture::check(view(blob, &data) == 0, "Capture ABI blob view failed");
    auto result = Json::parse(data.data, data.data + data.bytes);
    free_blob(blob);
    return result;
  };
  const auto initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  fixture::check(SUCCEEDED(initialized),
                 "ABI qualification requires a caller STA");
  struct com {
    ~com() { CoUninitialize(); }
  } apartment;
  const std::string client =
      "{\"version\":1,\"clientName\":\"capture-ABI-STA\"}";
  auto config = envelope(client);
  nss_engine *engine{};
  fixture::check(create(&config, &engine, &error) == NSS_OK,
                 "Capture ABI engine creation failed");
  struct context {
    nss_engine *&value;
    decltype(release) close;
    ~context() {
      if (value)
        close(value);
    }
  } owner{engine, release};
  nss_blob *output{};
  fixture::check(devices(engine, NSS_SOURCE_SCREEN, &output, &error) == 0,
                 "Capture ABI screen enumeration failed");
  const auto screens = consume(output);
  fixture::check(!screens.at("devices").empty() &&
                     !screens.at("acceleratedDeviceCreated").get<bool>(),
                 "Enumeration created a GPU or omitted real screens");
  Json control = {{"version", 1},
                  {"mode", gpu ? "RequireGpuResidentHardware" : "FullSoftware"},
                  {"cursor", false},
                  {"durationMs", 1000},
                  {"fpsNum", 30000},
                  {"fpsDen", 1001},
                  {"trace", true},
                  {"output", {{"width", 640}, {"height", 400}}}};
  if (!config_file.empty()) {
    std::ifstream file(config_file);
    fixture::check(static_cast<bool>(file), "ABI config input missing");
    file >> control;
  }
  Json rejection = Json::array();
  for (const auto &invalid :
       {Json{{"version", 2}}, Json{{"version", 1}, {"mode", "AUTO"}},
        Json{{"version", 1}, {"fpsNum", 30.5}},
        Json{{"version", 1}, {"durationMs", 100.5}},
        Json{{"version", 1}, {"output", {{"width", 333}, {"height", 241}}}},
        Json{{"version", 1},
             {"crop", {{"x", 0.5}, {"y", 0}, {"width", 100}, {"height", 100}}}},
        Json{{"version", 1}, {"maxBytes", 1}},
        Json{{"version", 1},
             {"output",
              {{"width", 640}, {"height", 400}, {"ignored", true}}}}}) {
    const auto text = invalid.dump();
    auto request = envelope(text);
    output = reinterpret_cast<nss_blob *>(1);
    const auto result = probe(engine, &request, &output, &error);
    fixture::check(result < 0 && !output && error.stage == NSS_STAGE_PLAN,
                   "Malformed capture configuration opened resources or lost "
                   "its plan error");
    rejection.push_back(
        {{"request", invalid}, {"code", result}, {"stage", error.stage}});
  }
  const auto text = control.dump();
  auto request = envelope(text);
  const auto probe_result = probe(engine, &request, &output, &error);
  if (probe_result != NSS_OK)
    throw nss::capture::Error(
        probe_result, error.stage, error.hresult,
        reinterpret_cast<const char *>(error.message_utf8));
  auto result = consume(output);
  fixture::check(
      result.at("scope") == "capture-processing-only" &&
          result.at("missingSlots") == 0 && result.at("livePipelines") == 0 &&
          result.at("liveSurfaces") == 0 && result.at("liveCaptureBytes") == 0,
      "Capture ABI failed timing or resource quiescence");
  APTTYPE type{};
  APTTYPEQUALIFIER qualifier{};
  fixture::check(SUCCEEDED(CoGetApartmentType(&type, &qualifier)) &&
                     (type == APTTYPE_STA || type == APTTYPE_MAINSTA),
                 "Capture ABI consumed caller COM initialization");
  release(engine);
  engine = nullptr;
  fixture::check(quiesce(1000, &error) == 0,
                 "Capture ABI objects did not quiesce");
  result["callerStaPreserved"] = true;
  result["configurationRejections"] = rejection;
  result["productionDll"] = true;
  result["processId"] = GetCurrentProcessId();
  return result;
}
