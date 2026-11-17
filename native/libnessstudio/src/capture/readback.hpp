#pragma once
#include "capture.hpp"

namespace nss::capture {
// Explicit GPU-processing + software-encoder bridge. A single staged copy is
// outstanding; input/output ownership and acquisition timestamps stay intact.
class Readback {
  struct Impl;
  std::unique_ptr<Impl> impl_;
public:
  struct QueuedPreparation { bool ready{}, submitted{}; };
  struct PackedPreparation { bool supported{}, ready{}, submitted{}; };
  // Synchronous producer callback. It queues writes to the supplied RGBA8
  // buffer view on the validated immediate context. Rows contain NV12 bytes
  // with stride rounded up to four; chroma follows all luma rows.
  struct PackedWriter { void *state; void (*write)(void *state, void *view); };
  explicit Readback(bool timing_diagnostic = false, bool prefer_query_completion = false);
  ~Readback();
  bool prepare(Frame &frame);
  // The producer queued all writes on this immediate context and retains the
  // texture through the frame's surface lease. A submitted copy must be
  // flushed by the producer before waiting; this does not publish GPU readiness.
  QueuedPreparation prepare_queued(Frame &frame, void *texture, void *immediate_context);
  PackedPreparation prepare_packed_queued(Frame &frame, void *immediate_context, PackedWriter writer);
  bool pending() const noexcept;
  bool quiesce();
  void *completion_event() const noexcept;
  Json metrics() const;
};
}
