#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "recording.hpp"
#include "encoder_selection.hpp"
#include "audio/audio.hpp"
#include "camera/camera.hpp"
#include "capture/capture.hpp"
#include "capture/acquisition_window.hpp"
#include "capture/readback.hpp"
#include "capture/scheduling.hpp"
#include "preview/preview.hpp"
#include "scene/graph.hpp"
#include <algorithm>
#include <array>
#include <deque>
#include <filesystem>
#include <limits>
#include <ness_muxer_v2.h>
#include <utility>
#include <windows.h>

namespace nss {
namespace {
template <class T> T fresh() {
  T v{};
  v.size = sizeof(v);
  v.version = 2;
  return v;
}
struct RecordingFailure {
  Fault error;
};
[[noreturn]] void reject(nss_result code, uint32_t stage, const char *message,
                         int32_t hr = 0) {
  throw RecordingFailure{{code, stage, hr, message}};
}
void checked(int32_t result, const ness_error_v2 &error) {
  if (result >= 0)
    return;
  const auto code =
      result == NESS_V2_E_BACKPRESSURE  ? NSS_E_BACKPRESSURE
      : result == NESS_V2_E_UNAVAILABLE ? NSS_E_UNAVAILABLE
      : result == NESS_V2_E_IO          ? NSS_E_IO
      : result == NESS_V2_E_ARGUMENT || result == NESS_V2_E_VERSION
          ? NSS_E_ARGUMENT
          : NSS_E_INTERNAL;
  reject(code,
         error.stage == NESS_V2_STAGE_CONFIG   ? NSS_STAGE_PLAN
         : error.stage >= NESS_V2_STAGE_OUTPUT ? NSS_STAGE_OUTPUT
                                               : NSS_STAGE_ENCODER,
         reinterpret_cast<const char *>(error.message_utf8), error.native_code);
}
void keys(const Json &value, std::initializer_list<const char *> allowed) {
  if (!value.is_object())
    reject(NSS_E_ARGUMENT, NSS_STAGE_PLAN,
           "Recording controls must be objects");
  for (const auto &item : value.items()) {
    bool found = false;
    for (const auto *key : allowed)
      if (item.key() == key)
        found = true;
    if (!found)
      reject(NSS_E_ARGUMENT, NSS_STAGE_PLAN, "Unknown recording control field");
  }
}
uint64_t number(const Json &value, const char *key, uint64_t fallback,
                uint64_t maximum) {
  if (!value.contains(key))
    return fallback;
  const auto &n = value.at(key);
  if (!(n.is_number_unsigned() ||
        (n.is_number_integer() && n.get<int64_t>() >= 0)))
    reject(NSS_E_ARGUMENT, NSS_STAGE_PLAN,
           "Expected a nonnegative integer control");
  const auto result = n.get<uint64_t>();
  if (result > maximum)
    reject(NSS_E_ARGUMENT, NSS_STAGE_PLAN,
           "Recording control exceeds its budget");
  return result;
}
struct Plan {
  capture::Plan capture;
  std::optional<camera::Plan> camera;
  Json audio = Json::array();
  ness_encoder_config_v2 encoder = fresh<ness_encoder_config_v2>();
  std::string output;
  uint64_t estimated_bytes{}, available_bytes{};
  int64_t duration{};
  bool interactive{};
  uint64_t source_id{};
  std::string requested_mode,requested_backend;
  bool automatic_route{},automatic_encoder{},allow_hybrid{};
};
const char *encoder_name(uint32_t backend) {
  switch(backend) {
  case NESS_V2_BACKEND_QSV_D3D11:return "QsvD3D11";
  case NESS_V2_BACKEND_NVENC_D3D11:return "NvencD3D11";
  case NESS_V2_BACKEND_AMF_D3D11:return "AmfD3D11";
  case NESS_V2_BACKEND_X264:return "X264";
  case NESS_V2_BACKEND_MF_D3D11:return "MfD3D11";
  case NESS_V2_BACKEND_MF_SOFTWARE:return "MfSoftware";
  default:return "Auto";
  }
}
Plan plan(const Json &request) {
  keys(request,
       {"version", "capture", "camera", "mode", "output", "encoder",
        "outputPath", "durationMs", "interactive", "pauseScript", "audio"});
  if (number(request, "version", 0, 1) != 1)
    reject(NSS_E_VERSION, NSS_STAGE_PLAN, "Unsupported recording version");
  Plan result;
  result.requested_mode=request.contains("camera")?request.at("mode").get<std::string>():
    request.at("capture").value("mode",std::string("RequireGpuResidentHardware"));
  result.automatic_route=result.requested_mode=="Auto";
  result.allow_hybrid=result.automatic_route || result.requested_mode=="GpuWithSoftwareEncoding";
  if (request.contains("camera")) {
    result.camera = camera::plan(request.at("camera"));
    const auto mode = request.at("mode").get<std::string>();
    if (mode != "FullSoftware" && mode != "RequireGpuResidentHardware" && mode!="Auto" && mode!="GpuWithSoftwareEncoding")
      reject(NSS_E_ARGUMENT, NSS_STAGE_PLAN,
             "Camera requires an explicit execution mode");
    auto &output = result.capture;
    output.gpu = mode != "FullSoftware";
    output.crop = {0, 0, static_cast<int32_t>(result.camera->width),
                   static_cast<int32_t>(result.camera->height)};
    const auto options = request.value("output", Json::object());
    keys(options, {"width", "height", "fpsNum", "fpsDen", "slots"});
    output.width = static_cast<uint32_t>(number(
        options, "width", result.camera->width, output.gpu ? 1920 : 1280));
    output.height = static_cast<uint32_t>(number(
        options, "height", result.camera->height, output.gpu ? 1080 : 720));
    if (!output.width || !output.height || output.width & 1 ||
        output.height & 1)
      reject(NSS_E_ARGUMENT, NSS_STAGE_PLAN,
             "Camera output must have positive even dimensions");
    output.requested_width = output.width;
    output.requested_height = output.height;
    output.fps_num =
        static_cast<uint32_t>(number(options, "fpsNum", 30, 30000));
    output.fps_den = static_cast<uint32_t>(number(options, "fpsDen", 1, 1001));
    output.slots =
        static_cast<uint32_t>(number(options, "slots", output.gpu ? 6 : 3, 8));
    if (!output.slots)
      reject(NSS_E_ARGUMENT, NSS_STAGE_PLAN,
             "Camera pool must have at least one slot");
    output.viewport =
        capture::fit_viewport(result.camera->width, result.camera->height,
                              output.width, output.height);
    if (output.gpu) {
      const auto list = capture::displays();
      const auto display =
          std::find_if(list.begin(), list.end(),
                       [](const auto &item) { return item.primary; });
      if (display == list.end() && !result.automatic_route)
        reject(NSS_E_UNAVAILABLE, NSS_STAGE_CAPTURE,
               "Camera hardware encoder requires an available adapter");
      if(display!=list.end()) output.display = *display;
      else {
        output.gpu=false;
        if(output.width>1280 || output.height>720)
          reject(NSS_E_UNAVAILABLE,NSS_STAGE_PLAN,
            "Auto camera cannot preserve this geometry without a GPU adapter");
      }
    }
  } else {
    auto capture_request=request.at("capture");
    if(result.allow_hybrid) capture_request["mode"]="RequireGpuResidentHardware";
    result.capture = capture::plan(capture_request);
    if (result.automatic_route && result.capture.desktop_duplication)
      reject(NSS_E_ARGUMENT, NSS_STAGE_PLAN,
             "Desktop Duplication qualification requires an explicit execution mode");
  }
  const auto &enc = request.at("encoder");
  keys(enc, {"backend", "bitrateBps", "profile", "maxPendingFrames",
             "maxPacketBytes"});
  const auto backend = enc.at("backend").get<std::string>();
  result.requested_backend=backend;
  const bool hybrid=result.requested_mode=="GpuWithSoftwareEncoding";
  result.automatic_encoder=backend=="Auto" && result.capture.gpu && !hybrid;
  if(result.automatic_route && backend!="Auto")
    reject(NSS_E_ARGUMENT,NSS_STAGE_PLAN,"Auto execution requires Auto encoder selection");
  if (hybrid ? (backend!="X264" && backend!="Auto") :
      result.capture.gpu ? (backend != "MfD3D11" && backend != "QsvD3D11" &&
          backend!="NvencD3D11" && backend!="AmfD3D11" && backend!="Auto")
                         : (backend != "MfSoftware" && backend != "X264" && backend!="Auto"))
    reject(NSS_E_UNAVAILABLE, NSS_STAGE_PLAN,
           "Capture and explicit encoder policies must agree; fallback is "
           "forbidden");
  auto &c = result.encoder;
  c.backend = result.automatic_encoder ? 0
      : backend == "QsvD3D11" ? NESS_V2_BACKEND_QSV_D3D11
      : backend == "NvencD3D11" ? NESS_V2_BACKEND_NVENC_D3D11
      : backend == "AmfD3D11" ? NESS_V2_BACKEND_AMF_D3D11
      : backend == "Auto" ? NESS_V2_BACKEND_X264
      : backend == "X264" ? NESS_V2_BACKEND_X264
      : result.capture.gpu ? NESS_V2_BACKEND_MF_D3D11 : NESS_V2_BACKEND_MF_SOFTWARE;
  c.policy =
      result.capture.gpu && !hybrid ? NESS_V2_POLICY_REQUIRE_GPU : NESS_V2_POLICY_SOFTWARE;
  c.width = result.capture.width;
  c.height = result.capture.height;
  c.fps_num = result.capture.fps_num;
  c.fps_den = result.capture.fps_den;
  if (!recording_cadence_supported(c.fps_num, c.fps_den))
    reject(NSS_E_UNAVAILABLE, NSS_STAGE_PLAN,
           "Recording supports 24, 25, 30, 48, 50, 60 or 30000/1001 fps");
  c.bitrate_bps = static_cast<uint32_t>(number(
      enc, "bitrateBps", result.capture.gpu ? 12000000 : 6000000, 100000000));
  if (c.bitrate_bps < 10000)
    reject(NSS_E_ARGUMENT, NSS_STAGE_PLAN,
           "Encoder bitrate is below its minimum");
  c.profile = static_cast<uint32_t>(number(enc, "profile", 66, 100));
  if (c.profile != 66)
    reject(NSS_E_UNAVAILABLE, NSS_STAGE_PLAN,
           "Only explicit H264 baseline with zero B frames is qualified");
  c.color = NESS_V2_BT709_LIMITED;
  c.rate_control = NESS_V2_CBR;
  c.max_pending_frames =
      static_cast<uint32_t>(number(enc, "maxPendingFrames", 16, 64));
  c.max_packet_bytes =
      number(enc, "maxPacketBytes", 16 * 1024 * 1024, 16 * 1024 * 1024);
  if (!c.max_pending_frames || !c.max_packet_bytes)
    reject(NSS_E_ARGUMENT, NSS_STAGE_PLAN, "Encoder budgets must be positive");
  if (request.contains("interactive") &&
      !request.at("interactive").is_boolean())
    reject(NSS_E_ARGUMENT, NSS_STAGE_PLAN, "interactive must be a boolean");
  result.interactive = request.value("interactive", false);
  if (result.interactive &&
      (request.contains("durationMs") || request.contains("pauseScript")))
    reject(NSS_E_ARGUMENT, NSS_STAGE_PLAN,
           "Interactive recording ends through Stop; duration and pause "
           "scripts are forbidden");
  const auto duration =
      number(request, "durationMs", 5000, 24ULL * 3600 * 1000);
  if (duration < 100)
    reject(NSS_E_ARGUMENT, NSS_STAGE_PLAN,
           "Recording duration must be at least 100 ms");
  result.duration = result.interactive
                        ? (std::numeric_limits<int64_t>::max)() / 2
                        : static_cast<int64_t>(duration) * 10000;
  if (request.contains("pauseScript")) {
    const auto &pauses = request.at("pauseScript");
    if (!pauses.is_array() || pauses.size() > 128)
      reject(NSS_E_ARGUMENT, NSS_STAGE_PLAN, "Pause script exceeds its budget");
    uint64_t end{};
    for (const auto &pause : pauses) {
      keys(pause, {"atMs", "durationMs"});
      const auto at = number(pause, "atMs", 0, duration);
      const auto length = number(pause, "durationMs", 0, 3600000);
      if (!length || at < end || at >= duration)
        reject(NSS_E_ARGUMENT, NSS_STAGE_PLAN,
               "Pause script must be ordered and inside the useful duration");
      end = at + 1;
    }
  }
  result.output = request.at("outputPath").get<std::string>();
  if (result.output.empty() || result.output.size() > 32760 ||
      result.output.find('\0') != std::string::npos)
    reject(NSS_E_ARGUMENT, NSS_STAGE_PLAN, "Invalid recording output path");
  const std::filesystem::path path(
      std::u8string(reinterpret_cast<const char8_t *>(result.output.data()),
                    result.output.size()));
  if (!path.is_absolute() || path.extension() != L".mkv" ||
      !std::filesystem::is_directory(path.parent_path()) ||
      std::filesystem::exists(path))
    reject(NSS_E_ARGUMENT, NSS_STAGE_PLAN,
           "Output requires a new absolute MKV path in an existing directory");
  for (const auto *suffix :
       {L".manifest.json", L".manifest.pending", L".partial.manifest.json",
        L".partial.manifest.pending"})
    if (std::filesystem::exists(std::filesystem::path(path.native() + suffix)))
      reject(NSS_E_ARGUMENT, NSS_STAGE_PLAN,
             "Recording manifest paths must also be new");
  if (request.contains("audio"))
    result.audio = audio::validate(request.at("audio"), result.output);
  // Interactive recordings use the host's monitored 50 MiB stop boundary.
  // Finite jobs still require their estimated output plus a 1 GiB reserve.
  const auto estimate_ms = result.interactive ? 60000ULL : duration;
  result.estimated_bytes = estimate_ms * c.bitrate_bps / 8000 +
                           estimate_ms * 192 * result.audio.size();
  result.available_bytes = std::filesystem::space(path.parent_path()).available;
  const auto required_bytes = result.interactive ? 50ULL * 1024 * 1024
      : result.estimated_bytes + 1024ULL * 1024 * 1024;
  if (result.available_bytes < required_bytes)
    reject(NSS_E_IO, NSS_STAGE_STORAGE,
           result.interactive ? "Not enough free disk space to start recording; at least 50 MiB are required"
                              : "Recording preflight requires estimated encoded bytes plus a 1 GiB reserve");
  return result;
}
class Recording final : public Backend {
  Plan plan_;
  std::shared_ptr<preview::Hub> preview_ = std::make_shared<preview::Hub>();
  std::unique_ptr<audio::Session> audio_;
  Json audio_final_ = Json::array();
  Fault recording_fault_;
  std::optional<capture::detail::CaptureTask> scheduling_;
  std::unique_ptr<capture::Pipeline> source_;
  std::unique_ptr<capture::Readback> readback_;
  Json readback_final_=Json::object(),selection_=Json::array();
  std::function<std::unique_ptr<capture::Pipeline>()> source_factory_;
  ness_encoder_v2 *encoder_{};
  ness_mkv_v2 *muxer_{};
  struct Inflight {
    std::shared_ptr<const capture::Surface> surface;
    ness_input_lease_v2 *lease{};
    Inflight(std::shared_ptr<const capture::Surface> s, ness_input_lease_v2 *l)
        : surface(std::move(s)), lease(l) {}
    Inflight(const Inflight &) = delete;
    Inflight &operator=(const Inflight &) = delete;
    Inflight(Inflight &&other) noexcept
        : surface(std::move(other.surface)),
          lease(std::exchange(other.lease, nullptr)) {}
    Inflight &operator=(Inflight &&other) noexcept {
      if (this != &other) {
        ness_input_release_v2(lease);
        surface = std::move(other.surface);
        lease = std::exchange(other.lease, nullptr);
      }
      return *this;
    }
    ~Inflight() { ness_input_release_v2(lease); }
  };
  std::deque<Inflight> inflight_;
  std::deque<capture::Frame> incoming_;
  std::optional<capture::Frame> last_;
  std::optional<capture::Frame> pending_;
  std::optional<capture::Frame> preview_frame_;
  std::optional<capture::Frame> partial_cut_tail_;
  std::optional<capture::Frame> startup_candidate_;
  int64_t startup_at_{};
  SessionClock *clock_{};
  HANDLE timer_{};
  std::atomic<bool> admitted_{}, complete_{}, completion_cancelled_{};
  bool paused_{}, failed_{}, codec_set_{}, eos_{}, drained_{}, source_closed_{};
  uint64_t frames_{}, packets_{}, received_{}, rejected_{}, decimated_{},
      repeats_{}, previous_id_{}, pressure_{}, inflight_peak_{}, queue_peak_{},
      expected_slots_{}, timer_wakeups_{}, source_wakeups_{}, gpu_wakeups_{}, readback_wakeups_{}, coalesced_receive_polls_{},
      coalesced_active_receive_polls_{},
      partial_cut_retained_{}, partial_cut_resumed_{};
  uint64_t startup_prefetched_{}, repeated_candidate_upgrades_{}, cpu_candidates_prepared_{},
      gpu_candidates_prepared_{};
  uint64_t interval_candidate_selections_{};
  capture::WindowSampling window_sampling_;
  uint64_t window_sampling_skips_{}, window_sampling_acquisitions_{};
  uint64_t deadline_waits_{}, extended_deadline_waits_{};
  int64_t interval_candidate_maximum_lead_{};
  int64_t first_latency_{}, prepare_at_{}, last_publish_{}, last_pts_{},
      last_duration_{}, last_receive_poll_{};
  struct CallCost {
    uint64_t calls{}, total_hns{}, maximum_hns{};
    struct SlowCall { int64_t begin{}, end{}, allowance{}; };
    std::array<SlowCall, 32> slow{};
    size_t retained_slow{};
    uint64_t suppressed_slow{};
    int64_t maximum_begin{};
    void observe(int64_t started, bool diagnostic = false, int64_t allowance = 0) {
      const auto finished = qpc_hns();
      const auto duration = static_cast<uint64_t>((std::max)(int64_t{}, finished - started));
      ++calls;
      total_hns += duration;
      if (duration > maximum_hns) { maximum_hns = duration; maximum_begin = started; }
      // Fixed storage, enabled only by the existing capture trace option.
      // A normal timer sleep is not a slow call: retain only an additional
      // ten milliseconds beyond the requested wait, or a ten-ms operation.
      if (diagnostic && duration > static_cast<uint64_t>(allowance + 100000)) {
        if (retained_slow < slow.size()) slow[retained_slow++] = {started, finished, allowance};
        else {
          ++suppressed_slow;
          const auto smallest = std::min_element(slow.begin(), slow.end(), [](const auto &a, const auto &b) {
            return a.end - a.begin - a.allowance < b.end - b.begin - b.allowance;
          });
          if (finished - started - allowance > smallest->end - smallest->begin - smallest->allowance)
            *smallest = {started, finished, allowance};
        }
      }
    }
    Json report() const {
      Json result{{"calls", calls}, {"totalMs", double(total_hns) / 10000},
              {"meanMs", calls ? double(total_hns) / 10000 / double(calls) : 0},
              {"maximumMs", double(maximum_hns) / 10000}, {"maximumBeginQpcHns", maximum_begin}};
      if (retained_slow || suppressed_slow) {
        Json samples = Json::array();
        for (size_t i = 0; i < retained_slow; ++i)
          samples.push_back({{"beginQpcHns", slow[i].begin}, {"endQpcHns", slow[i].end},
                            {"allowedWaitHns", slow[i].allowance}});
        result["slowCalls"] = std::move(samples);
        result["suppressedSlowCalls"] = suppressed_slow;
        result["slowCallRetention"] = "largest 32 excess wall times beyond allowed wait";
      }
      return result;
    }
  } capture_calls_, receive_calls_, submit_calls_, preparation_calls_, publication_calls_,
      flush_calls_, wait_calls_, retire_calls_, muxer_calls_, audio_check_calls_,
      metric_collection_calls_, tick_work_calls_;
  Json samples_ = Json::array(), forced_ = Json::array(),
       cadence_ = Json::array(), pipeline_ = Json::object(), source_preparation_ = Json::object(),
       encoder_info_ = Json::object(), encoder_stats_ = Json::object(),
       muxer_stats_ = Json::object();
  mutable std::mutex metrics_mutex_;
  std::shared_ptr<const Json> published_ = std::make_shared<const Json>(Json::object());
  std::shared_ptr<const Json> published_snapshot() const {
    std::lock_guard lock(metrics_mutex_);
    return published_;
  }
  std::string failure_message_;
  const int64_t completion_grace;
  uint32_t source_idle_poll_cap_ms_{10};
  bool uses_window_sampling() const {
    return plan_.capture.desktop_duplication && plan_.capture.defer_conversion &&
        !source_factory_ && clock_ && admitted_ && !paused_ && !preview_->active();
  }
  void wait() {
    const auto flush_started = plan_.capture.trace ? qpc_hns() : 0;
    if (source_ && !source_closed_)
      source_->flush_work();
    if (plan_.capture.trace) flush_calls_.observe(flush_started, true);
    const auto hint = source_ && !source_closed_
                          ? source_->wait_hint()
                          : capture::Pipeline::WaitHint{};
    LARGE_INTEGER due{};
    // Source arrival and registered GPU completion wake the owner directly.
    // Retain the 1 ms polling fallback and the 10 ms cap for CFR/control/health.
    // Neither wait changes source timestamps, output slots or completion grace.
    const auto readback_event=readback_?readback_->completion_event():nullptr;
    const bool readback_poll=readback_ && readback_->pending() && !readback_event;
    const auto idle_ms = hint.event ? 10u : (std::clamp)(hint.idle_poll_ms, 1u, 10u);
    if (source_ && !source_closed_) source_idle_poll_cap_ms_ = idle_ms;
    const bool completion_poll = readback_poll || (hint.processing && !hint.completion_event);
    auto delay = completion_poll ? int64_t{10000} : 10000 * static_cast<int64_t>(idle_ms);
    // When acquisition is known to be idle, waking every source-poll interval
    // cannot produce another candidate. Sleep to the next required sample or
    // boundary instead. Encoder progress, GPU polling and closed cuts retain
    // their original waits; completion events remain in the wait set below.
    if (!completion_poll && source_ && !source_closed_ && uses_window_sampling() &&
        !eos_ && !pending_ && frames_ == packets_ && inflight_.empty()) {
      const auto active = clock_->map(qpc_hns());
      if (active && *active < plan_.duration) {
        const auto window = capture::acquisition_window(*active, plan_.encoder.fps_num, plan_.encoder.fps_den);
        if (const auto idle_until = window_sampling_.idle_until(window, *active)) {
          const auto output_due = frame_time(frames_ + 1, plan_.encoder.fps_num, plan_.encoder.fps_den) + completion_grace;
          const auto deadline = (std::min)({*idle_until, output_due, plan_.duration});
          if (deadline > *active) {
            const auto scheduled = (std::min)(deadline - *active, int64_t{100000});
            ++deadline_waits_;
            if (scheduled > delay) ++extended_deadline_waits_;
            delay = scheduled;
          }
        }
      }
    }
    due.QuadPart = -delay;
    const auto sleep_started = plan_.capture.trace ? qpc_hns() : 0;
    if (!SetWaitableTimerEx(timer_, &due, 0, nullptr, nullptr, nullptr, 0))
      reject(NSS_E_TIMEOUT, NSS_STAGE_CAPTURE,
             "Native recording pacing failed");
    HANDLE objects[4]{timer_};
    DWORD count = 1, source_index = MAXDWORD, gpu_index = MAXDWORD,readback_index=MAXDWORD;
    if (hint.event) { source_index = count; objects[count++] = hint.event; }
    if (hint.completion_event) { gpu_index = count; objects[count++] = hint.completion_event; }
    if(readback_event) {readback_index=count;objects[count++]=readback_event;}
    const auto woke = WaitForMultipleObjects(count, objects, FALSE, 1000);
    if (plan_.capture.trace) wait_calls_.observe(sleep_started, true, delay);
    if (woke == WAIT_OBJECT_0)
      ++timer_wakeups_;
    else if (hint.event && woke == WAIT_OBJECT_0 + source_index)
      ++source_wakeups_;
    else if (hint.completion_event && woke == WAIT_OBJECT_0 + gpu_index)
      ++gpu_wakeups_;
    else if(readback_event && woke==WAIT_OBJECT_0+readback_index) {
      ++gpu_wakeups_;
      ++readback_wakeups_;
    }
    else
      reject(NSS_E_TIMEOUT, NSS_STAGE_CAPTURE,
             "Native recording pacing failed");
    // The session may persist a recovery checkpoint immediately after tick.
    // Return completed WGC buffers now instead of holding them through that I/O.
    if (source_ && !source_closed_ && plan_.capture.gpu && plan_.capture.defer_conversion) {
      const auto retire_started = plan_.capture.trace ? qpc_hns() : 0;
      source_->retire_completed_work();
      if (plan_.capture.trace) retire_calls_.observe(retire_started, true);
    }
  }
  template <class F> Fault guard(F operation) {
    try {
      operation();
      return {};
    } catch (const RecordingFailure &e) {
      if (!recording_fault_)
        recording_fault_ = e.error;
      failed_ = true;
      failure_message_ = e.error.message;
      publish();
      return e.error;
    } catch (const capture::Error &e) {
      if (!recording_fault_)
        recording_fault_ = {e.code, e.stage, e.native, e.what()};
      failed_ = true;
      failure_message_ = e.what();
      publish();
      return {e.code, e.stage, e.native, e.what()};
    } catch (const std::exception &e) {
      if (!recording_fault_)
        recording_fault_ = {NSS_E_INTERNAL, NSS_STAGE_CONTROL, 0, e.what()};
      failed_ = true;
      failure_message_ = e.what();
      publish();
      return {NSS_E_INTERNAL, NSS_STAGE_CONTROL, 0, e.what()};
    }
  }
  void publish() {
    const auto publication_started = qpc_hns();
    Json intervals = Json::array();
    if (clock_)
      for (const auto &cut : clock_->history())
        intervals.push_back({{"beginHns", cut.begin},
                             {"endHns", cut.end},
                             {"activeBeginHns", cut.active_begin},
                             {"generation", cut.generation}});
    auto gpu = fresh<ness_gpu_runtime_stats_v2>();
    (void)ness_gpu_runtime_stats_get_v2(&gpu, nullptr);
    auto runtime = fresh<ness_runtime_stats_v2>();
    (void)ness_runtime_stats_get_v2(&runtime, nullptr);
    const auto readback=readback_?readback_->metrics():readback_final_;
    Json result = {
        {"capture", pipeline_},
        {"sourcePreparation", source_preparation_},
        {"preview", preview_->metrics()},
        {"audio", audio_ ? audio_->snapshot() : audio_final_},
        {"liveAudioSources", audio::live_sources.load()},
        {"effective", capture::plan_json(plan_.capture)},
        {"camera",
         plan_.camera ? camera::plan_json(*plan_.camera) : Json(nullptr)},
        {"encoder", encoder_info_},
        {"encoderSelection",{{"requestedMode",plan_.requested_mode},{"requestedBackend",plan_.requested_backend},
          {"effectiveMode",plan_.capture.gpu?(plan_.encoder.policy==NESS_V2_POLICY_REQUIRE_GPU?"RequireGpuResidentHardware":"GpuWithSoftwareEncoding"):"FullSoftware"},
          {"attempts",selection_},{"midRecordingSwitch",false}}},
        {"softwareEncoderReadback",readback},
        {"encoderStats", encoder_stats_},
        {"muxer", muxer_stats_},
        {"framesSubmitted", frames_},
        {"packetsWritten", packets_},
        {"framesReceived", received_},
        {"rejectedAcquisitionIntervals", rejected_},
        {"schedulerInputDecimation", decimated_},
        {"repeatsWithoutIndependentStaticConfirmation", repeats_},
        {"partialCutCandidatesRetained", partial_cut_retained_},
        {"partialCutCandidatesResumed", partial_cut_resumed_},
        {"startupPrefetchedAcquisitions", startup_prefetched_},
        {"repeatedCandidatesReplacedBeforeSubmit", repeated_candidate_upgrades_},
        {"intervalCandidateSelections", interval_candidate_selections_},
        {"intervalCandidateMaximumLeadMs", double(interval_candidate_maximum_lead_) / 10000},
        {"cameraCfrAdaptationRepeats", plan_.camera ? repeats_ : 0},
        {"cameraSourceCadenceMeasuredSeparately", plan_.camera.has_value()},
        {"expectedSlotsAtStop", expected_slots_},
        {"missingSlots",
         expected_slots_ > frames_ ? expected_slots_ - frames_ : 0},
        {"encoderBackpressureObservations", pressure_},
        {"inflightLeases", inflight_.size()},
        {"inflightPeak", inflight_peak_},
        {"inflightLimit", plan_.encoder.max_pending_frames},
        {"schedulerQueuePeak", queue_peak_},
        {"schedulerQueueLimit", plan_.capture.slots - 1},
        {"firstFrameLatencyMs", double(first_latency_) / 10000},
        {"lastPtsHns", last_pts_},
        {"lastDurationHns", last_duration_},
        {"usefulDurationHns", frames_ ? last_pts_ + last_duration_ : 0},
        {"targetDurationHns",
         plan_.interactive ? Json(nullptr) : Json(plan_.duration)},
        {"interactive", plan_.interactive},
        {"pixelReadbacks", readback.value("copies",uint64_t{})},
        {"recordingReadbackBytes", readback.value("bytes",uint64_t{})},
        {"cpuEncoderInputCopies", plan_.encoder.policy==NESS_V2_POLICY_REQUIRE_GPU ? 0 : frames_},
        {"cpuCandidatesPreparedBeforeSubmit", cpu_candidates_prepared_},
        {"gpuCandidatesPreparedBeforeSubmit", gpu_candidates_prepared_},
        {"timelineSamples", samples_},
        {"forcedRepeatSamples", forced_},
        {"forcedRepeatSampleLimit", plan_.camera ? 64 : 4096},
        {"activeIntervals", intervals},
        {"cadenceWindows", cadence_},
        {"cadenceWindowLimit", 600},
        {"estimatedOutputBytes", plan_.estimated_bytes},
        {"storageAvailableAtPlanBytes", plan_.available_bytes},
        {"failed", failed_},
        {"failureMessage", failure_message_},
        {"livePipelines", capture::live_pipelines.load()},
        {"liveSurfaces", capture::live_surfaces.load()},
        {"liveCaptureBytes", capture::live_capture_bytes.load()},
        {"liveFramePoolBytes", live_pool_bytes.load()},
        {"liveFrameLeases", live_frame_leases.load()},
        {"liveGpuCompletionStates", gpu.completion_states},
        {"liveGpuInputLeases", gpu.input_leases},
        {"liveEncoders", runtime.encoders},
        {"liveMuxers", runtime.muxers},
        {"livePackets", runtime.packets},
        {"liveCodecDescriptions", runtime.codecs},
        {"livePacketBytes", runtime.packet_bytes},
        {"captureCompletionGraceHns", completion_grace},
        {"acquisitionWindowSampling", {{"enabledForExplicitDuplication", plan_.capture.desktop_duplication},
          {"skippedPolls", window_sampling_skips_}, {"eligibleWindowAcquisitions", window_sampling_acquisitions_},
          {"sampleFractionNumerator", capture::acquisition_sample_numerator},
          {"sampleFractionDenominator", capture::acquisition_sample_denominator},
          {"sourceTimestampsChanged", false}, {"previewAndClosedCutsBypass", true}}},
        {"pacing", {{"timerWakeups", timer_wakeups_},
                    {"acquisitionDeadlineWaits", deadline_waits_},
                    {"extendedAcquisitionDeadlineWaits", extended_deadline_waits_},
                    {"acquisitionDeadlineWaitCapMs", 10},
                    {"sourceWakeups", source_wakeups_},
                    {"gpuCompletionWakeups", gpu_wakeups_},
                    {"captureGpuCompletionWakeups", gpu_wakeups_-readback_wakeups_},
                    {"readbackCompletionWakeups", readback_wakeups_},
                    {"gpuCompletionPollMs", 1}, {"sourceIdlePollCapMs", source_idle_poll_cap_ms_},
                    {"gpuCompletionWaitPolicy", "registered event when available; 1 ms polling fallback; 10 ms watchdog"}}},
        {"metricsPublishedQpcHns", publication_started},
        {"regularMetricsPublishIntervalMs", 1000},
        {"workerCallTiming", {{"capturePoll", capture_calls_.report()},
                              {"framePreparation", preparation_calls_.report()},
                              {"completedMetricsPublications", publication_calls_.report()},
                              {"encoderReceive", receive_calls_.report()},
                              {"encoderSubmit", submit_calls_.report()},
                              {"traceEnabled", plan_.capture.trace},
                              {"sourceFlush", flush_calls_.report()},
                              {"ownerWait", wait_calls_.report()},
                              {"completionRetirement", retire_calls_.report()},
                              {"muxerWrite", muxer_calls_.report()},
                              {"audioHealthCheck", audio_check_calls_.report()},
                              {"metricCollection", metric_collection_calls_.report()},
                              {"tickWorkBeforeWait", tick_work_calls_.report()},
                              {"coalescedIdleReceivePolls", coalesced_receive_polls_},
                              {"coalescedActiveReceivePolls", coalesced_active_receive_polls_},
                              {"activeEncoderPollIntervalMs", 2},
                              {"idleEncoderPollIntervalMs", double(hns_per_second / 30) / 10000},
                              {"scope", "Owner-thread call wall time including synchronous waits; not per-frame end-to-end latency"}}},
        {"selectionPolicy",
         "latest timestamp in closed rational window; bounded one-frame "
         "completion "
         "grace; whole acquisition interval inside one active cut; no "
         "timestamp offset"},
        {"completionPolicy",
         "tracked sample return before releasing a texture lease; EOS and "
         "every packet before file finalization"}};
    std::shared_ptr<const Json> next = std::make_shared<const Json>(std::move(result));
    {
      std::lock_guard lock(metrics_mutex_);
      published_.swap(next);
    }
    // Readers copy immutable data after releasing the publication mutex.
    // Destroy the displaced publication outside that mutex as well.
    next.reset();
    last_publish_ = qpc_hns();
    publication_calls_.observe(publication_started, plan_.capture.trace);
  }
  void reap() {
    for (auto it = inflight_.begin(); it != inflight_.end();) {
      auto value = fresh<ness_completion_v2>();
      auto error = fresh<ness_error_v2>();
      checked(ness_input_completion_get_v2(it->lease, &value, &error), error);
      if (value.ready)
        it = inflight_.erase(it);
      else
        ++it;
    }
  }
  void collect() {
    if (!encoder_)
      return;
    const auto now = qpc_hns();
    // This recording contract emits one complete AU per submitted frame.
    // With all AUs consumed, no pending submit and no EOS, another immediate
    // Receive cannot deliver a missing frame. Still reap tracked textures on
    // every visit and poll idle MFT events within one frame for async faults.
    const bool idle = !pending_ && frames_ == packets_;
    const auto interval = idle ? hns_per_second / 30 : hns_per_second / 500;
    // tick/schedule can visit Receive several times without waiting. Bound
    // active polling to 2 ms instead of repeatedly querying the same MFT
    // event queue within one owner cycle. Always reap tracked input leases;
    // EOS/drain is never coalesced and every output packet is still consumed.
    if (!eos_ && now - last_receive_poll_ < interval) {
      if (idle)
        ++coalesced_receive_polls_;
      else
        ++coalesced_active_receive_polls_;
      reap();
      return;
    }
    last_receive_poll_ = now;
    auto error = fresh<ness_error_v2>();
    for (unsigned i = 0; i < 64; ++i) {
      ness_packet_v2 *packet{};
      const auto receive_started = qpc_hns();
      const auto result = ness_encoder_receive_v2(encoder_, &packet, &error);
      receive_calls_.observe(receive_started, plan_.capture.trace);
      checked(result, error);
      if (result == NESS_V2_END) {
        drained_ = true;
        break;
      }
      if (result == NESS_V2_AGAIN)
        break;
      struct Owned {
        ness_packet_v2 *p;
        ~Owned() { ness_packet_release_v2(p); }
      } owned{packet};
      if (!codec_set_) {
        ness_codec_v2 *codec{};
        const auto available =
            ness_encoder_description_v2(encoder_, &codec, &error);
        checked(available, error);
        if (available == NESS_V2_OK) {
          struct Codec {
            ness_codec_v2 *p;
            ~Codec() { ness_codec_release_v2(p); }
          } description{codec};
          checked(ness_mkv_set_codec_v2(muxer_, codec, &error), error);
          codec_set_ = true;
        }
      }
      const auto write_started = qpc_hns();
      checked(ness_mkv_write_packet_v2(muxer_, packet, &error), error);
      muxer_calls_.observe(write_started, true);
      ++packets_;
    }
    reap();
  }
  bool prepare_frame_for_use(capture::Frame &frame) {
    if(readback_ && !frame.surface->cpu_pixels().empty()) return true;
    if (!plan_.capture.defer_conversion)
      return source_->prepare_frame(frame);
    const auto started = qpc_hns();
    const auto ready = source_->prepare_frame(frame);
    preparation_calls_.observe(started, plan_.capture.trace);
    return ready;
  }
  bool prepare_recording_frame(capture::Frame &frame) {
    if(!readback_) return prepare_frame_for_use(frame);
    if(!frame.surface->cpu_pixels().empty()) return true;
    const auto started=qpc_hns();
    const auto result=source_->prepare_readback(frame,*readback_);
    const bool ready=result==capture::Pipeline::ReadbackPreparation::Unsupported
        ? source_->prepare_frame(frame) && readback_->prepare(frame)
        : result==capture::Pipeline::ReadbackPreparation::Ready;
    preparation_calls_.observe(started, plan_.capture.trace);
    return ready;
  }
  bool submit() {
    if (!pending_)
      return true;
    if (inflight_.size() >= plan_.encoder.max_pending_frames) {
      ++pressure_;
      return false;
    }
    auto &frame = *pending_;
    if(plan_.capture.defer_conversion && frames_ && capture::replace_repeated_candidate(
        frame,previous_id_,incoming_,
        frame_time(frames_+1,plan_.encoder.fps_num,plan_.encoder.fps_den),
        [&](int64_t timestamp) { return clock_->map(timestamp); },
        [&](const capture::Frame& candidate) { return eligible(candidate); },decimated_)) {
      last_=frame;
      ++repeated_candidate_upgrades_;
    }
    if (frames_ && plan_.capture.defer_conversion) {
      const auto boundary = frame_time(frames_ + 1, plan_.encoder.fps_num, plan_.encoder.fps_den);
      const auto period = boundary - frame_time(frames_, plan_.encoder.fps_num, plan_.encoder.fps_den);
      if (const auto lead = capture::replace_repeated_interval_candidate(
              frame, previous_id_, incoming_, boundary, period,
              [&](int64_t timestamp) { return clock_->map(timestamp); },
              [&](const capture::Frame& candidate) { return eligible(candidate); })) {
        last_ = frame;
        ++interval_candidate_selections_;
        interval_candidate_maximum_lead_ = (std::max)(interval_candidate_maximum_lead_, *lead);
      }
    }
    if (!prepare_recording_frame(frame))
      return false;
    if (!frames_ && !first_latency_)
      first_latency_ = qpc_hns() - prepare_at_;
    const auto pts =
        frame_time(frames_, plan_.encoder.fps_num, plan_.encoder.fps_den);
    const auto duration =
        frame_time(frames_ + 1, plan_.encoder.fps_num, plan_.encoder.fps_den) -
        pts;
    auto error = fresh<ness_error_v2>();
    int32_t result{};
    const auto submit_started = qpc_hns();
    if (plan_.encoder.policy==NESS_V2_POLICY_REQUIRE_GPU) {
      auto input = fresh<ness_gpu_frame_v2>();
      input.resource_type = NESS_V2_D3D11_TEXTURE2D;
      input.format = NESS_V2_NV12;
      input.width = frame.width;
      input.height = frame.height;
      input.frame_id = frames_ + 1;
      input.pts_hns = pts;
      input.duration_hns = duration;
      input.texture = frame.surface->gpu_texture();
      input.device = frame.surface->gpu_device();
      input.adapter_luid = frame.surface->adapter_luid();
      ness_input_lease_v2 *lease{};
      result = ness_encoder_submit_gpu_v2(encoder_, &input, &lease, &error);
      if (result == NESS_V2_OK) {
        std::unique_ptr<ness_input_lease_v2, decltype(&ness_input_release_v2)>
            owned(lease, ness_input_release_v2);
        inflight_.emplace_back(frame.surface, owned.get());
        (void)owned.release();
        inflight_peak_ = (std::max)(inflight_peak_, uint64_t(inflight_.size()));
      }
    } else {
      auto input = fresh<ness_cpu_frame_v2>();
      input.format = NESS_V2_NV12;
      input.width = frame.width;
      input.height = frame.height;
      input.frame_id = frames_ + 1;
      input.pts_hns = pts;
      input.duration_hns = duration;
      const auto pixels = frame.surface->cpu_pixels();
      const auto *bytes = reinterpret_cast<const uint8_t *>(pixels.data());
      const auto luma = uint64_t(frame.stride) * frame.height;
      if(!bytes || frame.stride<frame.width || pixels.size()<luma+uint64_t(frame.stride)*(frame.height/2))
        reject(NSS_E_ARGUMENT,NSS_STAGE_ENCODER,"CPU encoder input has invalid NV12 storage");
      input.planes[0] = {bytes, luma, frame.stride, 0};
      input.planes[1] = {bytes + luma, pixels.size() - luma, frame.stride, 0};
      result = ness_encoder_submit_cpu_v2(encoder_, &input, &error);
    }
    submit_calls_.observe(submit_started, plan_.capture.trace);
    if (result == NESS_V2_E_BACKPRESSURE) {
      ++pressure_;
      return false;
    }
    checked(result, error);
    const bool repeated = previous_id_ == frame.id;
    if (repeated) {
      ++repeats_;
      const auto limit = plan_.camera ? 64U : 4096U;
      if (plan_.camera && forced_.size() == limit)
        forced_.erase(forced_.begin() + 32);
      if (forced_.size() < limit) {
        Json sample{{"index", frames_}, {"sourceId", frame.id}};
        // Bound the richer timing evidence; retain every existing identity
        // sample independently for decoded-media verification.
        if (forced_.size() < 128) {
          sample["sourceCaptureQpcHns"] = frame.capture_hns;
          sample["sourceBeginQpcHns"] = frame.begin_hns;
          sample["sourceEndQpcHns"] = frame.end_hns;
          sample["submitBeginQpcHns"] = submit_started;
          sample["submitBeginActiveHns"] = clock_->active_time(submit_started);
          sample["windowBoundaryHns"] = pts + duration;
          sample["queuedCandidates"] = incoming_.size();
          sample["firstQueuedCaptureQpcHns"] = incoming_.empty()
              ? Json(nullptr) : Json(incoming_.front().capture_hns);
          sample["firstQueuedSourceId"] = incoming_.empty()
              ? Json(nullptr) : Json(incoming_.front().id);
          sample["firstQueuedBeginQpcHns"] = incoming_.empty()
              ? Json(nullptr) : Json(incoming_.front().begin_hns);
          sample["firstQueuedEndQpcHns"] = incoming_.empty()
              ? Json(nullptr) : Json(incoming_.front().end_hns);
          sample["captureProcessing"] = source_ && source_->wait_hint().processing;
          if (plan_.capture.trace && source_ && forced_.size() < 16)
            sample["acquisitionDiagnostic"] = source_->diagnostic_acquisition_history();
        }
        forced_.push_back(std::move(sample));
      }
    }
    const auto second = static_cast<uint64_t>(pts / hns_per_second);
    if (second < 600) {
      if (cadence_.empty() || cadence_.back().at("second") != second)
        cadence_.push_back(
            {{"second", second}, {"frames", 0}, {"forcedRepeats", 0}});
      auto &row = cadence_.back();
      row["frames"] = row["frames"].get<uint64_t>() + 1;
      row["forcedRepeats"] =
          row["forcedRepeats"].get<uint64_t>() + uint64_t(repeated);
    }
    previous_id_ = frame.id;
    if (samples_.size() < 64)
      samples_.push_back({{"index", frames_},
                          {"ptsHns", pts},
                          {"durationHns", duration},
                          {"sourceId", frame.id},
                          {"sourceBeginHns", frame.begin_hns},
                          {"sourceEndHns", frame.end_hns},
                          {"sourceCaptureHns", frame.capture_hns},{"programPresentationQpcHns",frame.presentation_hns?Json(*frame.presentation_hns):Json(nullptr)}});
    ++frames_;
    last_pts_ = pts;
    last_duration_ = duration;
    pending_.reset();
    return true;
  }
  bool eligible(const capture::Frame &frame) const {
    const auto begin = clock_->map(frame.begin_hns);
    return clock_->eligible(frame.begin_hns, frame.end_hns) && begin &&
           *begin + frame.end_hns - frame.begin_hns <= plan_.duration;
  }
  void schedule(int64_t active, bool cut_closed = false) {
    collect();
    if (!submit())
      return;
    while (last_) {
      const auto pts =
          frame_time(frames_, plan_.encoder.fps_num, plan_.encoder.fps_den);
      if (pts >= plan_.duration)
        break;
      const auto boundary =
          frame_time(frames_ + 1, plan_.encoder.fps_num, plan_.encoder.fps_den);
      if (frames_ && boundary + (cut_closed ? 0 : completion_grace) > active)
        break;
      bool selected = false;
      while (!incoming_.empty()) {
        const auto mapped = clock_->map(incoming_.front().presentation_hns.value_or(incoming_.front().capture_hns));
        if (mapped && *mapped > boundary)
          break;
        if (eligible(incoming_.front())) {
          if (selected)
            ++decimated_;
          last_ = std::move(incoming_.front());
          selected = true;
        } else
          ++rejected_;
        incoming_.pop_front();
      }
      if (!eligible(*last_)) {
        last_.reset();
        break;
      }
      pending_ = *last_;
      if (!submit())
        break;
      collect();
      if (frames_ > 24ULL * 3600 * 120)
        reject(NSS_E_INTERNAL, NSS_STAGE_ENCODER,
               "Frame index budget exhausted");
    }
  }
  void pump_preview() {
    if (!preview_frame_)
      return;
    if (!preview_->active()) {
      preview_frame_.reset();
      return;
    }
    if (prepare_frame_for_use(*preview_frame_)) {
      preview_->offer(*preview_frame_);
      preview_frame_.reset();
    }
  }
  void finish_cut(int64_t active) {
    // The acquisition cut and its completion grace have already closed.
    // Finish every due CFR slot from retained eligible surfaces. Conversion,
    // hybrid readback and encoder backpressure can all leave a pending submit.
    // Releasing the remaining candidates after one attempt truncates the tail
    // (especially the program track's two-frame completion grace).
    // This completion budget never extends the acquisition interval or changes
    // timestamps. The session's bounded Stop deadline still applies.
    const auto deadline = qpc_hns() + hns_per_second;
    for (;;) {
      schedule(active, true);
      if (!pending_)
        return;
      if (qpc_hns() >= deadline)
        reject(NSS_E_TIMEOUT, NSS_STAGE_PROCESS,
               "Selected frames did not finish before the cut drain deadline");
      wait();
    }
  }
  void retain_partial_cut(int64_t active) {
    partial_cut_tail_.reset();
    const auto begin = frame_time(frames_, plan_.encoder.fps_num, plan_.encoder.fps_den);
    const auto end = frame_time(frames_ + 1, plan_.encoder.fps_num, plan_.encoder.fps_den);
    if (!(begin < active && active < end))
      return;
    int64_t newest = -1;
    const auto retain = [&](const capture::Frame &frame) {
      const auto at = clock_->map(frame.presentation_hns.value_or(frame.capture_hns));
      if (eligible(frame) && at && *at <= active && *at > newest) {
        partial_cut_tail_ = frame;
        newest = *at;
      }
    };
    if (last_)
      retain(*last_);
    for (const auto &frame : incoming_)
      retain(frame);
    if (pending_)
      retain(*pending_);
    if (partial_cut_tail_)
      ++partial_cut_retained_;
  }
  void offer_preview(const capture::Frame &frame) {
    if (!plan_.capture.defer_conversion) {
      preview_->offer(frame);
      return;
    }
    pump_preview();
    if (preview_->active()) {
      preview_frame_ = frame;
      pump_preview();
    }
  }
  void prepare_closed_window_candidate() {
    if (!source_ || source_closed_ || !clock_ || paused_ ||
        !plan_.capture.defer_conversion || incoming_.size() < 2)
      return;
    const auto boundary = frame_time(frames_ + 1, plan_.encoder.fps_num, plan_.encoder.fps_den);
    capture::coalesce_window(incoming_, boundary,
        [&](int64_t timestamp) { return clock_->map(timestamp); }, decimated_);
    if (incoming_.size() < 2)
      return;
    auto &candidate = incoming_.front();
    const auto current = clock_->map(candidate.capture_hns);
    const auto future = clock_->map(incoming_[1].capture_hns);
    // GDI and WGC enqueue acquisitions in source-time order. A following
    // acquisition past this boundary identifies the last candidate in the
    // window. Preparing it now releases the borrowed DIB/WGC buffer sooner;
    // submission still uses the original CFR boundary and completion grace.
    if (candidate.format == capture::Frame::Format::Bgra && current && future &&
        *current <= boundary && *future > boundary && eligible(candidate) &&
        prepare_recording_frame(candidate)) {
      if (plan_.capture.gpu) ++gpu_candidates_prepared_;
      else ++cpu_candidates_prepared_;
    }
  }
  void poll_source() {
    if (clock_ && !paused_)
      capture::coalesce_window(
          incoming_,
          frame_time(frames_ + 1, plan_.encoder.fps_num, plan_.encoder.fps_den),
          [&](int64_t timestamp) { return clock_->map(timestamp); },
          decimated_);
    prepare_closed_window_candidate();
    if (!source_ || source_closed_ ||
        incoming_.size() >= plan_.capture.slots - 1)
      return;
    std::optional<capture::AcquisitionWindow> sampling;
    std::optional<int64_t> minimum_source_time;
    if (uses_window_sampling()) {
      const auto now = qpc_hns();
      const auto active = clock_->map(now);
      if (active && *active < plan_.duration) {
        sampling = capture::acquisition_window(*active, plan_.encoder.fps_num, plan_.encoder.fps_den);
        if (!window_sampling_.due(*sampling, *active)) {
          ++window_sampling_skips_;
          return;
        }
        if (const auto minimum = window_sampling_.minimum_source_time(*sampling))
          minimum_source_time = now - *active + *minimum;
      }
    }
    const auto capture_started = qpc_hns();
    auto frame = minimum_source_time ? source_->poll_after(*minimum_source_time) : source_->poll();
    capture_calls_.observe(capture_started, plan_.capture.trace);
    if (!frame)
      return;
    if (sampling) {
      const auto source_active = clock_->map(frame->capture_hns);
      if (source_active && window_sampling_.observe(*sampling, *source_active))
        ++window_sampling_acquisitions_;
    }
    offer_preview(*frame);
    ++received_;
    if (paused_ || !eligible(*frame)) {
      ++rejected_;
      return;
    }
    incoming_.push_back(std::move(*frame));
    queue_peak_ = (std::max)(queue_peak_, uint64_t(incoming_.size()));
    prepare_closed_window_candidate();
  }
  void update_stats() {
    auto error = fresh<ness_error_v2>();
    if (encoder_) {
      auto v = fresh<ness_encoder_stats_v2>();
      checked(ness_encoder_stats_get_v2(encoder_, &v, &error), error);
      encoder_stats_ = {
          {"submitted", v.submitted},      {"emitted", v.emitted},
          {"pending", v.pending},          {"queuedPackets", v.queued_packets},
          {"queuedBytes", v.queued_bytes}, {"discarded", v.discarded},
          {"drained", bool(v.drained)}};
    }
    if (muxer_) {
      auto v = fresh<ness_muxer_stats_v2>();
      checked(ness_mkv_stats_get_v2(muxer_, &v, &error), error);
      muxer_stats_ = {{"packets", v.packets},
                      {"bufferedPackets", v.buffered_packets},
                      {"bytesWritten", v.bytes_written},
                      {"durationHns", v.duration_hns},
                      {"finalized", bool(v.finalized)},
                      {"failed", bool(v.failed)}};
    }
  }
  void persist_manifest(bool partial = false) {
    const std::filesystem::path output(
        std::u8string(reinterpret_cast<const char8_t *>(plan_.output.data()),
                      plan_.output.size()));
    const auto pending =
                   output.native() + (partial ? L".partial.manifest.pending"
                                              : L".manifest.pending"),
               final = output.native() + (partial ? L".partial.manifest.json"
                                                  : L".manifest.json");
    const auto bytes =
        Json{{"version", 1},
             {"sessionId", session_id()},
             {"mediaComplete", !partial},
             {"interactive", plan_.interactive},
             {"outputPath", plan_.output},
             {"container", "matroska"},
             {"codec", "h264"},
             {"bytes", std::filesystem::exists(output)
                           ? std::filesystem::file_size(output)
                           : 0},
             {"frames", frames_},
             {"packets", packets_},
             {"durationHns", last_pts_ + last_duration_},
             {"encoder", encoder_info_},
             {"audio", audio_ ? audio_->snapshot() : audio_final_},
             {"requestedAudio", plan_.audio},
             {"firstError",
              partial
                  ? Json{{"code", recording_fault_ ? recording_fault_.code
                                                   : NSS_E_CANCELLED},
                         {"stage", recording_fault_ ? recording_fault_.stage
                                                    : NSS_STAGE_CONTROL},
                         {"hresult", recording_fault_.hresult},
                         {"message", recording_fault_ ? recording_fault_.message
                                                      : "Completion cancelled"}}
                  : Json(nullptr)},
             {"activeIntervals",
              published_snapshot()->value("activeIntervals", Json::array())},
             {"capture", capture::plan_json(plan_.capture)},
             {"camera",
              plan_.camera ? camera::plan_json(*plan_.camera) : Json(nullptr)},
             {"finalized", !partial}}
            .dump(2) +
        "\n";
    HANDLE handle = CreateFileW(pending.c_str(), GENERIC_WRITE, 0, nullptr,
                                CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
      reject(NSS_E_IO, NSS_STAGE_STORAGE,
             "Complete manifest could not be created",
             HRESULT_FROM_WIN32(GetLastError()));
    DWORD written{};
    const bool saved =
        WriteFile(handle, bytes.data(), static_cast<DWORD>(bytes.size()),
                  &written, nullptr) &&
        written == bytes.size() && FlushFileBuffers(handle);
    const auto write_error = GetLastError();
    const bool closed = CloseHandle(handle) != FALSE;
    if (!saved || !closed)
      reject(NSS_E_IO, NSS_STAGE_STORAGE,
             "Complete manifest write/flush/close failed",
             HRESULT_FROM_WIN32(write_error));
    if (!MoveFileExW(pending.c_str(), final.c_str(), MOVEFILE_WRITE_THROUGH))
      reject(NSS_E_IO, NSS_STAGE_STORAGE,
             "Complete manifest could not be published",
             HRESULT_FROM_WIN32(GetLastError()));
  }

public:
  explicit Recording(Plan p, std::function<std::unique_ptr<capture::Pipeline>()> factory = {}) : plan_(std::move(p)), source_factory_(std::move(factory)), completion_grace((plan_.source_id==3?2:1)*capture::completion_grace_hns) {}
  ~Recording() override {
    // Normal destruction follows Resources on the worker. Partial Prepare is
    // also cleaned there.
    if (timer_)
      CloseHandle(timer_);
  }
  bool self_paced() const noexcept override { return true; }
  void select_software_before_prepare(const Fault &failure) {
    if(!plan_.automatic_route || source_ || encoder_ || muxer_ || timer_)
      reject(NSS_E_STATE,NSS_STAGE_CONTROL,"Scene fallback must precede track Prepare");
    plan_.capture.gpu=false;plan_.automatic_encoder=false;
    plan_.encoder.backend=NESS_V2_BACKEND_X264;
    plan_.encoder.policy=NESS_V2_POLICY_SOFTWARE;
    selection_.push_back({{"kind","capture"},{"route","GPU scene"},{"accepted",false},
      {"code",failure.code},{"nativeCode",failure.hresult},{"reason",failure.message}});
  }
  Fault prepare(std::stop_token stop) override {
    return guard([&] {
      scheduling_.emplace();
      if (!plan_.audio.empty()) {
        audio_ = std::make_unique<audio::Session>(plan_.audio);
        audio_->prepare();
      }
      prepare_at_ = qpc_hns();
      timer_ = CreateWaitableTimerExW(nullptr, nullptr,
                                      CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                      TIMER_MODIFY_STATE | SYNCHRONIZE);
      if (!timer_)
        reject(NSS_E_UNAVAILABLE, NSS_STAGE_CAPTURE,
               "High resolution recording timer unavailable",
               HRESULT_FROM_WIN32(GetLastError()));
      std::optional<capture::Frame> first;
      auto initialize_source=[&] {
        plan_.capture.defer_conversion=!plan_.camera && !source_factory_ &&
          (plan_.capture.gpu || plan_.capture.slots>=3);
        plan_.capture.prefer_legacy_duplication=plan_.capture.desktop_duplication &&
          plan_.capture.defer_conversion;
        source_=source_factory_?source_factory_():
          plan_.camera?(plan_.capture.gpu?capture::make_camera_gpu(*plan_.camera,plan_.capture):
            camera::make_cpu(*plan_.camera,plan_.capture)):
          plan_.capture.gpu?capture::make_gpu(plan_.capture):capture::make_cpu(plan_.capture);
        if(plan_.capture.gpu || plan_.camera) {
          const bool camera_warmup=plan_.camera && !source_factory_;
          camera::StartupCadence warmup(plan_.camera?plan_.camera->fps_num:30,
                                       plan_.camera?plan_.camera->fps_den:1);
          const auto deadline=qpc_hns()+3*hns_per_second;
          for(;;) {
            if(auto frame=source_->poll()) {
              first=std::move(frame);
              if(!camera_warmup || warmup.observe(first->capture_hns)) break;
            }
            if(stop.stop_requested()) reject(NSS_E_CANCELLED,NSS_STAGE_CONTROL,"Prepare interrupted");
            if(qpc_hns()>deadline) reject(NSS_E_TIMEOUT,NSS_STAGE_CAPTURE,"Prepare first-content deadline exceeded");
            wait();
          }
          if(camera_warmup) source_preparation_=warmup.metrics();
          offer_preview(*first);
        }
      };
      auto fallback_capture=[&](const Fault &failure) {
        if(!plan_.automatic_route || source_factory_ ||
           (failure.code!=NSS_E_UNAVAILABLE && failure.code!=NSS_E_TIMEOUT))
          reject(failure.code,failure.stage,failure.message.c_str(),failure.hresult);
        if(plan_.capture.width>1280 || plan_.capture.height>720)
          reject(NSS_E_UNAVAILABLE,NSS_STAGE_PLAN,
            "Auto cannot preserve this geometry within the FullSoftware 1280x720 budget");
        selection_.push_back({{"kind","capture"},{"route","GPU"},{"accepted",false},
          {"code",failure.code},{"nativeCode",failure.hresult},{"reason",failure.message}});
        first.reset();
        if(source_) source_->close();
        source_.reset();plan_.capture.gpu=false;plan_.automatic_encoder=false;
        plan_.encoder.backend=NESS_V2_BACKEND_X264;plan_.encoder.policy=NESS_V2_POLICY_SOFTWARE;
        initialize_source();
      };
      try {initialize_source();}
      catch(const capture::Error &failure) {fallback_capture({failure.code,failure.stage,failure.native,failure.what()});}
      catch(const RecordingFailure &failure) {fallback_capture(failure.error);}
      selection_.push_back({{"kind","capture"},{"route",plan_.capture.gpu?"GPU":"FullSoftware"},{"accepted",true}});
      auto error=fresh<ness_error_v2>();
      auto device=fresh<ness_gpu_device_v2>();
      if(plan_.capture.gpu) {
        device.device=first->surface->gpu_device();
        device.adapter_luid=first->surface->adapter_luid();
      }
      checked(select_encoder(plan_.encoder,plan_.capture.gpu?&device:nullptr,
        plan_.automatic_encoder,plan_.allow_hybrid,&encoder_,error,
        [&](uint32_t backend,int32_t result,const ness_error_v2 &diagnostic) {
          selection_.push_back({{"kind","encoder"},{"backend",encoder_name(backend)},
            {"adapterLuid",plan_.encoder.policy==NESS_V2_POLICY_REQUIRE_GPU?Json(device.adapter_luid):Json(nullptr)},
            {"accepted",result==NESS_V2_OK},{"code",result},{"nativeCode",diagnostic.native_code},
            {"reason",reinterpret_cast<const char *>(diagnostic.message_utf8)}});
        }),error);
      if(plan_.encoder.policy==NESS_V2_POLICY_SOFTWARE) {
        if(plan_.capture.gpu) {
          readback_=std::make_unique<capture::Readback>(plan_.capture.trace,plan_.capture.prefer_query_completion);
          const auto deadline=qpc_hns()+3*hns_per_second;
          while(!prepare_recording_frame(*first)) {
            if(stop.stop_requested()) reject(NSS_E_CANCELLED,NSS_STAGE_CONTROL,"Readback Prepare interrupted");
            if(qpc_hns()>deadline) reject(NSS_E_TIMEOUT,NSS_STAGE_PROCESS,"Readback Prepare copy timed out");
            wait();
          }
        }
      }
      if(plan_.encoder.policy==NESS_V2_POLICY_REQUIRE_GPU) {
        auto gpu=fresh<ness_gpu_info_v2>();
        checked(ness_encoder_gpu_info_get_v2(encoder_,&gpu,&error),error);
        encoder_info_={{"implementationId",reinterpret_cast<const char *>(gpu.implementation_utf8)},
          {"hardwareUrl",reinterpret_cast<const char *>(gpu.hardware_url_utf8)},
          {"clsid",reinterpret_cast<const char *>(gpu.clsid_utf8)},{"adapterLuid",gpu.adapter_luid},
          {"asynchronous",bool(gpu.asynchronous)},{"d3d11Aware",bool(gpu.d3d11_aware)},
          {"hardware",true},{"software",false},{"input","D3D11 NV12 texture on capture device"}};
      } else {
        auto software=fresh<ness_encoder_info_v2>();
        checked(ness_encoder_info_get_v2(encoder_,&software,&error),error);
        encoder_info_={{"implementationId",reinterpret_cast<const char *>(software.implementation_utf8)},
          {"adapterLuid",nullptr},{"hardware",false},{"software",true},
          {"input",plan_.capture.gpu?"GPU NV12 readback, CPU planes copied before submit returns":"CPU NV12 planes copied before submit returns"}};
      }
      encoder_info_["backend"]=encoder_name(plan_.encoder.backend);
      encoder_info_["width"] = plan_.encoder.width;
      encoder_info_["height"] = plan_.encoder.height;
      encoder_info_["fpsNum"] = plan_.encoder.fps_num;
      encoder_info_["fpsDen"] = plan_.encoder.fps_den;
      encoder_info_["bitrateBps"] = plan_.encoder.bitrate_bps;
      encoder_info_["profile"] = plan_.encoder.profile;
      encoder_info_["color"] = "BT709 limited SDR";
      encoder_info_["maxBFrames"] = 0;
      if (stop.stop_requested())
        reject(NSS_E_CANCELLED, NSS_STAGE_CONTROL,
               "Prepare interrupted before output creation");
      auto output = fresh<ness_muxer_config_v2>();
      output.track_id = 1;
      output.max_buffered_packets = 32;
      output.max_buffered_bytes = 4 * 1024 * 1024;
      output.output_path_utf8 = {
          reinterpret_cast<const uint8_t *>(plan_.output.data()),
          plan_.output.size()};
      checked(ness_mkv_open_v2(&output, nullptr, &muxer_, &error), error);
      pipeline_ = source_->metrics();
      publish();
    });
  }
  Fault arm_start(SessionClock &clock) {
    return guard([&] {
      clock_ = &clock;
      if (audio_)
        audio_->start(clock);
      startup_at_ = qpc_hns();
    });
  }
  bool start_content_ready() const noexcept {return last_.has_value();}
  void check_start_progress(std::stop_token stop) const {
    if (stop.stop_requested())
      reject(NSS_E_CANCELLED, NSS_STAGE_CONTROL, "Start interrupted");
    if (qpc_hns() - startup_at_ > (plan_.capture.gpu ? 3 : 5) * hns_per_second)
      reject(NSS_E_TIMEOUT, NSS_STAGE_CAPTURE, "Start did not admit eligible first content and encode it");
  }
  Fault prime_start(std::stop_token stop) {
    return guard([&] {
      check_start_progress(stop);
      if (!last_) {
        if (!startup_candidate_) startup_candidate_ = source_->poll();
        if (startup_candidate_ && startup_candidate_->end_hns <= qpc_hns()) {
          offer_preview(*startup_candidate_);
          ++received_;
          if (eligible(*startup_candidate_))
            last_ = std::move(*startup_candidate_);
          else
            ++rejected_;
          startup_candidate_.reset();
        }
      }
      if (last_ && !plan_.capture.defer_conversion && !first_latency_)
        first_latency_ = qpc_hns() - prepare_at_;
    });
  }
  Fault encode_start(std::stop_token stop) {
    return guard([&] {
      check_start_progress(stop);
      if (!last_) reject(NSS_E_STATE,NSS_STAGE_CAPTURE,"Start requires retained first content");
      if (!frames_) schedule(clock_->active_time(qpc_hns()));
      admitted_ = frames_ != 0;
    });
  }
  Fault finish_start(std::stop_token stop) {
    return guard([&] {
      if (plan_.capture.defer_conversion) {
        // The session writes a durable state transition after Start. Preserve
        // the next owned acquisition before that I/O can occupy the owner.
        const auto before = received_;
        const auto deadline = qpc_hns() + capture::completion_grace_hns;
        while (received_ == before && qpc_hns() < deadline) {
          if (stop.stop_requested())
            reject(NSS_E_CANCELLED, NSS_STAGE_CONTROL, "Start prefetch interrupted");
          poll_source();
          if (received_ == before)
            wait();
        }
        startup_prefetched_ += received_ - before;
      }
      if (audio_)
        audio_->check();
      publish();
    });
  }
  Fault start(std::stop_token stop, int64_t, SessionClock &clock) override {
    if (auto error=arm_start(clock)) return error;
    while (!start_content_ready()) {
      if (auto error=prime_start(stop)) return error;
      if (!start_content_ready()) {if (auto error=guard([&] {wait();})) return error;}
    }
    while (!admitted_) {
      if (auto error=encode_start(stop)) return error;
      if (!admitted_) {if (auto error=guard([&] {wait();})) return error;}
    }
    return finish_start(stop);
  }
  Fault pause(std::stop_token) override {
    return guard([&] {
      if (clock_) {
        const auto until = qpc_hns() + completion_grace;
        do {
          poll_source();
          collect();
          wait();
        } while (qpc_hns() < until);
        const auto active = clock_->active_time(qpc_hns());
        finish_cut(active);
        // The next rational slot may contain active time on both sides of
        // this cut. Retain one eligible pre-cut candidate for that slot;
        // a later post-resume acquisition must not fill an earlier window.
        retain_partial_cut(active);
      }
      paused_ = true;
      incoming_.clear();
      last_.reset();
      pending_.reset();
      publish();
    });
  }
  Fault resume(std::stop_token, SessionClock &) override {
    return guard([&] {
      paused_ = false;
      window_sampling_.reset();
      incoming_.clear();
      last_.reset();
      pending_.reset();
      if (partial_cut_tail_ && eligible(*partial_cut_tail_)) {
        incoming_.push_back(std::move(*partial_cut_tail_));
        ++partial_cut_resumed_;
      }
      partial_cut_tail_.reset();
      publish();
    });
  }
  Fault tick(SessionClock &clock) override {
    return guard([&] {
      const auto tick_started = plan_.capture.trace ? qpc_hns() : 0;
      preview_->pump();
      pump_preview();
      if (audio_) {
        const auto audio_started = plan_.capture.trace ? qpc_hns() : 0;
        audio_->check();
        if (plan_.capture.trace) audio_check_calls_.observe(audio_started, true);
      }
      collect();
      // Admit completed capture work before closing a CFR window. Otherwise
      // a frame already ready at this wakeup could lose to an older repeat.
      // Shared taps can also have a second completed graph result queued.
      poll_source();
      if (source_factory_)
        poll_source();
      if (!paused_)
        schedule((std::min)(clock.active_time(qpc_hns()), plan_.duration));
      poll_source();
      if (!paused_ && !last_ && !incoming_.empty()) {
        last_ = std::move(incoming_.front());
        incoming_.pop_front();
      }
      if (!paused_)
        schedule((std::min)(clock.active_time(qpc_hns()), plan_.duration));
      pump_preview();
      if (qpc_hns() - last_publish_ >= hns_per_second) {
        const auto metrics_started = plan_.capture.trace ? qpc_hns() : 0;
        pipeline_ = source_->metrics();
        update_stats();
        if (plan_.capture.trace) metric_collection_calls_.observe(metrics_started, true);
        publish();
      }
      if (plan_.capture.trace) tick_work_calls_.observe(tick_started, true);
      wait();
    });
  }
  ShutdownProgress shutdown(ShutdownStage stage) override {
    bool pending = false;
    const auto error = guard([&] {
      auto diagnostic = fresh<ness_error_v2>();
      switch (stage) {
      case ShutdownStage::Sources:
        if (audio_)
          audio_->stop();
        if (!source_closed_ && source_) {
          if (clock_ && !failed_) {
            const auto until = qpc_hns() + completion_grace;
            do {
              poll_source();
              collect();
              wait();
            } while (qpc_hns() < until);
            const auto active =
                (std::min)(clock_->active_time(qpc_hns()), plan_.duration);
            expected_slots_ = scale_floor(
                static_cast<uint64_t>(active), plan_.encoder.fps_num,
                uint64_t(hns_per_second) * plan_.encoder.fps_den);
            if (frame_time(expected_slots_ + 1, plan_.encoder.fps_num,
                           plan_.encoder.fps_den) <= active)
              ++expected_slots_;
            if (admitted_)
              expected_slots_ = (std::max)(expected_slots_, uint64_t{1});
            finish_cut(active);
            if (frames_ < expected_slots_)
              reject(NSS_E_IO, NSS_STAGE_PROCESS,
                     "Recording could not complete every frame due before the final cut");
          }
          preview_frame_.reset();
          partial_cut_tail_.reset();
          preview_->close();
          source_->close();
          source_closed_ = true;
          pipeline_ = source_->metrics();
        }
        incoming_.clear();
        last_.reset();
        break;
      case ShutdownStage::Inflight:
        if (pending_ && !failed_) {
          collect();
          pending = !submit();
        }
        if (failed_)
          pending_.reset();
        break;
      case ShutdownStage::EncoderEos:
        if (encoder_ && !failed_ && !eos_) {
          checked(ness_encoder_drain_v2(encoder_, &diagnostic), diagnostic);
          eos_ = true;
        }
        break;
      case ShutdownStage::Packets:
        if (encoder_ && !failed_) {
          collect();
          pending = !drained_;
          update_stats();
        }
        if (!pending && encoder_) {
          ness_encoder_release_v2(encoder_);
          encoder_ = nullptr;
        }
        if (!pending && !failed_) {
          reap();
          auto gpu = fresh<ness_gpu_runtime_stats_v2>();
          checked(ness_gpu_runtime_stats_get_v2(&gpu, &diagnostic), diagnostic);
          pending =
              !inflight_.empty() || gpu.completion_states || gpu.input_leases;
        }
        break;
      case ShutdownStage::Output:
        if (muxer_ && !failed_ && admitted_ && !completion_cancelled_) {
          if (!drained_ || packets_ != frames_ || !codec_set_)
            reject(NSS_E_INTERNAL, NSS_STAGE_OUTPUT,
                   "Cannot finalize before every accepted frame and codec "
                   "description");
          checked(ness_mkv_finalize_v2(muxer_, &diagnostic), diagnostic);
          update_stats();
          if (audio_)
            audio_->finalize(last_pts_ + last_duration_);
          persist_manifest();
          complete_ = true;
        } else if (muxer_)
          (void)ness_mkv_discard_v2(muxer_, &diagnostic);
        break;
      case ShutdownStage::Resources:
        if(readback_ && !readback_->quiesce()) {pending=true;break;}
        if (encoder_) {
          ness_encoder_release_v2(encoder_);
          encoder_ = nullptr;
        }
        if (muxer_) {
          ness_mkv_release_v2(muxer_);
          muxer_ = nullptr;
        }
        inflight_.clear();
        incoming_.clear();
        pending_.reset();
        last_.reset();
        startup_candidate_.reset();
        source_.reset();
        if(readback_) {readback_final_=readback_->metrics();readback_.reset();}
        if (audio_) {
          audio_->stop();
          audio_final_ = audio_->snapshot();
          audio_.reset();
        }
        if (!plan_.audio.empty() && !complete_)
          persist_manifest(true);
        scheduling_.reset();
        break;
      default:
        break;
      }
      publish();
    });
    return {pending && !error, error};
  }
  Json metrics() const override {
    const auto publication = published_snapshot();
    return *publication;
  }
  Json stages() const override {
    const auto publication = published_snapshot();
    const auto &published = *publication;
    const Json empty = Json::object();
    const auto &capture = published.contains("capture") ? published.at("capture") : empty;
    const char *conversion = !plan_.capture.gpu ? "CPU BT709 NV12"
        : capture.value("shaderConversionSelected", false)
            ? "D3D11 native shader BT709 NV12" : "D3D11 Video Processor NV12";
    auto result = Json::array(
        {{{"implementationId",
           plan_.camera        ? "DirectShow native camera CPU buffers"
           : plan_.capture.gpu ? "WGC"
                               : "GDI DIB"},
          {"adapterLuid", plan_.capture.gpu
                              ? Json(plan_.capture.display.adapter_luid)
                              : Json(nullptr)},
          {"hardware", plan_.camera ? false : plan_.capture.gpu},
          {"software", plan_.camera ? true : !plan_.capture.gpu}},
         {{"implementationId",
           conversion},
          {"adapterLuid", plan_.capture.gpu
                              ? Json(plan_.capture.display.adapter_luid)
                              : Json(nullptr)},
          {"hardware", plan_.capture.gpu},
          {"software", !plan_.capture.gpu}},
         published.value("encoder", Json::object()),
         {{"implementationId", "NessMuxer V2 MKV"},
          {"hardware", false},
          {"software", true}}});
    for (const auto &track : published.value("audio", Json::array()))
      if (track.is_object() && track.contains("nativeFormat"))
        result.push_back(
            {{"implementationId", track.value("kind", "") == "loopback"
                                      ? "WASAPI render loopback"
                                      : "WASAPI microphone"},
             {"software", true},
             {"hardware", false},
             {"adapterLuid", nullptr},
             {"endpointId", track.at("endpointId")},
             {"negotiatedFormat", track.at("nativeFormat")},
             {"outputFormat", "RF64 PCM16 stereo 48kHz"},
             {"clockKeepalive", track.value("loopbackClockKeepalive", false)}});
    return result;
  }
  Json assets() const override {
    const auto publication = published_snapshot();
    if (!complete_)
      return Json::array();
    auto result = Json::array({{{"path", plan_.output},
                                {"container", "matroska"},
                                {"finalized", true}}});
    for (const auto &track : publication->value("audio", Json::array()))
      result.push_back(track);
    return result;
  }
  std::shared_ptr<preview::Hub> preview(uint64_t source) override {
    const uint64_t actual = plan_.source_id ? plan_.source_id : (plan_.camera ? 2 : 1);
    if (source != actual)
      return {};
    preview_->bind(session_id(), actual);
    return preview_;
  }
  bool media_complete() const noexcept override { return complete_; }
  void cancel_media_completion() noexcept override {
    completion_cancelled_ = true;
  }
  bool first_content_admitted() const noexcept override { return admitted_; }
};
class IndependentVideo final : public Backend {
  std::unique_ptr<Backend> screen_, camera_;
  mutable std::mutex mutex_;
  Json metrics_ = Json::object(), stages_ = Json::array(),
       assets_ = Json::array();
  std::atomic<bool> complete_{};
  int64_t last_publish_{};
  void publish() {
    auto screen = screen_->metrics(), camera = camera_->metrics();
    screen["cameraTrack"] = camera;
    screen["independentVideoTracks"] = 2;
    screen["cameraSourceCpuBytes"] = camera.value("capture", Json::object())
                                         .value("sourceCpuBytes", uint64_t{});
    auto stages = screen_->stages();
    for (auto &item : stages) {
      item["sourceId"] = 1;
      item["sourceKind"] = "screen";
    }
    for (auto item : camera_->stages()) {
      item["sourceId"] = 2;
      item["sourceKind"] = "camera";
      stages.push_back(std::move(item));
    }
    auto assets = screen_->assets();
    for (const auto &item : camera_->assets())
      assets.push_back(item);
    std::lock_guard lock(mutex_);
    metrics_ = std::move(screen);
    stages_ = std::move(stages);
    assets_ = std::move(assets);
    last_publish_ = qpc_hns();
  }
  template <class F> Fault each(F action) {
    auto first = action(*screen_);
    if (first) {
      publish();
      return first;
    }
    auto second = action(*camera_);
    publish();
    return second;
  }

public:
  IndependentVideo(Json screen, Json camera)
      : screen_(std::make_unique<Recording>(plan(screen))),
        camera_(std::make_unique<Recording>(plan(camera))) {}
  bool self_paced() const noexcept override { return true; }
  Fault prepare(std::stop_token stop) override {
    screen_->bind_session(session_id());
    camera_->bind_session(session_id());
    return each([&](Backend &child) { return child.prepare(stop); });
  }
  Fault start(std::stop_token stop, int64_t epoch,
              SessionClock &clock) override {
    return each(
        [&](Backend &child) { return child.start(stop, epoch, clock); });
  }
  Fault pause(std::stop_token stop) override {
    return each([&](Backend &child) { return child.pause(stop); });
  }
  Fault resume(std::stop_token stop, SessionClock &clock) override {
    return each([&](Backend &child) { return child.resume(stop, clock); });
  }
  Fault tick(SessionClock &clock) override {
    auto first = screen_->tick(clock);
    if (first) {
      publish();
      return first;
    }
    auto second = camera_->tick(clock);
    if (second || qpc_hns() - last_publish_ >= hns_per_second / 5)
      publish();
    return second;
  }
  ShutdownProgress shutdown(ShutdownStage stage) override {
    auto first = screen_->shutdown(stage), second = camera_->shutdown(stage);
    if (stage == ShutdownStage::Resources)
      complete_ = screen_->media_complete() && camera_->media_complete();
    publish();
    return {first.pending || second.pending,
            first.error ? first.error : second.error};
  }
  Json metrics() const override {
    std::lock_guard lock(mutex_);
    return metrics_;
  }
  Json stages() const override {
    std::lock_guard lock(mutex_);
    return stages_;
  }
  Json assets() const override {
    std::lock_guard lock(mutex_);
    return assets_;
  }
  bool media_complete() const noexcept override { return complete_; }
  void cancel_media_completion() noexcept override {
    screen_->cancel_media_completion();
    camera_->cancel_media_completion();
  }
  bool first_content_admitted() const noexcept override {
    return screen_->first_content_admitted() &&
           camera_->first_content_admitted();
  }
  std::shared_ptr<preview::Hub> preview(uint64_t source) override {
    screen_->bind_session(session_id());
    camera_->bind_session(session_id());
    return source == 1   ? screen_->preview(source)
           : source == 2 ? camera_->preview(source)
                         : nullptr;
  }
};
#include "scene_backend.hpp"
} // namespace
std::unique_ptr<Backend> make_recording_backend(const Json &request,
                                                Fault &error) {
  try {
    if (request.contains("scene")) return std::make_unique<SceneBackend>(request);
    if (request.contains("capture") && request.contains("camera")) {
      keys(request,
           {"version", "capture", "camera", "cameraOutput", "cameraEncoder",
            "cameraOutputPath", "encoder", "outputPath", "durationMs",
            "interactive", "pauseScript", "audio"});
      auto screen = request;
      for (const auto *key :
           {"camera", "cameraOutput", "cameraEncoder", "cameraOutputPath"})
        screen.erase(key);
      Json camera = {
          {"version", 1},
          {"camera", request.at("camera")},
          {"mode", request.at("capture").at("mode")},
          {"output", request.value("cameraOutput", Json::object())},
          {"encoder", request.value("cameraEncoder", request.at("encoder"))},
          {"outputPath", request.at("cameraOutputPath")}};
      for (const auto *key : {"durationMs", "interactive", "pauseScript"})
        if (request.contains(key))
          camera[key] = request.at(key);
      return std::make_unique<IndependentVideo>(std::move(screen),
                                                std::move(camera));
    }
    return std::make_unique<Recording>(plan(request));
  } catch (const RecordingFailure &e) {
    error = e.error;
  } catch (const capture::Error &e) {
    error = {e.code, e.stage, e.native, e.what()};
  } catch (const std::exception &e) {
    error = {NSS_E_ARGUMENT, NSS_STAGE_PLAN, 0, e.what()};
  }
  return {};
}
} // namespace nss
