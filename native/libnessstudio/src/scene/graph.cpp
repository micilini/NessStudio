#include "graph.hpp"
#include <thread>

namespace nss::scene {
namespace {
void wait() { std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
} // namespace
Graph::Graph(Snapshot snapshot, std::optional<capture::Plan> screen,
             std::optional<std::pair<camera::Plan, capture::Plan>> camera,
             bool gpu, bool raw, bool queued_program)
    : store_(std::move(snapshot)), gpu_(gpu) {
  queued_program_ = queued_program;
  if (screen)
    sources_[1].output = *screen;
  if (camera) {
    sources_[2].camera = camera->first;
    sources_[2].output = camera->second;
  }
  for (const auto &layer : store_.get()->layers) {
    if ((layer.source == 1 && !screen) || (layer.source == 2 && !camera))
      throw capture::Error(NSS_E_ARGUMENT, NSS_STAGE_PLAN, 0,
                           "Scene references an unopened source");
    sources_[layer.source].required = raw || layer.required;
  }
  if (raw) {
    sources_[1].required = screen.has_value();
    sources_[2].required = camera.has_value();
  }
}
void Graph::bind(uint64_t session) {
  sources_[1].preview->defer_gpu(gpu_);
  sources_[2].preview->defer_gpu(gpu_);
  program_->defer_gpu(gpu_);
  sources_[1].preview->bind(session, 1);
  sources_[2].preview->bind(session, 2);
  program_->bind(session, 3);
}
void Graph::prepare(std::stop_token stop) {
  std::optional<capture::Frame> reference;
  for (uint64_t id = 1; id <= 2; ++id) {
    auto &source = sources_[id];
    if (!source.output.width)
      continue;
    source.pipeline =
        source.camera
            ? (gpu_
                   ? (reference ? capture::make_camera_gpu_shared(
                                      *source.camera, source.output, *reference)
                                : capture::make_camera_gpu(*source.camera,
                                                           source.output))
                   : camera::make_cpu(*source.camera, source.output))
            : (gpu_ ? capture::make_gpu(source.output)
                    : capture::make_cpu(source.output));
    ++source.opens;
    camera::StartupCadence warmup(source.camera ? source.camera->fps_num : 30,
                                 source.camera ? source.camera->fps_den : 1);
    const auto deadline = qpc_hns() + 3 * hns_per_second;
    for (;;) {
      if (stop.stop_requested())
        throw capture::Error(NSS_E_CANCELLED, NSS_STAGE_CONTROL, 0,
                             "Shared source Prepare cancelled");
      if (auto frame = source.pipeline->poll()) {
        source.latest = std::move(frame);
        if (!source.camera || warmup.observe(source.latest->capture_hns))
          break;
      }
      if (qpc_hns() > deadline)
        throw capture::Error(NSS_E_TIMEOUT, NSS_STAGE_CAPTURE, 0,
                             "Shared source first content deadline exceeded");
      wait();
    }
    if (source.camera) source.preparation = warmup.metrics();
    if (!reference)
      reference = source.latest;
    source.eligible = source.latest;
  }
  const auto snapshot = store_.get();
  renderer_ = gpu_ ? make_gpu(*snapshot, *reference) : make_cpu(*snapshot);
  // Publish only after the whole graph is viable. A failed Prepare may retry
  // the same graph in software without exposing an abandoned GPU generation.
  for (auto &source : sources_)
    if (source.latest)
      source.preview->offer(*source.latest);
}
void Graph::prepare_software(std::stop_token stop) {
  if (!gpu_ || clock_ || closed_)
    throw capture::Error(NSS_E_STATE, NSS_STAGE_CONTROL, 0,
                         "Software fallback is only allowed before scene Start");
  const auto snapshot=store_.get();
  auto fits=[](uint32_t width,uint32_t height) {return width<=1280 && height<=720;};
  if(!fits(snapshot->width,snapshot->height) ||
     std::any_of(sources_.begin(),sources_.end(),[&](const auto &source) {
       return !fits(source.output.width,source.output.height);
     }))
    throw capture::Error(NSS_E_UNAVAILABLE,NSS_STAGE_PLAN,0,
                         "Auto cannot preserve this scene within the FullSoftware 1280x720 budget");
  if(renderer_) renderer_->close();
  renderer_.reset();
  program_frame_.reset();program_pending_.clear();
  for(auto &source:sources_) {
    if(source.pipeline) source.pipeline->close();
    source.pipeline.reset();source.latest.reset();source.eligible.reset();
    source.output.gpu=false;
    source.preview->defer_gpu(false);
  }
  gpu_=false;program_->defer_gpu(false);
  prepare(stop);
}
void Graph::start(SessionClock &clock) {
  clock_ = &clock;
  for (auto &source : sources_)
    source.eligible.reset();
  program_frame_.reset();
  program_pending_.clear();
  next_render_ = 0;
}
void Graph::resume() {
  // HoldLast may reuse content acquired in the preceding active interval.
  // Samples acquired during the pause never replace this eligible cache.
  // Strict freshness layers wait for new post-resume content instead.
  for (const auto &layer : store_.get()->layers)
    if (layer.missing == Missing::Fail)
      sources_[layer.source].eligible.reset();
  program_frame_.reset();
  program_pending_.clear();
  next_render_ = 0;
}
void Graph::poll() {
  if (closed_)
    return;
  const auto now = qpc_hns();
  if (now == last_poll_)
    return;
  last_poll_ = now;
  for (uint64_t id = 1; id <= 2; ++id) {
    auto &source = sources_[id];
    source.preview->pump();
    if (!source.pipeline)
      continue;
    try {
      ++source.polls;
      auto frame = source.pipeline->poll();
      if (!frame)
        continue;
      source.latest = std::move(frame);
      source.preview->offer(*source.latest);
      if (!clock_ ||
          clock_->eligible(source.latest->begin_hns, source.latest->end_hns))
        source.eligible = source.latest;
    } catch (const capture::Error &error) {
      if (source.required)
        throw;
      source.final = source.pipeline->metrics();
      source.final["sourceError"] = {{"code", error.code},
                                     {"stage", error.stage},
                                     {"hresult", error.native},
                                     {"message", error.what()}};
      source.pipeline->close();
      source.pipeline.reset();
      ++source.missing;
    }
  }
  program_->pump();
}
std::optional<capture::Frame> Graph::source(uint64_t id, uint64_t &previous) {
  if (!cached_consumers_)
    poll();
  const auto &frame = sources_.at(id).latest;
  if (!frame || frame->id == previous)
    return {};
  previous = frame->id;
  return frame;
}
std::optional<capture::Frame> Graph::program(uint64_t &previous) {
  if (!cached_consumers_)
    poll();
  if (auto ready = renderer_->take_ready())
    completed(std::move(*ready));
  const auto now = qpc_hns();
  if (now >= next_render_) {
    const auto snapshot = store_.get();
    std::vector<Input> inputs;
    bool ready = true;
    for (const auto &layer : snapshot->layers) {
      if (!layer.visible)
        continue;
      auto &source = sources_[layer.source];
      const auto &frame =
          clock_ && !clock_->map(now) ? source.latest : source.eligible;
      const auto selection = select(layer, frame, now, source.required);
      if (selection == Selection::Wait) {
        ready = false;
        continue;
      }
      if (selection == Selection::Fail)
        throw capture::Error(
            NSS_E_UNAVAILABLE, NSS_STAGE_CAPTURE, 0,
            "Required scene source missed its freshness deadline");
      if (selection == Selection::Background) {
        ++source.background;
        continue;
      }
      if (frame && now - frame->capture_hns > int64_t(layer.max_age_ms) * 10000)
        ++source.held;
      if (frame)
        inputs.push_back({layer.source, *frame});
    }
    if (ready) {
      std::optional<int64_t> presentation;
      int64_t next = now + frame_time(1, snapshot->fps_num, snapshot->fps_den);
      if (clock_ && clock_->map(now)) {
        const auto active = clock_->active_time(now);
        auto index =
            scale_floor(static_cast<uint64_t>(active), snapshot->fps_num,
                        uint64_t(hns_per_second) * snapshot->fps_den);
        while (frame_time(index + 1, snapshot->fps_num, snapshot->fps_den) <=
               active)
          ++index;
        const auto due = now - (active - frame_time(index, snapshot->fps_num,
                                                    snapshot->fps_den));
        presentation = clock_->map(due) ? due : now;
        next = now +
               frame_time(index + 1, snapshot->fps_num, snapshot->fps_den) -
               active;
      }
      auto frame = renderer_->render(*snapshot, inputs, presentation);
      if (frame)
        completed(std::move(*frame));
      // A GPU render can submit without returning a completed frame. Poll the
      // completion on the next output tick; bounded leases retain every input.
      next_render_ = next;
      applied_ = snapshot->revision;
      if (history_.empty() || history_.back().at("revision") != applied_) {
        if (history_.size() >= 64)
          history_.erase(history_.begin() + 1);
        auto item = json(*snapshot);
        item["appliedAtQpcHns"] = now;
        item["activeTimeHns"] = clock_ ? clock_->active_time(now) : 0;
        history_.push_back(std::move(item));
      }
    }
  }
  if (!program_frame_ || program_frame_->id == previous)
    return {};
  previous = program_frame_->id;
  return program_frame_;
}
void Graph::completed(capture::Frame frame) {
  program_frame_ = frame;
  program_->offer(frame);
  if (queued_program_ &&
      (!clock_ || clock_->eligible(frame.begin_hns, frame.end_hns))) {
    if (program_pending_.size() >= 6)
      throw capture::Error(NSS_E_BACKPRESSURE, NSS_STAGE_PROCESS, 0,
                           "Bounded program delivery queue exhausted");
    program_pending_.push_back(std::move(frame));
  }
}
std::optional<capture::Frame> Graph::consume_program(uint64_t &previous) {
  if (!cached_consumers_) {
    uint64_t producer_cursor{};
    (void)program(producer_cursor);
  }
  if (queued_program_) {
    while (!program_pending_.empty() && program_pending_.front().id <= previous)
      program_pending_.pop_front();
    if (program_pending_.empty())
      return {};
    auto frame = std::move(program_pending_.front());
    program_pending_.pop_front();
    previous = frame.id;
    return frame;
  }
  if (!program_frame_ || program_frame_->id == previous)
    return {};
  previous = program_frame_->id;
  return program_frame_;
}
std::shared_ptr<preview::Hub> Graph::preview(uint64_t id) {
  return id == 3 ? program_
                 : (id == 1 || id == 2 ? sources_[id].preview : nullptr);
}
void Graph::present_one() {
  // D3D9/WPF interop work is optional. Give the next composition deadline a
  // conservative 20ms margin and dispatch at most one consumer per iteration.
  if (!gpu_ || qpc_hns() + 200000 > next_render_)
    return;
  for (uint32_t attempt = 0; attempt < 3; ++attempt) {
    const auto source = preview_turn_;
    preview_turn_ = preview_turn_ % 3 + 1;
    if (preview(source)->dispatch_gpu())
      return;
  }
}
void Graph::close() {
  if (closed_)
    return;
  program_->close();
  for (auto &source : sources_)
    source.preview->close();
  if (renderer_)
    renderer_->close();
  program_frame_.reset();
  program_pending_.clear();
  for (auto &source : sources_) {
    if (source.pipeline) {
      source.pipeline->close();
      source.final = source.pipeline->metrics();
      source.pipeline.reset();
    }
    source.latest.reset();
    source.eligible.reset();
  }
  closed_ = true;
}
Json Graph::metrics() const {
  Json sources = Json::array();
  for (uint64_t id = 1; id <= 2; ++id) {
    const auto &source = sources_[id];
    if (!source.opens)
      continue;
    sources.push_back(
        {{"sourceId", id},
         {"captureInstances", source.opens},
         {"polls", source.polls},
         {"preparation", source.preparation},
         {"disconnects", source.missing},
         {"backgroundFrames", source.background},
         {"heldFrames", source.held},
         {"capture",
          source.pipeline ? source.pipeline->metrics() : source.final},
         {"required", source.required},
         {"latestFrameId", source.latest ? source.latest->id : 0},
         {"latestCaptureHns", source.latest ? source.latest->capture_hns : 0}});
  }
  return {{"captureInstances", sources.size()},
          {"programTimestampMethod",
           "qpc-active rational presentation grid; actual acquisition "
           "begin/end and source timestamps remain separate"},
          {"sources", sources},
          {"scene", json(*store_.get())},
          {"appliedRevision", applied_},
          {"history", history_},
          {"renderer", renderer_ ? renderer_->metrics() : Json::object()},
          {"preview", program_->metrics()},
          {"closed", closed_},
          {"pendingProgramFrames", program_pending_.size()},
          {"programQueueLimit", 6}};
}
Json Graph::source_metrics(uint64_t source) const {
  if (source == 3)
    return renderer_ ? renderer_->metrics() : Json::object();
  const auto &value = sources_.at(source);
  return value.pipeline ? value.pipeline->metrics() : value.final;
}
namespace {
class Tap final : public capture::Pipeline {
  std::shared_ptr<Graph> graph_;
  uint64_t source_, previous_{};
  bool closed_{};

public:
  Tap(std::shared_ptr<Graph> graph, uint64_t source)
      : graph_(std::move(graph)), source_(source) {}
  std::optional<capture::Frame> poll() override {
    return closed_ ? std::nullopt
                   : (source_ == 3 ? graph_->consume_program(previous_)
                                   : graph_->source(source_, previous_));
  }
  Json metrics() const override {
    auto value = graph_->source_metrics(source_);
    value["physicalCaptureOpens"] = 0;
    value["sourceId"] = source_;
    value["sharedSourceTap"] = true;
    return value;
  }
  void close() override { closed_ = true; }
};
} // namespace
std::unique_ptr<capture::Pipeline> tap(std::shared_ptr<Graph> graph,
                                       uint64_t source) {
  return std::make_unique<Tap>(std::move(graph), source);
}
} // namespace nss::scene
