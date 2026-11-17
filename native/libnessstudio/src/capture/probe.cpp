#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "capture.hpp"
#include "scheduling.hpp"
#include <chrono>
#include <deque>
#include <future>
#include <thread>
#include <windows.h>

namespace nss::capture {
namespace {
class ProbeWait final {
  HANDLE timer_ = CreateWaitableTimerExW(nullptr, nullptr,
                                         CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                         TIMER_MODIFY_STATE | SYNCHRONIZE);

public:
  ProbeWait() {
    if (!timer_)
      checked(HRESULT_FROM_WIN32(GetLastError()), NSS_STAGE_CAPTURE,
              "High-resolution capture timer unavailable");
  }
  ~ProbeWait() {
    if (timer_)
      CloseHandle(timer_);
  }
  void tick() {
    LARGE_INTEGER due{};
    due.QuadPart = -10000;
    if (!SetWaitableTimerEx(timer_, &due, 0, nullptr, nullptr, nullptr, 0))
      checked(HRESULT_FROM_WIN32(GetLastError()), NSS_STAGE_CAPTURE,
              "Capture timer could not be armed");
    if (WaitForSingleObject(timer_, 1000) != WAIT_OBJECT_0)
      throw Error(NSS_E_TIMEOUT, NSS_STAGE_CAPTURE, 0,
                  "Capture pacing timer timed out");
  }
};
Json run(const Json &request) try {
  auto config = plan(request);
  detail::CaptureTask scheduling;
  ProbeWait wait;
  const auto duration = config.duration_ms;
  if (duration < 100 || duration > 120000)
    throw Error(NSS_E_ARGUMENT, NSS_STAGE_PLAN, 0,
                "Capture probe duration must be 100..120000 ms");
  const auto before = qpc_hns();
  auto pipeline = config.gpu ? make_gpu(config) : make_cpu(config);
  const auto first_limit = before + (config.gpu ? 3 : 5) * hns_per_second;
  std::optional<Frame> last;
  while (!(last = pipeline->poll())) {
    if (qpc_hns() > first_limit)
      throw Error(
          NSS_E_TIMEOUT, NSS_STAGE_CAPTURE, 0,
          "Capture did not deliver valid first content before its deadline");
    wait.tick();
  }
  const auto timestamp_lead = (std::max)(int64_t{0}, last->end_hns - qpc_hns());
  // A compositor timestamp may name a presentation instant slightly ahead of
  // the completed GPU copy. Preserve that timestamp and wait for eligibility;
  // never submit a negative time to the common CFR scheduler or invent an
  // offset.
  while (qpc_hns() < last->end_hns) {
    if (qpc_hns() > first_limit)
      throw Error(NSS_E_TIMEOUT, NSS_STAGE_CAPTURE, 0,
                  "First capture timestamp did not become eligible before its "
                  "deadline");
    wait.tick();
  }
  const auto first_ready = qpc_hns();
  SessionClock clock;
  clock.start(last->begin_hns);
  EligibleVideoCache cache;
  if (!cache.admit(clock, {last->id, last->begin_hns, last->end_hns, false}))
    throw Error(NSS_E_INTERNAL, NSS_STAGE_CAPTURE, 0,
                "First capture interval is not eligible");
  CfrScheduler scheduler(config.fps_num, config.fps_den);
  const auto epoch = last->begin_hns, end = epoch + int64_t(duration) * 10000;
  uint64_t received = 1, rejected = 0, slots = 0, hash = 1469598103934665603ULL,
           previous_id = 0, input_decimation = 0, incoming_peak = 0;
  std::deque<Frame> incoming;
  int64_t last_pts{}, last_duration{};
  Json samples = Json::array(),forced_samples=Json::array();
  auto emit = [&](int64_t pts) {
    const auto frame = scheduler.next(pts, cache, last->id != previous_id);
    if (!frame || frame->missing)
      throw Error(NSS_E_INTERNAL, NSS_STAGE_CAPTURE, 0,
                  "CFR emitted a slot without first content");
    previous_id = last->id;
    if(frame->forced && forced_samples.size()<128)
      forced_samples.push_back({{"index",frame->index},{"sourceId",last->id},
          {"sourceCaptureHns",last->capture_hns-epoch},
          {"sourceBeginHns",last->begin_hns-epoch},{"sourceEndHns",last->end_hns-epoch},
          {"windowEndHns",frame->pts+frame->duration},
          {"oldestPendingCaptureHns",incoming.empty()?Json(nullptr):Json(incoming.front().capture_hns-epoch)},
          {"pending",incoming.size()},{"nowHns",qpc_hns()-epoch}});
    ++slots;
    last_pts = frame->pts;
    last_duration = frame->duration;
    for (const auto value :
         {frame->index, uint64_t(frame->pts), uint64_t(frame->duration)}) {
      hash ^= value;
      hash *= 1099511628211ULL;
    }
    if (samples.size() < 16)
      samples.push_back({{"index", frame->index},
                         {"ptsHns", frame->pts},
                         {"durationHns", frame->duration},
                         {"frameId", frame->frame_id},
                         {"sourceBeginHns", last->begin_hns},
                         {"sourceEndHns", last->end_hns},
                         {"sourceCaptureHns", last->capture_hns},
                         {"sourceEstimated", last->estimated}});
  };
  // PTS describes the capture window, not submission/completion wall time.
  // Keep at most slots-1 pending surfaces and close each rational window before
  // selecting its newest source timestamp. The whole acquisition interval still
  // must fit the active session, including start/stop cuts. This permits GPU
  // completion without repeating merely because it arrived just after the PTS.
  emit(0);
  auto schedule = [&](int64_t active) {
    for (;;) {
      const auto pts = frame_time(slots, config.fps_num, config.fps_den);
      if (pts >= int64_t(duration) * 10000)
        return;
      const auto boundary =
          (std::min)(frame_time(slots + 1, config.fps_num, config.fps_den),
                     int64_t(duration) * 10000);
      if (boundary + completion_grace_hns > active)
        return;
      bool selected = false;
      while (!incoming.empty() &&
             incoming.front().capture_hns - epoch <= boundary) {
        if (selected)
          ++input_decimation;
        last = std::move(incoming.front());
        incoming.pop_front();
        selected = true;
      }
      if (selected && !cache.admit(clock, {last->id, last->begin_hns,
                                           last->end_hns, false}))
        throw Error(NSS_E_INTERNAL, NSS_STAGE_CAPTURE, 0,
                    "Selected capture window is not eligible");
      emit(pts);
    }
  };
  for (;;) {
    const auto now = qpc_hns();
    schedule(now - epoch);
    if (now >= end + completion_grace_hns)
      break;
    coalesce_window(incoming,
                    (std::min)(frame_time(slots + 1, config.fps_num, config.fps_den),
                               int64_t(duration) * 10000),
                    [&](int64_t timestamp) { return std::optional<int64_t>(timestamp - epoch); },
                    input_decimation);
    // Source acquisition is capacity-driven. Pacing CPU acquisition exactly at
    // the output boundary couples GDI latency/jitter to CFR and leaves no ready
    // candidate after a slow acquisition. The common scheduler alone paces the
    // output; excess source frames are explicitly counted as decimation.
    if (incoming.size() < config.slots - 1) {
      auto frame = pipeline->poll();
      if (frame) {
        ++received;
        const auto previous_end =
            incoming.empty() ? last->end_hns : incoming.back().end_hns;
        if (frame->begin_hns < end && frame->end_hns <= end &&
            frame->end_hns >= previous_end &&
            clock.eligible(frame->begin_hns, frame->end_hns)) {
          incoming.push_back(std::move(*frame));
          incoming_peak = (std::max)(incoming_peak, uint64_t(incoming.size()));
        } else
          ++rejected;
      }
    }
    schedule(qpc_hns() - epoch);
    wait.tick();
  }
  schedule(int64_t(duration) * 10000 + completion_grace_hns);
  clock.stop(end);
  incoming.clear();
  last.reset();
  pipeline->close();
  const auto metrics = pipeline->metrics();
  pipeline.reset();
  return {
      {"status", "PASS"},
      {"scope", "capture-processing-only"},
      {"recordingImplemented", false},
      {"encoderUsed", false},
      {"requested", request},
      {"effective", plan_json(config)},
      {"pipeline", metrics},
      {"framesReceived", received},
      {"rejectedAcquisitionIntervals", rejected},
      {"schedulerInputDecimation", input_decimation},
      {"schedulerQueuePeak", incoming_peak},
      {"schedulerQueueLimit", config.slots - 1},
      {"selectionPolicy",
       "newest source timestamp within the closed rational output window; "
       "bounded one-frame capture completion grace; "
       "whole acquisition interval eligible in active session; one interval "
       "consumption latency; no timestamp offset"},
      {"firstFrameLatencyMs", double(first_ready - before) / 10000},
      {"firstFrameTimestampLeadHns", timestamp_lead},
      {"pacing",
       "native high-resolution waitable timer; no global timer-period change"},
      {"durationHns", int64_t(duration) * 10000},
      {"sourceEpochHns", epoch},
      {"slots", slots},
      {"lastPtsHns", last_pts},
      {"lastDurationHns", last_duration},
      {"timelineHash", hash},
      {"timelineSamples", samples},
      {"forcedRepeatSamples",forced_samples},
      {"reusedSlots", scheduler.reused},
      {"repeatsWithoutIndependentStaticConfirmation", scheduler.forced},
      {"missingSlots", scheduler.missing},
      {"staticPolicy", "retain last eligible texture; no black substitute; "
                       "source Closed/device removal is fatal"},
      {"livePipelines", live_pipelines.load()},
      {"liveSurfaces", live_surfaces.load()},
      {"liveCaptureBytes", live_capture_bytes.load()},
      {"liveFramePoolBytes", live_pool_bytes.load()},
      {"liveFrameLeases", live_frame_leases.load()}};
} catch (const Error &) {
  throw;
} catch (const std::out_of_range &error) {
  throw Error(NSS_E_INTERNAL, NSS_STAGE_CAPTURE, 0, error.what());
}
} // namespace
Json probe(const Json &request) {
  // WinRT and all immediate-context/GDI operations have one native owner. A
  // calling STA/managed frontend never becomes the capture dispatcher.
  return std::async(std::launch::async, [request] { return run(request); })
      .get();
}
} // namespace nss::capture
