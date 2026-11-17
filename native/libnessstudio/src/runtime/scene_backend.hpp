// Private source/composition/encoder/output graph; included only by
// recording.cpp.
class SceneBackend final : public Backend {
  struct Track {
    uint64_t source;
    std::unique_ptr<Recording> recording;
  };
  std::shared_ptr<scene::Graph> graph_;
  std::unique_ptr<capture::Pipeline> preview_program_;
  std::vector<Track> tracks_;
  std::string mode_;
  mutable std::mutex gate_;
  Json metrics_ = Json::object(), stages_ = Json::array(),
       assets_ = Json::array();
  std::atomic<bool> complete_{}, admitted_{};
  int64_t last_publish_{};
  bool preview_only_{}, gpu_{};
  bool automatic_scene_{}, software_budget_{};
  Json capture_selection_=Json::array();
  uint64_t preview_cursor_{};
  void publish() {
    Json value =
        tracks_.empty() ? Json::object() : tracks_.front().recording->metrics();
    Json stages = Json::array(), assets = Json::array();
    for (const auto &track : tracks_) {
      auto item = track.recording->metrics();
      if (track.source == 2 && tracks_.front().source != 2)
        value["cameraTrack"] = item;
      if (track.source == 3)
        value["programTrack"] = item;
      for (auto stage : track.recording->stages()) {
        stage["sourceId"] = track.source;
        stage["sourceKind"] = track.source == 1   ? "screen"
                              : track.source == 2 ? "camera"
                                                  : "program";
        stages.push_back(std::move(stage));
      }
      for (auto asset : track.recording->assets()) {
        asset["role"] = track.source == 1   ? "screen"
                        : track.source == 2 ? "camera"
                                            : "program";
        assets.push_back(std::move(asset));
      }
    }
    value["recordingMode"] = mode_;
    value["encoderCount"] = tracks_.size();
    value["sharedScene"] = graph_->metrics();
    value["sceneCaptureSelection"] = capture_selection_;
    value["independentVideoTracks"] =
        std::count_if(tracks_.begin(), tracks_.end(),
                      [](const Track &track) { return track.source != 3; });
    stages.push_back({{"sourceId", 3},
                      {"sourceKind", "composition"},
                      {"implementationId", "Native immutable shared scene"},
                      {"sharedCaptures", true},
                      {"hardware", gpu_},
                      {"software", !gpu_}});
    std::lock_guard lock(gate_);
    metrics_ = std::move(value);
    stages_ = std::move(stages);
    assets_ = std::move(assets);
    last_publish_ = qpc_hns();
  }
  template <class Action> Fault guard(Action action) {
    try {
      action();
      return {};
    } catch (const RecordingFailure &error) {
      return error.error;
    } catch (const capture::Error &error) {
      return {error.code, error.stage, error.native, error.what()};
    } catch (const std::exception &error) {
      return {NSS_E_INTERNAL, NSS_STAGE_PROCESS, 0, error.what()};
    }
  }
  template <class Action> Fault each(Action action) {
    for (auto &track : tracks_) {
      const auto error = action(*track.recording);
      if (error) {
        publish();
        return error;
      }
    }
    publish();
    return {};
  }

public:
  explicit SceneBackend(const Json &request) {
    keys(request, {"version",
                   "capture",
                   "camera",
                   "mode",
                   "output",
                   "encoder",
                   "outputPath",
                   "cameraOutput",
                   "cameraOutputPath",
                   "cameraEncoder",
                   "durationMs",
                   "interactive",
                   "pauseScript",
                   "audio",
                   "scene",
                   "recordingMode",
                   "programOutputPath",
                   "programEncoder",
                   "encoderBudget",
                   "previewOnly"});
    mode_ = request.value("recordingMode", std::string{"SeparateSources"});
    if (mode_ != "SeparateSources" && mode_ != "Program" &&
        mode_ != "ProgramAndSources")
      reject(NSS_E_ARGUMENT, NSS_STAGE_PLAN, "Unknown recording mode");
    preview_only_ = request.value("previewOnly", false);
    auto snapshot = scene::parse(request.at("scene"));
    std::optional<Plan> screen, camera;
    Json base = request;
    for (const auto *key :
         {"scene", "recordingMode", "programOutputPath", "programEncoder",
          "encoderBudget", "previewOnly", "cameraOutput", "cameraOutputPath",
          "cameraEncoder"})
      base.erase(key);
    if (request.contains("capture")) {
      auto control = base;
      control.erase("camera");
      screen = plan(control);
    }
    if (request.contains("camera")) {
      auto control = base;
      control.erase("capture");
      if (screen) {
        control["mode"] = request.at("capture").at("mode");
        control["output"] = request.value("cameraOutput", Json::object());
        control["encoder"] =
            request.value("cameraEncoder", request.at("encoder"));
        control["outputPath"] = request.at("cameraOutputPath");
      }
      camera = plan(control);
    }
    if (!screen && !camera)
      reject(NSS_E_ARGUMENT, NSS_STAGE_PLAN,
             "A scene requires a physical screen or camera");
    // Checked access prevents MSVC/ASan from speculating a read of the empty
    // optional when selecting a subobject (VS 2026 19.51).
    const bool gpu = (screen ? screen.value() : camera.value()).capture.gpu;
    gpu_ = gpu;
    automatic_scene_=(!screen || screen->automatic_route) && (!camera || camera->automatic_route);
    if (screen && camera && screen->capture.gpu != camera->capture.gpu)
      reject(NSS_E_ARGUMENT, NSS_STAGE_PLAN,
             "Scene sources must use the same execution route");
    if (!gpu && (snapshot.width > 1280 || snapshot.height > 720))
      reject(NSS_E_UNAVAILABLE, NSS_STAGE_PLAN,
             "Software scene canvas exceeds the qualified 1280x720 budget");
    const bool raw = mode_ != "Program", program = mode_ != "SeparateSources";
    const size_t encoders =
        preview_only_
            ? 0
            : (raw ? size_t(screen.has_value()) + size_t(camera.has_value())
                   : 0) +
                  (program ? 1 : 0);
    if (encoders > number(request, "encoderBudget", 3, 3))
      reject(
          NSS_E_UNAVAILABLE, NSS_STAGE_PLAN,
          "Requested scene exceeds the explicit simultaneous encoder budget");
    const uint32_t software_width=encoders==3?640:1280,software_height=encoders==3?360:720;
    auto fits_software=[&](uint32_t width,uint32_t height) {return width<=software_width && height<=software_height;};
    software_budget_=fits_software(snapshot.width,snapshot.height) &&
      (!screen || fits_software(screen->capture.width,screen->capture.height)) &&
      (!camera || fits_software(camera->capture.width,camera->capture.height));
    if (encoders == 3) {
      const uint32_t max_width = gpu ? 1280 : 640, max_height = gpu ? 720 : 360;
      auto exceeds = [&](uint32_t width, uint32_t height) {
        return width > max_width || height > max_height;
      };
      if (exceeds(snapshot.width, snapshot.height) ||
          (screen && exceeds(screen->capture.width, screen->capture.height)) ||
          (camera && exceeds(camera->capture.width, camera->capture.height)))
        reject(NSS_E_UNAVAILABLE, NSS_STAGE_PLAN,
               gpu ? "Three GPU outputs require the HD 720p preset for every "
                     "video track. Select HD 720p for all tracks before REC."
                   : "Three software outputs require the SD 360p preset for "
                     "every video track. Select SD 360p for all tracks, choose "
                     "Program/SeparateSources, or choose the GPU route.");
    }
    graph_ = std::make_shared<scene::Graph>(
        snapshot, screen ? std::optional(screen->capture) : std::nullopt,
        camera ? std::optional(std::pair(*camera->camera, camera->capture))
               : std::nullopt,
        gpu, raw, program && !preview_only_);
    std::vector<std::string> outputs;
    auto add = [&](Plan output, uint64_t source) {
      if (std::find(outputs.begin(), outputs.end(), output.output) !=
          outputs.end())
        reject(NSS_E_ARGUMENT, NSS_STAGE_PLAN,
               "Scene output paths must be distinct");
      outputs.push_back(output.output);
      output.source_id = source;
      tracks_.push_back(
          {source, std::make_unique<Recording>(
                       std::move(output), [graph = graph_, source] {
                         return scene::tap(graph, source);
                       })});
    };
    if (!preview_only_) {
      if (raw) {
        if (screen) {
          auto output = *screen;
          if (program)
            output.audio = Json::array();
          add(std::move(output), 1);
        }
        if (camera) {
          auto output = *camera;
          if (screen || program)
            output.audio = Json::array();
          add(std::move(output), 2);
        }
      }
      if (program) {
        auto control = base;
        if (screen) {
          control.erase("camera");
          control["capture"]["output"] = {{"width", snapshot.width},
                                          {"height", snapshot.height},
                                          {"oddPolicy", "Reject"}};
          control["capture"]["fpsNum"] = snapshot.fps_num;
          control["capture"]["fpsDen"] = snapshot.fps_den;
        } else
          control["output"] = {{"width", snapshot.width},
                               {"height", snapshot.height},
                               {"fpsNum", snapshot.fps_num},
                               {"fpsDen", snapshot.fps_den}};
        control["outputPath"] = request.at("programOutputPath");
        control["encoder"] =
            request.value("programEncoder", request.at("encoder"));
        auto output = plan(control);
        output.camera.reset();
        add(std::move(output), 3);
      }
    }
    if (!program || preview_only_)
      preview_program_ = scene::tap(graph_, 3);
  }
  bool self_paced() const noexcept override { return true; }
  Fault prepare(std::stop_token stop) override {
    auto error = guard([&] {
      graph_->bind(session_id());
      graph_->prepare(stop);
    });
    if(error && gpu_ && automatic_scene_ && software_budget_ &&
       (error.code==NSS_E_UNAVAILABLE || error.code==NSS_E_TIMEOUT) && !stop.stop_requested()) {
      const auto gpu_error=error;
      capture_selection_.push_back({{"route","GPU"},{"accepted",false},
        {"code",gpu_error.code},{"nativeCode",gpu_error.hresult},{"reason",gpu_error.message}});
      error=guard([&] {
        graph_->prepare_software(stop);
        gpu_=false;
        for(auto &track:tracks_) track.recording->select_software_before_prepare(gpu_error);
      });
    }
    if (error)
      return error;
    capture_selection_.push_back({{"route",gpu_?"GPU":"FullSoftware"},{"accepted",true}});
    return each([&](Recording &track) {
      track.bind_session(session_id());
      return track.prepare(stop);
    });
  }
  Fault start(std::stop_token stop, int64_t,
              SessionClock &clock) override {
    graph_->start(clock);
    auto error=each([&](Recording &track) {return track.arm_start(clock);});
    if(error) return error;
    // Retain real eligible content for every track before encoding any track.
    // Sequential blocking Start can consume a short clip's entire acquisition
    // window while another raw track has not yet observed its first sample.
    for(bool content=false;!content;) {
      content=true;
      for(auto &track:tracks_) {
        if(!track.recording->start_content_ready()) {
          error=track.recording->prime_start(stop);
          if(error) {publish();return error;}
        }
        content=content && track.recording->start_content_ready();
      }
      if(!content) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    for(bool encoded=false;!encoded;) {
      encoded=true;
      for(auto &track:tracks_) {
        if(!track.recording->first_content_admitted()) {
          error=track.recording->encode_start(stop);
          if(error) {publish();return error;}
        }
        encoded=encoded && track.recording->first_content_admitted();
      }
      if(!encoded) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    error=each([&](Recording &track) {return track.finish_start(stop);});
    if(error) return error;
    if (preview_only_)
      error = guard([&] {
        const auto deadline = qpc_hns() + 3 * hns_per_second;
        while (!preview_program_->poll()) {
          if (stop.stop_requested())
            throw capture::Error(NSS_E_CANCELLED, NSS_STAGE_CONTROL, 0,
                                 "Scene preview Start cancelled");
          if (qpc_hns() > deadline)
            throw capture::Error(
                NSS_E_TIMEOUT, NSS_STAGE_CAPTURE, 0,
                "Scene preview first content deadline exceeded");
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
      });
    admitted_ = !error;
    return error;
  }
  Fault pause(std::stop_token stop) override {
    return each([&](Recording &track) { return track.pause(stop); });
  }
  Fault resume(std::stop_token stop, SessionClock &clock) override {
    graph_->resume();
    return each([&](Recording &track) { return track.resume(stop, clock); });
  }
  Fault tick(SessionClock &clock) override {
    auto error = guard([&] {
      graph_->poll();
      graph_->cached_consumers(true);
      (void)graph_->program(preview_cursor_);
    });
    if (!error)
      for (auto &track : tracks_) {
        error = track.recording->tick(clock);
        if (error)
          break;
      }
    if (!error)
      error = guard([&] { graph_->present_one(); });
    graph_->cached_consumers(false);
    if (error) {
      publish();
      return error;
    }
    if (tracks_.empty())
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (error || qpc_hns() - last_publish_ >= hns_per_second / 5)
      publish();
    return error;
  }
  ShutdownProgress shutdown(ShutdownStage stage) override {
    ShutdownProgress result;
    for (auto &track : tracks_) {
      auto current = track.recording->shutdown(stage);
      result.pending |= current.pending;
      if (!result.error)
        result.error = current.error;
    }
    if (stage == ShutdownStage::Sources) {
      const auto error = guard([&] { graph_->close(); });
      if (!result.error)
        result.error = error;
    }
    if (stage == ShutdownStage::Resources)
      complete_ =
          !tracks_.empty() &&
          std::all_of(tracks_.begin(), tracks_.end(), [](const Track &track) {
            return track.recording->media_complete();
          });
    publish();
    return result;
  }
  Json metrics() const override {
    std::lock_guard lock(gate_);
    return metrics_;
  }
  Json stages() const override {
    std::lock_guard lock(gate_);
    return stages_;
  }
  Json assets() const override {
    std::lock_guard lock(gate_);
    return assets_;
  }
  std::shared_ptr<preview::Hub> preview(uint64_t source) override {
    return graph_->preview(source);
  }
  uint64_t update_scene(const Json &snapshot) override {
    return graph_->update(snapshot);
  }
  bool media_complete() const noexcept override { return complete_; }
  bool first_content_admitted() const noexcept override { return admitted_; }
  void cancel_media_completion() noexcept override {
    for (auto &track : tracks_)
      track.recording->cancel_media_completion();
  }
};
