#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "audio/audio.hpp"
#include "build_info.h"
#include "camera/camera.hpp"
#include "capture/capture.hpp"
#include "jobs/job.hpp"
#include "json.hpp"
#include "preview/preview.hpp"
#include "runtime/diagnostic_fixture.hpp"
#include "runtime/session.hpp"
#include "scene/scene.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <memory>
#include <ness_muxer_v2.h>
#include <nessstudio/nessstudio.h>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#include <windows.h>

using json = nlohmann::json;
struct nss_engine {
  std::string client;
  std::string diagnostics;
  std::shared_ptr<nss::EngineBudget> budget =
      std::make_shared<nss::EngineBudget>();
  std::shared_ptr<std::atomic<bool>> job_budget =
      std::make_shared<std::atomic<bool>>(false);
};
struct nss_blob {
  std::string bytes;
};
struct nss_session {
  std::unique_ptr<nss::Session> value;
  PTP_WORK release_work{};
  ~nss_session() {
    if (release_work)
      CloseThreadpoolWork(release_work);
  }
};
struct nss_job {
  std::unique_ptr<nss::jobs::Job> value;
  PTP_WORK release_work{};
  ~nss_job() {
    if (release_work)
      CloseThreadpoolWork(release_work);
  }
};
struct nss_preview {
  std::shared_ptr<nss::preview::Channel> value;
};
struct nss_preview_lease {
  std::shared_ptr<nss::preview::Frame> value;
};
namespace {
std::atomic<uint64_t> engines{}, blobs{};
#ifndef NSS_BUILD_FLAVOR
#define NSS_BUILD_FLAVOR ""
#endif
constexpr char build_id[] =
    NSS_BUILD_BASE "-" NSS_BUILD_CONFIG NSS_BUILD_FLAVOR;
template <class T> bool envelope(const T *value) noexcept {
  return value && value->size == sizeof(T) &&
         value->version == NSS_STRUCT_VERSION;
}
bool error_valid(const nss_error_v1 *error) noexcept {
  return !error || envelope(error);
}
nss_result failure(nss_error_v1 *error, nss_result code, uint32_t stage,
                   const char *message) noexcept {
  if (error && envelope(error)) {
    std::memset(error, 0, sizeof(*error));
    error->size = sizeof(*error);
    error->version = NSS_STRUCT_VERSION;
    error->code = code;
    error->stage = stage;
    const auto count =
        (std::min)(std::strlen(message), sizeof(error->message_utf8) - 1);
    // Diagnostics here are deliberately ASCII; arbitrary JSON/client text is
    // never copied into errors.
    std::memcpy(error->message_utf8, message, count);
  }
  return code;
}
void clear_error(nss_error_v1 *error) noexcept {
  (void)failure(error, NSS_OK, NSS_STAGE_NONE, "");
}
bool utf8(nss_utf8_view view, bool optional = false) noexcept {
  if (view.bytes == 0)
    return optional;
  if (!view.data || view.bytes > NSS_MAX_JSON_BYTES)
    return false;
  if (std::memchr(view.data, 0, static_cast<size_t>(view.bytes)))
    return false;
  return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                             reinterpret_cast<const char *>(view.data),
                             static_cast<int>(view.bytes), nullptr, 0) != 0;
}
bool bounded_json(nss_utf8_view input) noexcept {
  if (!utf8(input))
    return false;
  int depth = 0;
  bool quoted = false, escape = false;
  for (uint64_t i = 0; i < input.bytes; ++i) {
    const auto c = input.data[i];
    if (quoted) {
      if (escape)
        escape = false;
      else if (c == '\\')
        escape = true;
      else if (c == '"')
        quoted = false;
    } else if (c == '"')
      quoted = true;
    else if (c == '{' || c == '[') {
      if (++depth > 64)
        return false;
    } else if (c == '}' || c == ']') {
      if (--depth < 0)
        return false;
    }
  }
  return depth == 0 && !quoted;
}
template <class F>
nss_result boundary(nss_error_v1 *error, F operation) noexcept {
  if (!error_valid(error))
    return NSS_E_VERSION;
  clear_error(error);
  try {
    return operation();
  } catch (const nss::capture::Error &detail) {
    const auto result =
        failure(error, detail.code, detail.stage, detail.what());
    if (error && envelope(error))
      error->hresult = detail.native;
    return result;
  } catch (const std::bad_alloc &) {
    return failure(error, NSS_E_INTERNAL, NSS_STAGE_BOOTSTRAP,
                   "Allocation failed");
  } catch (const json::exception &) {
    return failure(error, NSS_E_ARGUMENT, NSS_STAGE_BOOTSTRAP,
                   "Invalid control JSON");
  } catch (const std::invalid_argument &) {
    return failure(error, NSS_E_ARGUMENT, NSS_STAGE_PLAN,
                   "Invalid control request");
  } catch (const std::length_error &) {
    return failure(error, NSS_E_ARGUMENT, NSS_STAGE_PLAN,
                   "Control request exceeds its budget");
  } catch (...) {
    return failure(error, NSS_E_INTERNAL, NSS_STAGE_BOOTSTRAP,
                   "Native operation failed");
  }
}
nss_result output_blob(const json &value, nss_blob **output) {
  auto result = std::make_unique<nss_blob>();
  result->bytes = value.dump();
  if (result->bytes.size() > NSS_MAX_JSON_BYTES)
    return NSS_E_BACKPRESSURE;
  *output = result.release();
  ++blobs;
  return NSS_OK;
}
nss_result unavailable(nss_error_v1 *error) noexcept {
  if (!error_valid(error))
    return NSS_E_VERSION;
  return failure(
      error, NSS_E_UNAVAILABLE, NSS_STAGE_PLAN,
      "Operation is not implemented; no recording or job was started");
}
} // namespace

