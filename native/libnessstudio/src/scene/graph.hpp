#pragma once
#include "camera/camera.hpp"
#include "preview/preview.hpp"
#include "runtime/session.hpp"
#include "scene.hpp"
#include <deque>

namespace nss::scene {
// One worker owns physical capture, shared immutable frame leases and
// rendering. Consumers may hold a lease; none owns or reopens a physical
// source.
class Graph final {
  struct Source {
    capture::Plan output;
    std::optional<camera::Plan> camera;
    std::unique_ptr<capture::Pipeline> pipeline;
    std::optional<capture::Frame> latest, eligible;
    std::shared_ptr<preview::Hub> preview = std::make_shared<preview::Hub>();
    Json final = Json::object();
    Json preparation = Json::object();
    uint64_t polls{}, opens{}, missing{}, background{}, held{};
    bool required{};
  };
  std::array<Source, 3> sources_;
  Store store_;
  std::unique_ptr<Renderer> renderer_;
  std::shared_ptr<preview::Hub> program_ = std::make_shared<preview::Hub>();
  std::optional<capture::Frame> program_frame_;
  std::deque<capture::Frame> program_pending_;
  bool queued_program_{};
  uint32_t preview_turn_{1};
  void completed(capture::Frame frame);
  SessionClock *clock_{};
  bool gpu_{}, closed_{}, cached_consumers_{};
  int64_t next_render_{}, last_poll_{};
  uint64_t applied_{};
  Json history_ = Json::array();

public:
  Graph(Snapshot snapshot, std::optional<capture::Plan> screen,
        std::optional<std::pair<camera::Plan, capture::Plan>> camera, bool gpu,
        bool raw, bool queued_program);
  void prepare(std::stop_token stop);
  void prepare_software(std::stop_token stop);
  void bind(uint64_t session);
  void start(SessionClock &clock);
  void resume();
  void poll();
  void present_one();
  void cached_consumers(bool value) { cached_consumers_ = value; }
  std::optional<capture::Frame> source(uint64_t id, uint64_t &previous);
  std::optional<capture::Frame> program(uint64_t &previous);
  std::optional<capture::Frame> consume_program(uint64_t &previous);
  std::shared_ptr<preview::Hub> preview(uint64_t id);
  uint64_t update(const Json &value) { return store_.update(value); }
  void close();
  Json metrics() const;
  Json source_metrics(uint64_t source) const;
  Json history() const { return history_; }
};
std::unique_ptr<capture::Pipeline> tap(std::shared_ptr<Graph>, uint64_t source);
} // namespace nss::scene
