#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "audio/audio.hpp"
#include "camera/camera.hpp"
#include "preview/preview.hpp"
#include "recording.hpp"
#include <filesystem>
#include <windows.h>

namespace nss {
namespace {
std::filesystem::path path(const std::string &value) {
  if (value.empty() || value.find('\0') != std::string::npos ||
      value.size() > 32760)
    throw std::invalid_argument("Invalid auxiliary output path");
  return std::filesystem::path(std::u8string(
      reinterpret_cast<const char8_t *>(value.data()), value.size()));
}
void fields(const Json &request, std::initializer_list<const char *> allowed) {
  if (!request.is_object() || request.at("version") != 1)
    throw std::invalid_argument("Expected a version 1 auxiliary plan");
  for (const auto &item : request.items()) {
    if (std::find(allowed.begin(), allowed.end(), item.key()) == allowed.end())
      throw std::invalid_argument("Unknown auxiliary plan field");
  }
}
class Auxiliary final : public Backend {
  struct Video {
    uint64_t id;
    capture::Plan output;
    std::optional<camera::Plan> camera;
    std::unique_ptr<capture::Pipeline> source;
    std::shared_ptr<preview::Hub> hub = std::make_shared<preview::Hub>();
    bool admitted{};
  };
  std::vector<Video> videos_;
  Json audio_plan_ = Json::array(), audio_final_ = Json::array();
  std::unique_ptr<audio::Session> audio_;
  SessionClock *clock_{};
  mutable std::mutex gate_;
  Json published_ = Json::object();
  std::string manifest_;
  int64_t last_publish_{}, duration_{};
  bool preview_only_{}, prepared_{};
  std::atomic<bool> cancelled_{}, admitted_{}, complete_{};
  template <class F> Fault guard(F action) {
    try {
      action();
      return {};
    } catch (const capture::Error &e) {
      cancelled_ = true;
      return {e.code, e.stage, e.native, e.what()};
    } catch (const std::exception &e) {
      cancelled_ = true;
      return {NSS_E_IO, preview_only_ ? NSS_STAGE_CAPTURE : NSS_STAGE_AUDIO, 0,
              e.what()};
    }
  }
  void publish() {
    Json sources = Json::array();
    for (const auto &video : videos_)
      sources.push_back(
          {{"sourceId", video.id},
           {"capture", video.source ? video.source->metrics() : Json::object()},
           {"preview", video.hub->metrics()}});
    Json value = {{"previewOnly", preview_only_},
                  {"sources", sources},
                  {"audio", audio_ ? audio_->snapshot() : audio_final_},
                  {"usefulDurationHns", duration_},
                  {"submittedFrames", 0},
                  {"boundedVideoSources", 2},
                  {"gpuToCpuReadbackBytes", 0}};
    std::lock_guard lock(gate_);
    published_ = std::move(value);
    last_publish_ = qpc_hns();
  }
  void poll() {
    for (auto &video : videos_) {
      video.hub->pump();
      if (auto frame = video.source->poll()) {
        video.admitted = true;
        video.hub->offer(*frame);
      }
    }
  }
  void persist() {
    const auto output = path(manifest_), pending = path(manifest_ + ".pending");
    const auto text = Json{{"version", 1},
                           {"kind", "audio-only"},
                           {"sessionId", session_id()},
                           {"mediaComplete", true},
                           {"finalized", true},
                           {"durationHns", duration_},
                           {"audio", audio_final_},
                           {"firstError", nullptr}}
                          .dump(2) +
                      "\n";
    HANDLE file = CreateFileW(pending.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
      throw capture::Error(NSS_E_IO, NSS_STAGE_STORAGE,
                           HRESULT_FROM_WIN32(GetLastError()),
                           "Cannot reserve audio manifest");
    DWORD written{};
    const bool saved =
        WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written,
                  nullptr) &&
        written == text.size() && FlushFileBuffers(file);
    const auto error = GetLastError();
    const bool closed = CloseHandle(file) != FALSE;
    if (!saved || !closed ||
        !MoveFileExW(pending.c_str(), output.c_str(), MOVEFILE_WRITE_THROUGH))
      throw capture::Error(NSS_E_IO, NSS_STAGE_STORAGE,
                           HRESULT_FROM_WIN32(error),
                           "Cannot publish audio manifest");
  }

public:
  explicit Auxiliary(const Json &request) {
    preview_only_ = request.value("previewOnly", false);
    if (preview_only_) {
      fields(request, {"version", "previewOnly", "capture", "camera", "mode"});
      if (!request.contains("capture") && !request.contains("camera"))
        throw std::invalid_argument("Idle preview needs at least one source");
      videos_.reserve(2);
      if (request.contains("capture")) {
        auto output = capture::plan(request.at("capture"));
        videos_.push_back({1, output, std::nullopt, nullptr,
                           std::make_shared<preview::Hub>(), false});
      }
      if (request.contains("camera")) {
        const auto input = camera::plan(request.at("camera"));
        const auto mode = request.at("mode").get<std::string>();
        if (mode != "FullSoftware" && mode != "RequireGpuResidentHardware")
          throw std::invalid_argument(
              "Idle preview requires explicit execution mode");
        capture::Plan output;
        output.gpu = mode == "RequireGpuResidentHardware";
        output.crop = {0, 0, static_cast<int32_t>(input.width),
                       static_cast<int32_t>(input.height)};
        output.width = output.requested_width = 640;
        output.height = output.requested_height = 360;
        output.viewport =
            capture::fit_viewport(input.width, input.height, 640, 360);
        output.slots = output.gpu ? 6 : 3;
        if (output.gpu) {
          const auto displays = capture::displays();
          const auto display =
              std::find_if(displays.begin(), displays.end(),
                           [](const auto &item) { return item.primary; });
          if (display == displays.end())
            throw capture::Error(NSS_E_UNAVAILABLE, NSS_STAGE_CAPTURE, 0,
                                 "No adapter for GPU camera preview");
          output.display = *display;
        }
        videos_.push_back({2, output, input, nullptr,
                           std::make_shared<preview::Hub>(), false});
      }
    } else {
      fields(request, {"version", "audio", "manifestPath", "interactive"});
      if (request.value("interactive", false) != true)
        throw std::invalid_argument(
            "Audio-only recording requires interactive Stop");
      manifest_ = request.at("manifestPath").get<std::string>();
      const auto output = path(manifest_);
      if (!output.is_absolute() ||
          !std::filesystem::is_directory(output.parent_path()) ||
          std::filesystem::exists(output) ||
          std::filesystem::exists(path(manifest_ + ".pending")))
        throw std::invalid_argument("Audio manifest needs a new absolute path");
      audio_plan_ = audio::validate(request.at("audio"), manifest_);
      if (std::filesystem::space(output.parent_path()).available < 50ULL * 1024 * 1024)
        throw capture::Error(NSS_E_IO, NSS_STAGE_STORAGE, 0,
                             "Not enough free disk space to start recording; at least 50 MiB are required");
    }
  }
  bool self_paced() const noexcept override { return true; }
  Fault prepare(std::stop_token stop) override {
    return guard([&] {
      if (stop.stop_requested())
        throw capture::Error(NSS_E_CANCELLED, NSS_STAGE_PLAN, 0,
                             "Auxiliary preparation cancelled");
      if (!audio_plan_.empty()) {
        audio_ = std::make_unique<audio::Session>(audio_plan_);
        audio_->prepare();
      }
      for (auto &video : videos_) {
        video.hub->bind(session_id(), video.id);
        video.source =
            video.camera
                ? (video.output.gpu
                       ? capture::make_camera_gpu(*video.camera, video.output)
                       : camera::make_cpu(*video.camera, video.output))
            : video.output.gpu ? capture::make_gpu(video.output)
                               : capture::make_cpu(video.output);
      }
      prepared_ = true;
      publish();
    });
  }
  Fault start(std::stop_token stop, int64_t, SessionClock &clock) override {
    return guard([&] {
      clock_ = &clock;
      if (audio_) {
        audio_->start(clock);
        audio_->check();
      }
      const auto deadline = qpc_hns() + 5 * hns_per_second;
      if (audio_) {
        for (;;) {
          audio_->check();
          const auto tracks = audio_->snapshot();
          if (std::all_of(tracks.begin(), tracks.end(), [](const auto &track) {
                return track.value("packets", uint64_t{}) > 0;
              }))
            break;
          if (stop.stop_requested() || qpc_hns() > deadline)
            throw capture::Error(NSS_E_TIMEOUT, NSS_STAGE_AUDIO, 0,
                                 "Audio-only did not admit first packets");
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
      }
      while (!videos_.empty() &&
             !std::all_of(videos_.begin(), videos_.end(),
                          [](const auto &value) { return value.admitted; })) {
        if (stop.stop_requested())
          throw capture::Error(NSS_E_CANCELLED, NSS_STAGE_CAPTURE, 0,
                               "Preview Start cancelled");
        poll();
        if (qpc_hns() > deadline)
          throw capture::Error(NSS_E_TIMEOUT, NSS_STAGE_CAPTURE, 0,
                               "Preview did not receive first content");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      admitted_ = true;
      publish();
    });
  }
  Fault pause(std::stop_token) override { return {}; }
  Fault resume(std::stop_token, SessionClock &) override { return {}; }
  Fault tick(SessionClock &clock) override {
    return guard([&] {
      poll();
      if (audio_)
        audio_->check();
      duration_ = clock.active_time(qpc_hns());
      if (qpc_hns() - last_publish_ >= hns_per_second / 5)
        publish();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    });
  }
  ShutdownProgress shutdown(ShutdownStage stage) override {
    return {false, guard([&] {
              if (stage == ShutdownStage::Sources) {
                if (clock_)
                  duration_ = clock_->active_time(qpc_hns());
                for (auto &video : videos_) {
                  video.hub->close();
                  if (video.source)
                    video.source->close();
                }
                if (audio_)
                  audio_->stop();
              }
              if (stage == ShutdownStage::Output && !preview_only_ &&
                  prepared_ && admitted_ && !cancelled_ && duration_ > 0) {
                audio_->finalize(duration_);
                audio_final_ = audio_->snapshot();
                persist();
                complete_ = true;
              }
              publish();
              if (stage == ShutdownStage::Resources) {
                for (auto &video : videos_)
                  video.source.reset();
                audio_.reset();
              }
            })};
  }
  Json metrics() const override {
    std::lock_guard lock(gate_);
    return published_;
  }
  Json stages() const override {
    Json result = Json::array();
    if (!preview_only_)
      result.push_back({{"implementationId", "WASAPI RF64 audio-only"},
                        {"hardware", false},
                        {"software", true}});
    for (const auto &video : videos_) {
      result.push_back(
          {{"sourceId", video.id},
           {"implementationId", video.camera ? "DirectShow native RAM BGRA"
                                : video.output.gpu ? "WGC"
                                                   : "GDI DIB"},
           {"hardware", !video.camera && video.output.gpu},
           {"software", video.camera.has_value() || !video.output.gpu}});
      result.push_back(
          {{"sourceId", video.id},
           {"implementationId",
            video.output.gpu ? "D3D11 NV12 processing" : "CPU NV12 processing"},
           {"hardware", video.output.gpu},
           {"software", !video.output.gpu}});
    }
    return result;
  }
  Json assets() const override {
    std::lock_guard lock(gate_);
    return complete_ ? audio_final_ : Json::array();
  }
  std::shared_ptr<preview::Hub> preview(uint64_t source) override {
    for (auto &video : videos_)
      if (video.id == source) {
        video.hub->bind(session_id(), source);
        return video.hub;
      }
    return {};
  }
  bool media_complete() const noexcept override { return complete_; }
  void cancel_media_completion() noexcept override { cancelled_ = true; }
  bool first_content_admitted() const noexcept override { return admitted_; }
};
} // namespace
std::unique_ptr<Backend> make_auxiliary_backend(const Json &request,
                                                Fault &error) {
  try {
    return std::make_unique<Auxiliary>(request);
  } catch (const capture::Error &e) {
    error = {e.code, e.stage, e.native, e.what()};
  } catch (const std::exception &e) {
    error = {NSS_E_ARGUMENT, NSS_STAGE_PLAN, 0, e.what()};
  }
  return {};
}
} // namespace nss