extern "C" nss_result NSS_CALL nss_get_version(nss_version_v1 *output) {
  if (!output)
    return NSS_E_ARGUMENT;
  if (!envelope(output))
    return NSS_E_VERSION;
  std::memset(output, 0, sizeof(*output));
  output->size = sizeof(*output);
  output->version = NSS_STRUCT_VERSION;
  output->abi_major = NSS_ABI_MAJOR;
  output->abi_minor = NSS_ABI_MINOR;
  output->pointer_bits = sizeof(void *) * 8;
  output->contract_revision = NSS_CONTRACT_REVISION;
  static_assert(sizeof(build_id) <= sizeof(output->build_utf8));
  std::memcpy(output->build_utf8, build_id, sizeof(build_id));
  return NSS_OK;
}
extern "C" nss_result NSS_CALL nss_engine_create(const nss_json_v1 *config,
                                                 nss_engine **output,
                                                 nss_error_v1 *error) {
  if (output)
    *output = nullptr;
  return boundary(error, [&]() -> nss_result {
    if (!output || !config)
      return failure(error, NSS_E_ARGUMENT, NSS_STAGE_BOOTSTRAP,
                     "Engine configuration/output is required");
    if (!envelope(config))
      return failure(error, NSS_E_VERSION, NSS_STAGE_BOOTSTRAP,
                     "Invalid configuration envelope size/version");
    if (!bounded_json(config->json))
      return failure(error, NSS_E_ARGUMENT, NSS_STAGE_BOOTSTRAP,
                     "Expected bounded strict UTF-8 JSON without embedded NUL");
    auto parsed =
        json::parse(config->json.data, config->json.data + config->json.bytes);
    if (!parsed.is_object())
      return failure(error, NSS_E_ARGUMENT, NSS_STAGE_BOOTSTRAP,
                     "Engine configuration must be an object");
    for (const auto &item : parsed.items()) {
      if (item.key() != "version" && item.key() != "diagnostics" &&
          item.key() != "maxActiveSessions" && item.key() != "maxActiveJobs" &&
          item.key() != "clientName")
        return failure(error, NSS_E_ARGUMENT, NSS_STAGE_BOOTSTRAP,
                       "Unknown engine configuration property");
    }
    if (!parsed.contains("version") ||
        !parsed["version"].is_number_unsigned() || parsed["version"] != 1)
      return failure(error, NSS_E_VERSION, NSS_STAGE_BOOTSTRAP,
                     "Unsupported engine configuration version");
    for (const auto *key : {"maxActiveSessions", "maxActiveJobs"})
      if (parsed.contains(key) &&
          (!parsed[key].is_number_unsigned() || parsed[key] != 1))
        return failure(error, NSS_E_ARGUMENT, NSS_STAGE_BOOTSTRAP,
                       "Only an integer one-session/job budget is defined");
    auto context = std::make_unique<nss_engine>();
    context->client = parsed.value("clientName", std::string{});
    context->diagnostics = parsed.value("diagnostics", std::string{"Normal"});
    if (context->client.size() > 1024 ||
        context->client.find('\0') != std::string::npos ||
        (context->diagnostics != "Normal" && context->diagnostics != "Verbose"))
      return failure(error, NSS_E_ARGUMENT, NSS_STAGE_BOOTSTRAP,
                     "Invalid diagnostics/client name");
    // No COM, Media Foundation, capture source, GPU or encoder is initialized
    // here.
    *output = context.release();
    ++engines;
    return NSS_OK;
  });
}
extern "C" void NSS_CALL nss_engine_release(nss_engine *engine) {
  if (engine) {
    delete engine;
    --engines;
  }
}
extern "C" nss_result NSS_CALL nss_engine_capabilities(nss_engine *engine,
                                                       nss_blob **output,
                                                       nss_error_v1 *error) {
  if (output)
    *output = nullptr;
  return boundary(error, [&]() -> nss_result {
    if (!engine || !output)
      return failure(error, NSS_E_ARGUMENT, NSS_STAGE_BOOTSTRAP,
                     "Engine and blob output are required");
    return output_blob(
        json{{"version", 1},
             {"build", build_id},
             {"sourceFingerprint", NSS_SOURCE_FINGERPRINT},
             {"nessMuxerRevision", NSS_NESSMUXER_REVISION},
             {"clientName", engine->client},
             {"declaredCapabilities",
              {{"recording", !nss::test_backend_build()},
               {"deviceEnumeration", true},
               {"jobs", !nss::test_backend_build()},
               {"preview", !nss::test_backend_build()},
               {"fullSoftware", !nss::test_backend_build()}}},
             {"recordingScope",
              {{"source", "physical SDR monitor/region and independent DirectShow "
                          "camera; audio-only"},
               {"video", "H264 Baseline/MKV; independent camera MP4 "
                         "streamcopy; zero B frames"},
               {"audio", !nss::test_backend_build()},
               {"cadences", json::array({"24/1", "25/1", "30/1", "48/1", "50/1", "60/1", "30000/1001"})},
               {"gpu", "same-adapter direct QSV/NVENC/AMF; exact configuration requires Prepare"},
               {"cpu", "GDI/camera RAM + CPU NV12 + x264"},
               {"selection", "Auto probes native hardware before REC, then GPU processing + x264, then FullSoftware + x264 when GPU capture is unavailable; no mid-file switching"},
               {"executionModes",json::array({"Auto","FullSoftware","GpuWithSoftwareEncoding","RequireGpuResidentHardware"})},
               {"nativeGpuQualification", "Availability and media quality require validation on the actual adapter/driver; compilation is not a hardware certification"},
               {"productFactory",
                "native screen/camera/audio and audio-only for every supported output cadence"},
               {"interactive", true}}},
             {"sceneScope",
              {{"modes", json::array({"SeparateSources", "Program",
                                      "ProgramAndSources"})},
               {"layouts", json::array({"Screen", "Camera", "PictureInPicture",
                                        "SideBySide"})},
               {"maxSources", 2},
               {"maxSimultaneousEncoders", 3},
               {"gpuThreeEncoderVideoLimit",
                {{"maxWidth", 1280}, {"maxHeight", 720}}},
               {"softwareThreeEncoderVideoLimit",
                {{"maxWidth", 640},
                 {"maxHeight", 360},
                 {"independentSources", "SD 360p for every video track"}}},
               {"actualEncoderAvailability",
                "Prepare opens every requested encoder before Ready; no silent "
                "asset drop"},
               {"updates", "immutable revisions at render boundary; fixed "
                           "canvas and cadence"},
               {"programPreviewSourceId", 3},
               {"gpuReadback", false},
               {"hybridReadback", "GPU-processing/software-encoder route only; counted separately"},
               {"alpha", "premultiplied interpolation and blending; opaque "
                         "BT709 limited SDR output"}}},
             {"cameraScope",
              {{"api", "DirectShow native receiver; raw conversion and Windows JPEG decoder"},
               {"identity", "stable symbolic link"},
               {"input", "explicit advertised NV12/YUY2/MJPG/RGB32; software "
                         "RGB32 decode/conversion"},
               {"clock", "QPC IReferenceClock plus graph Run origin and sample stream timestamp; exposure remains estimated"},
               {"memory",
                "CPU RAM; GPU processing/encoder route performs separately "
                "counted upload, never claims GPU-resident camera input"},
               {"maxSourcesPerCamera", 1}}},
             {"previewScope",
              {{"cpu", "bounded BGRA lease; reused WriteableBitmap; no "
                       "D3DImage/GPU device requirement"},
               {"gpu", "isolated D3D11 NV12 to shared D3D9Ex surface to "
                       "D3DImage; no D3D11 pointer given to WPF"},
               {"maxWidth", 640},
               {"maxHeight", 360},
               {"maxFps", 15},
               {"slots", 3},
               {"recordingClockIndependent", true}}},
             {"audioScope",
              {{"sources", json::array({"microphone", "render-loopback"})},
               {"input", "WASAPI shared PCM/float mono/stereo"},
               {"output", "aligned RF64 PCM16 stereo 48kHz"},
               {"driftCorrectionPpmMax", 200},
               {"endpointSwitching",
                "fail mandatory source; no automatic switch"}}},
             {"jobScope",
              {{"profile", "av-basic"},
               {"profiles",
                json::array({"av-basic", "camera-remux", "audio-basic", "project-export", "thumbnail-png"})},
               {"thumbnail", "software video decode to one PNG; at most 640x360; bounded seek and cancellation"},
               {"implementation", "supervised native CreateProcessW FFmpeg; "
                                  "explicit packaged executable required"},
               {"video", "H264 stream copy"},
               {"audio",
                "PCM16 stereo 48kHz; gain 0.5 per source; no normalization"},
               {"completion",
                "complete manifest + FFmpeg exit 0 + required streams full "
                "decode and duration/frame validation + exclusive publication; "
                "inputs retained"}}},
             {"captureProcessing",
              {{"monitorEnumeration", true},
               {"gpu", "WGC+D3D11 Video Processor; requires actual "
                       "configuration probe"},
               {"cpu", "GDI+DIB+CPU NV12; requires actual configuration probe"},
               {"scope", "capture/process only; no encoded recording"}}},
             {"sessionInfrastructure", true},
             {"syntheticTestBackend", nss::test_backend_build()},
             {"moduleLifetime",
              "Process-pinned after first asynchronous session or GPU capture; "
              "all capture resources still quiesce"},
             {"encoders",
              json::array({{{"id", "qsv-h264-d3d11"},
                            {"compiled", true},
                            {"availability", "requires actual Intel adapter/driver/configuration probe"},
                            {"input", "nv12-d3d11-texture"}},
                           {{"id", "nvenc-h264-d3d11"},
                            {"compiled", true},
                            {"availability", "requires actual NVENC API 13.1 adapter/driver/configuration probe; hardware qualification pending"},
                            {"input", "nv12-d3d11-texture"}},
                           {{"id", "amf-h264-d3d11"},
                            {"compiled", true},
                            {"availability", "requires actual AMD AMF adapter/driver/configuration probe; hardware qualification pending"},
                            {"input", "nv12-d3d11-texture"}},
                           {{"id", "x264-h264-software"},
                            {"compiled", true},
                            {"availability", "requires pinned ABI164 runtime and configuration probe"},
                            {"input", "nv12-cpu"}}})},
             {"optionalVendorRuntimes", json::array({"Intel graphics media runtime for QSV", "NVIDIA NVENC driver API 13.1", "AMD AMF driver runtime", "pinned x264 ABI164 for CPU encoding"})},
             {"devices", json::array()}},
        output);
  });
}
extern "C" nss_result NSS_CALL nss_engine_devices(nss_engine *engine,
                                                  uint32_t kind,
                                                  nss_blob **output,
                                                  nss_error_v1 *error) {
  if (output)
    *output = nullptr;
  if (!error_valid(error))
    return NSS_E_VERSION;
  if (!engine || !output || kind < NSS_SOURCE_SCREEN ||
      kind > NSS_SOURCE_LOOPBACK)
    return failure(error, NSS_E_ARGUMENT, NSS_STAGE_PLAN,
                   "Invalid engine, source kind or output");
  if (kind == NSS_SOURCE_MICROPHONE || kind == NSS_SOURCE_LOOPBACK)
    return boundary(error, [&]() -> nss_result {
      return output_blob(nss::audio::devices(kind), output);
    });
  if (kind == NSS_SOURCE_CAMERA)
    return boundary(error, [&]() -> nss_result {
      return output_blob(nss::camera::devices(), output);
    });
  if (kind != NSS_SOURCE_SCREEN)
    return unavailable(error);
  return boundary(error, [&]() -> nss_result {
    json monitors = json::array();
    for (const auto &display : nss::capture::displays())
      monitors.push_back(nss::capture::display_json(display));
    return output_blob(json{{"version", 1},
                            {"kind", kind},
                            {"devices", monitors},
                            {"acceleratedDeviceCreated", false}},
                       output);
  });
}
extern "C" nss_result NSS_CALL nss_probe_capture(nss_engine *engine,
                                                 const nss_json_v1 *request,
                                                 nss_blob **out,
                                                 nss_error_v1 *error) {
  if (out)
    *out = nullptr;
  return boundary(error, [&]() -> nss_result {
    if (!engine || !out || !request)
      return failure(error, NSS_E_ARGUMENT, NSS_STAGE_PLAN,
                     "Capture probe requires an engine, request and output");
    if (!envelope(request))
      return failure(error, NSS_E_VERSION, NSS_STAGE_PLAN,
                     "Invalid capture probe envelope");
    if (!bounded_json(request->json))
      return failure(error, NSS_E_ARGUMENT, NSS_STAGE_PLAN,
                     "Expected bounded capture probe JSON");
    const auto control = json::parse(request->json.data,
                                     request->json.data + request->json.bytes);
    bool expected = false;
    if (!engine->budget->active_session.compare_exchange_strong(expected, true))
      return failure(error, NSS_E_BUSY, NSS_STAGE_PLAN,
                     "Engine already owns an active session or capture probe");
    struct release_budget {
      std::shared_ptr<nss::EngineBudget> budget;
      ~release_budget() { budget->active_session = false; }
    } reservation{engine->budget};
    return output_blob(nss::capture::probe(control), out);
  });
}
extern "C" nss_result NSS_CALL nss_blob_view(const nss_blob *blob,
                                             nss_utf8_view *output) {
  if (!output)
    return NSS_E_ARGUMENT;
  *output = {};
  if (!blob)
    return NSS_E_ARGUMENT;
  output->data = reinterpret_cast<const uint8_t *>(blob->bytes.data());
  output->bytes = blob->bytes.size();
  return NSS_OK;
}
extern "C" void NSS_CALL nss_blob_release(nss_blob *blob) {
  if (blob) {
    delete blob;
    --blobs;
  }
}
extern "C" nss_result NSS_CALL nss_runtime_quiesce(uint32_t timeout,
                                                   nss_error_v1 *error) {
  return boundary(error, [&]() -> nss_result {
    if (timeout > 1000)
      return failure(error, NSS_E_ARGUMENT, NSS_STAGE_CONTROL,
                     "Quiescence timeout must be between 0 and 1000 ms");
    const auto end =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout);
    auto encoder_leases_alive = [] {
      ness_gpu_runtime_stats_v2 stats{};
      stats.size = sizeof(stats);
      stats.version = 2;
      return ness_gpu_runtime_stats_get_v2(&stats, nullptr) != 0 ||
             stats.completion_states || stats.input_leases;
    };
    while (engines.load() || blobs.load() || nss::live_sessions.load() ||
           nss::live_workers.load() || nss::live_checkpoint_workers.load() || nss::pending_reapers.load() ||
           nss::session_client_refs.load() || nss::audio::live_sources.load() ||
           nss::camera::live_sources.load() ||
           nss::preview::live_channels.load() ||
           nss::preview::live_client_leases.load() ||
           nss::preview::live_bytes.load() || nss::jobs::live_jobs.load() ||
           nss::jobs::workers.load() || nss::jobs::children.load() ||
           nss::live_frame_leases.load() || nss::live_pool_bytes.load() ||
           nss::capture::live_pipelines.load() ||
           nss::capture::live_surfaces.load() ||
           nss::capture::live_capture_bytes.load() ||
           nss::scene::live_renderers.load() || nss::scene::live_bytes.load() ||
           encoder_leases_alive()) {
      if (std::chrono::steady_clock::now() >= end)
        return failure(
            error, NSS_TIMEOUT, NSS_STAGE_CONTROL,
            "Contexts, sessions, blobs, workers or frame leases remain alive");
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    clear_error(error);
    return NSS_OK;
  });
}
extern "C" nss_result NSS_CALL nss_probe_software_encoder(nss_engine *engine,
                                                          nss_utf8_view path,
                                                          nss_blob **output,
                                                          nss_error_v1 *error) {
  if (output)
    *output = nullptr;
  return boundary(error, [&]() -> nss_result {
    if (!engine || !output || !utf8(path) || path.bytes > 131072)
      return failure(
          error, NSS_E_ARGUMENT, NSS_STAGE_ENCODER,
          "Engine, valid output path and report output are required");
    std::string filename(reinterpret_cast<const char *>(path.data),
                         static_cast<size_t>(path.bytes));
    auto destination = std::filesystem::path(std::u8string(
        reinterpret_cast<const char8_t *>(filename.data()), filename.size()));
    if (!destination.is_absolute() || std::filesystem::exists(destination))
      return failure(error, NSS_E_IO, NSS_STAGE_OUTPUT,
                     "Diagnostic output must be a new absolute path");
    constexpr int width = 320, height = 240, count = 60, fps = 30;
    const auto packets = nss::software_fixture(filename);
    if (packets != count ||
        !std::filesystem::exists(destination) ||
        std::filesystem::file_size(destination) == 0)
      return failure(
          error, NSS_E_IO, NSS_STAGE_OUTPUT,
          "Software diagnostic did not finalize the expected packets");
    return output_blob(json{{"version", 1},
                            {"build", build_id},
                            {"encoder", "x264-h264-software"},
                            {"source", "generated-nv12-fixture"},
                            {"captureImplemented", false},
                            {"hardwareDeviceCreated", false},
                            {"framesSubmitted", count},
                            {"packets", packets},
                            {"width", width},
                            {"height", height},
                            {"fps", fps},
                            {"path", filename},
                            {"nessMuxerRevision", NSS_NESSMUXER_REVISION}},
                       output);
  });
}

