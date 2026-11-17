#pragma once
#include "capture/capture.hpp"
#include <array>
#include <functional>

namespace nss::scene {
using Json = nlohmann::json;
inline std::atomic<uint64_t> live_renderers{}, live_bytes{};
struct Box {
  double x{}, y{}, width{1}, height{1};
};
enum class Missing { HoldLast, Background, Fail };
enum class Selection { Wait, Frame, Background, Fail };
struct Layer {
  uint64_t source{};
  Box crop, destination;
  double opacity{1};
  uint32_t rotation{}, max_age_ms{500};
  bool visible{true}, mirror{}, cover{}, required{true};
  Missing missing{Missing::HoldLast};
};
struct Snapshot {
  uint64_t revision{1};
  uint32_t width{}, height{}, fps_num{30}, fps_den{1};
  std::array<uint8_t, 3>
      background{}; // R,G,B; output is opaque BT709 limited NV12.
  std::string layout;
  std::vector<Layer> layers;
};
Selection select(const Layer &, const std::optional<capture::Frame> &,
                 int64_t now, bool required);
Snapshot parse(const Json &value);
Json json(const Snapshot &value);
struct Placement {
  capture::Rect destination;
  Box crop;
  uint32_t rotation{};
  bool mirror{};
};
Placement place(const Snapshot &, const Layer &, uint32_t width,
                uint32_t height, uint32_t orientation = 0);
std::array<double, 2> source_uv(const Placement &, double x, double y);
class Store final {
  std::atomic<std::shared_ptr<const Snapshot>> current_;
  std::mutex gate_;
  std::array<bool, 3> allowed_{}, required_{};

public:
  explicit Store(Snapshot value);
  std::shared_ptr<const Snapshot> get() const;
  uint64_t update(const Json &value);
};
struct Input {
  uint64_t source{};
  capture::Frame frame;
};
class Renderer {
public:
  Renderer() { ++live_renderers; }
  virtual ~Renderer() { --live_renderers; }
  virtual std::optional<capture::Frame>
  render(const Snapshot &, std::span<const Input>,
         std::optional<int64_t> presentation = std::nullopt) = 0;
  virtual std::optional<capture::Frame> take_ready() { return {}; }
  virtual void close() = 0;
  virtual Json metrics() const = 0;
};
std::unique_ptr<Renderer> make_cpu(const Snapshot &);
enum class GpuKernel { Automatic, Raster, Compute };
std::unique_ptr<Renderer> make_gpu(const Snapshot &,
                                   const capture::Frame &device_reference,
                                   GpuKernel kernel = GpuKernel::Automatic);
// Converts one frame to premultiplied BGRA for interpolation; source storage is
// never retained.
void decode_bgra(const capture::Frame &, std::span<uint8_t> destination);
} // namespace nss::scene