extern "C" nss_result NSS_CALL nss_session_create(nss_engine *engine,
                                                  const nss_json_v1 *request,
                                                  nss_session **out,
                                                  nss_error_v1 *error) {
  if (out)
    *out = nullptr;
  return boundary(error, [&]() -> nss_result {
    if (!engine || !out || !request)
      return failure(error, NSS_E_ARGUMENT, NSS_STAGE_PLAN,
                     "Engine, session request and output are required");
    if (!envelope(request))
      return failure(error, NSS_E_VERSION, NSS_STAGE_PLAN,
                     "Invalid session envelope");
    if (!bounded_json(request->json))
      return failure(error, NSS_E_ARGUMENT, NSS_STAGE_PLAN,
                     "Expected bounded UTF-8 session JSON");
    auto parsed = json::parse(request->json.data,
                              request->json.data + request->json.bytes);
    if (!parsed.is_object() || !parsed.contains("version") ||
        !parsed["version"].is_number_unsigned() || parsed["version"] != 1)
      return failure(error, NSS_E_VERSION, NSS_STAGE_PLAN,
                     "Unsupported session request version");
    nss::Fault diagnostic;
    auto backend = nss::make_backend(parsed, diagnostic);
    if (!backend)
      return failure(error, diagnostic.code, diagnostic.stage,
                     diagnostic.message.c_str());
    bool expected = false;
    if (!engine->budget->active_session.compare_exchange_strong(expected, true))
      return failure(error, NSS_E_BUSY, NSS_STAGE_PLAN,
                     "Engine already owns an active session");
    struct rollback {
      std::shared_ptr<nss::EngineBudget> budget;
      bool armed{true};
      ~rollback() {
        if (armed)
          budget->active_session = false;
      }
    } reserved{engine->budget};
    if (nss::live_sessions.load() >= 64)
      return failure(error, NSS_E_BUSY, NSS_STAGE_PLAN,
                     "Runtime session object budget exhausted");
    auto session = std::make_unique<nss_session>();
    try {
      session->value = std::make_unique<nss::Session>(
          engine->budget, std::move(backend), std::move(parsed));
    } catch (const nss::SessionBudgetExceeded &) {
      return failure(error, NSS_E_BUSY, NSS_STAGE_PLAN,
                     "Runtime session object budget exhausted");
    }
    reserved.armed = false;
    session->release_work = CreateThreadpoolWork(
        [](PTP_CALLBACK_INSTANCE, void *context, PTP_WORK) {
          delete static_cast<nss_session *>(context);
          --nss::pending_reapers;
        },
        session.get(), nullptr);
    if (!session->release_work)
      return failure(error, NSS_E_INTERNAL, NSS_STAGE_CONTROL,
                     "Cannot allocate native session cleanup work");
    // Emergency release may outlive the calling LoadLibrary owner. Pinning is
    // explicit and process-scoped; quiescence still requires all session
    // workers/resources gone.
    static const wchar_t module_anchor[] = L"NessStudio session runtime";
    HMODULE module{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_PIN,
                            module_anchor, &module))
      return failure(error, NSS_E_INTERNAL, NSS_STAGE_CONTROL,
                     "Cannot retain session runtime module");
    session->value->start_worker();
    ++nss::session_client_refs;
    *out = session.release();
    return NSS_OK;
  });
}
extern "C" nss_result NSS_CALL nss_session_command(nss_session *session,
                                                   uint32_t command,
                                                   uint64_t *id,
                                                   nss_error_v1 *error) {
  if (id)
    *id = 0;
  return boundary(error, [&]() -> nss_result {
    if (!session || !id)
      return failure(error, NSS_E_ARGUMENT, NSS_STAGE_CONTROL,
                     "Session and command output are required");
    nss::Fault detail;
    const auto result = session->value->command(command, *id, detail);
    if (result < 0)
      return failure(error, result, detail.stage, detail.message.c_str());
    return result;
  });
}
extern "C" nss_result NSS_CALL nss_session_snapshot(nss_session *session,
                                                    nss_blob **out,
                                                    nss_error_v1 *error) {
  if (out)
    *out = nullptr;
  return boundary(error, [&]() -> nss_result {
    if (!session || !out)
      return failure(error, NSS_E_ARGUMENT, NSS_STAGE_CONTROL,
                     "Session and snapshot output are required");
    return output_blob(session->value->snapshot(), out);
  });
}
extern "C" nss_result NSS_CALL nss_session_update_scene(nss_session *session,
                                                        nss_utf8_view input,
                                                        uint64_t *revision,
                                                        nss_error_v1 *error) {
  if (revision)
    *revision = 0;
  return boundary(error, [&]() -> nss_result {
    if (!session || !revision || !utf8(input) || !bounded_json(input))
      return failure(
          error, NSS_E_ARGUMENT, NSS_STAGE_PLAN,
          "Session, bounded scene JSON and revision output required");
    *revision = session->value->update_scene(
        json::parse(input.data, input.data + input.bytes));
    if (!*revision)
      return unavailable(error);
    return NSS_OK;
  });
}
extern "C" nss_result NSS_CALL nss_session_poll_event(nss_session *session,
                                                      uint32_t timeout,
                                                      nss_event_v1 *out,
                                                      nss_error_v1 *error) {
  return boundary(error, [&]() -> nss_result {
    if (!session || !out)
      return failure(error, NSS_E_ARGUMENT, NSS_STAGE_CONTROL,
                     "Session and event envelope are required");
    if (!envelope(out))
      return failure(error, NSS_E_VERSION, NSS_STAGE_CONTROL,
                     "Invalid event envelope size/version");
    nss::Fault detail;
    const auto result = session->value->poll(timeout, *out, detail);
    if (result < 0 && detail)
      return failure(error, result, detail.stage, detail.message.c_str());
    return result;
  });
}
extern "C" void NSS_CALL nss_session_interrupt_poll(nss_session *session) {
  if (session)
    session->value->interrupt();
}
extern "C" void NSS_CALL nss_session_release(nss_session *session) {
  if (!session)
    return;
  --nss::session_client_refs;
  ++nss::pending_reapers;
  session->value->abandon();
  SubmitThreadpoolWork(session->release_work);
}
// Preview handles attach to the selected session source and retain its lifetime.
extern "C" nss_result NSS_CALL
nss_preview_attach(nss_session *session, const nss_preview_config_v1 *config,
                   nss_preview **out, nss_error_v1 *e) {
  if (out)
    *out = nullptr;
  return boundary(e, [&]() -> nss_result {
    if (!session || !config || !out)
      return failure(e, NSS_E_ARGUMENT, NSS_STAGE_PREVIEW,
                     "Session, preview configuration and output required");
    if (!envelope(config))
      return failure(e, NSS_E_VERSION, NSS_STAGE_PREVIEW,
                     "Invalid preview envelope");
    auto hub = session->value->preview(config->source_id);
    if (!hub)
      return unavailable(e);
    auto preview = std::make_unique<nss_preview>();
    preview->value = hub->attach(*config);
    *out = preview.release();
    return NSS_OK;
  });
}
extern "C" nss_result NSS_CALL nss_preview_set_enabled(nss_preview *preview,
                                                       uint32_t enabled,
                                                       nss_error_v1 *error) {
  return boundary(error, [&]() -> nss_result {
    if (!preview || enabled > 1)
      return failure(error, NSS_E_ARGUMENT, NSS_STAGE_PREVIEW,
                     "Preview and boolean enabled state required");
    preview->value->set_enabled(enabled != 0);
    return NSS_OK;
  });
}
extern "C" nss_result NSS_CALL nss_preview_acquire(nss_preview *preview,
                                                   nss_preview_lease **out,
                                                   nss_preview_frame_v1 *frame,
                                                   nss_error_v1 *e) {
  if (out)
    *out = nullptr;
  return boundary(e, [&]() -> nss_result {
    if (!preview || !out || !frame)
      return failure(e, NSS_E_ARGUMENT, NSS_STAGE_PREVIEW,
                     "Preview, lease output and frame required");
    if (!envelope(frame))
      return failure(e, NSS_E_VERSION, NSS_STAGE_PREVIEW,
                     "Invalid CPU preview frame envelope");
    auto value = preview->value->acquire(false);
    if (!value)
      return NSS_TIMEOUT;
    auto lease = std::make_unique<nss_preview_lease>();
    lease->value = std::move(value);
    *frame = lease->value->cpu;
    ++nss::preview::live_client_leases;
    *out = lease.release();
    return NSS_OK;
  });
}
extern "C" nss_result NSS_CALL
nss_gpu_preview_acquire(nss_preview *preview, nss_preview_lease **out,
                        nss_gpu_surface_v1 *frame, nss_error_v1 *e) {
  if (out)
    *out = nullptr;
  return boundary(e, [&]() -> nss_result {
    if (!preview || !out || !frame)
      return failure(e, NSS_E_ARGUMENT, NSS_STAGE_PREVIEW,
                     "Preview, lease output and GPU surface required");
    if (!envelope(frame))
      return failure(e, NSS_E_VERSION, NSS_STAGE_PREVIEW,
                     "Invalid GPU preview envelope");
    auto value = preview->value->acquire(true);
    if (!value)
      return NSS_TIMEOUT;
    auto lease = std::make_unique<nss_preview_lease>();
    lease->value = std::move(value);
    *frame = lease->value->gpu;
    ++nss::preview::live_client_leases;
    *out = lease.release();
    return NSS_OK;
  });
}
extern "C" nss_result NSS_CALL nss_preview_detach(nss_preview *preview,
                                                  nss_error_v1 *e) {
  return boundary(e, [&]() -> nss_result {
    if (!preview)
      return failure(e, NSS_E_ARGUMENT, NSS_STAGE_PREVIEW, "Preview required");
    preview->value->detach();
    return NSS_OK;
  });
}
extern "C" void NSS_CALL nss_preview_lease_release(nss_preview_lease *lease) {
  if (lease) {
    delete lease;
    --nss::preview::live_client_leases;
  }
}
extern "C" void NSS_CALL nss_preview_release(nss_preview *preview) {
  if (preview) {
    preview->value->detach();
    delete preview;
  }
}
extern "C" nss_result NSS_CALL nss_job_create(nss_engine *engine,
                                              const nss_json_v1 *request,
                                              nss_job **out, nss_error_v1 *e) {
  if (out)
    *out = nullptr;
  return boundary(e, [&]() -> nss_result {
    if (!engine || !request || !out)
      return failure(e, NSS_E_ARGUMENT, NSS_STAGE_JOB,
                     "Job engine/request/output required");
    if (!envelope(request))
      return failure(e, NSS_E_VERSION, NSS_STAGE_JOB, "Invalid job envelope");
    if (!bounded_json(request->json))
      return failure(e, NSS_E_ARGUMENT, NSS_STAGE_JOB,
                     "Invalid bounded job JSON");
    const auto parsed = json::parse(request->json.data,
                                    request->json.data + request->json.bytes);
    if (!parsed.is_object() || !parsed.contains("version") ||
        !parsed["version"].is_number_unsigned() || parsed["version"] != 1)
      return failure(e, NSS_E_VERSION, NSS_STAGE_PLAN,
                     "Unsupported export job version");
    if (nss::test_backend_build())
      return unavailable(e);
    if (nss::jobs::live_jobs >= 64)
      return failure(e, NSS_E_BUSY, NSS_STAGE_JOB,
                     "Job object budget exhausted");
    bool expected = false;
    if (!engine->job_budget->compare_exchange_strong(expected, true))
      return failure(e, NSS_E_BUSY, NSS_STAGE_JOB,
                     "Engine already owns an active job");
    struct Rollback {
      std::shared_ptr<std::atomic<bool>> budget;
      bool armed{true};
      ~Rollback() {
        if (armed)
          *budget = false;
      }
    } rollback{engine->job_budget};
    auto job = std::make_unique<nss_job>();
    job->value = std::make_unique<nss::jobs::Job>(parsed, engine->job_budget);
    job->release_work = CreateThreadpoolWork(
        [](PTP_CALLBACK_INSTANCE, void *context, PTP_WORK) {
          delete static_cast<nss_job *>(context);
          --nss::pending_reapers;
        },
        job.get(), nullptr);
    if (!job->release_work)
      return failure(e, NSS_E_INTERNAL, NSS_STAGE_JOB,
                     "Cannot allocate job cleanup work");
    static const wchar_t anchor[] = L"NessStudio job runtime";
    HMODULE module{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_PIN,
                            anchor, &module))
      return failure(e, NSS_E_INTERNAL, NSS_STAGE_JOB,
                     "Cannot retain job runtime");
    rollback.armed = false;
    *out = job.release();
    return NSS_OK;
  });
}
extern "C" nss_result NSS_CALL nss_job_start(nss_job *job, uint64_t *id,
                                             nss_error_v1 *e) {
  if (id)
    *id = 0;
  return boundary(e, [&]() -> nss_result {
    if (!job || !id)
      return failure(e, NSS_E_ARGUMENT, NSS_STAGE_JOB,
                     "Job and command output required");
    const auto r = job->value->start(*id);
    return r < 0 ? failure(e, r, NSS_STAGE_JOB,
                           "Job cannot start in its current state")
                 : r;
  });
}
extern "C" nss_result NSS_CALL nss_job_cancel(nss_job *job, uint64_t *id,
                                              nss_error_v1 *e) {
  if (id)
    *id = 0;
  return boundary(e, [&]() -> nss_result {
    if (!job || !id)
      return failure(e, NSS_E_ARGUMENT, NSS_STAGE_JOB,
                     "Job and command output required");
    const auto r = job->value->cancel(*id);
    return r < 0 ? failure(e, r, NSS_STAGE_JOB,
                           "Job cannot cancel in its current state")
                 : r;
  });
}
extern "C" nss_result NSS_CALL nss_job_snapshot(nss_job *job, nss_blob **out,
                                                nss_error_v1 *e) {
  if (out)
    *out = nullptr;
  return boundary(e, [&]() -> nss_result {
    if (!job || !out)
      return failure(e, NSS_E_ARGUMENT, NSS_STAGE_JOB,
                     "Job and snapshot output required");
    return output_blob(job->value->snapshot(), out);
  });
}
extern "C" nss_result NSS_CALL nss_job_poll_event(nss_job *job,
                                                  uint32_t timeout,
                                                  nss_event_v1 *out,
                                                  nss_error_v1 *e) {
  return boundary(e, [&]() -> nss_result {
    if (!job || !out)
      return failure(e, NSS_E_ARGUMENT, NSS_STAGE_JOB,
                     "Job and event required");
    if (!envelope(out))
      return failure(e, NSS_E_VERSION, NSS_STAGE_JOB, "Invalid event envelope");
    const auto r = job->value->poll(timeout, *out);
    return r < 0 ? failure(e, r, NSS_STAGE_JOB,
                           r == NSS_E_BUSY ? "Job already has an active poller"
                           : r == NSS_E_CANCELLED ? "Job poll interrupted"
                                                  : "Invalid job poll timeout")
                 : r;
  });
}
extern "C" void NSS_CALL nss_job_interrupt_poll(nss_job *job) {
  if (job)
    job->value->interrupt();
}
extern "C" void NSS_CALL nss_job_release(nss_job *job) {
  if (!job)
    return;
  ++nss::pending_reapers;
  job->value->abandon();
  SubmitThreadpoolWork(job->release_work);
}
