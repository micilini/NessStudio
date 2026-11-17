#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "capture/cpu_worker.hpp"
#include "nv12.hpp"
#include "scene.hpp"
#include <cmath>
#include <cstring>

namespace nss::scene {
namespace {
uint8_t byte(double value) {
  return static_cast<uint8_t>((std::clamp)(value, 0.0, 255.0) + .5);
}
struct CpuSurface final : capture::Surface {
  FramePool::Lease lease;
  explicit CpuSurface(FramePool::Lease value) : lease(std::move(value)) {}
  std::span<const std::byte> cpu_pixels() const override {
    return lease.view();
  }
};
std::atomic<uint64_t> generations{};
class Cpu final : public Renderer {
  capture::detail::CpuWorker worker_;
  uint64_t nv12_frames_{};
  FramePool pool_;
  std::vector<uint8_t> canvas_;
  struct Decoded {
    uint64_t id{}, generation{};
    uint32_t width{}, height{};
    std::vector<uint8_t> bytes;
  };
  std::array<Decoded, 3> decoded_;
  uint32_t width_, height_;
  uint64_t generation_{++generations}, frames_{}, pressure_{}, bytes_{},
      decoded_bytes_{}, revision_{};

public:
  explicit Cpu(const Snapshot &scene)
      : pool_(6, uint64_t(scene.width) * scene.height * 3 / 2),
        canvas_(size_t(scene.width) * scene.height * 4), width_(scene.width),
        height_(scene.height) {
    if (scene.width > 1280 || scene.height > 720)
      throw capture::Error(NSS_E_UNAVAILABLE, NSS_STAGE_PLAN, 0,
                           "CPU composition exceeds its 720p preset");
    bytes_ = canvas_.size();
    live_bytes += bytes_;
  }
  ~Cpu() override { live_bytes -= bytes_; }
  std::optional<capture::Frame>
  render(const Snapshot &scene, std::span<const Input> sources,
         std::optional<int64_t> presentation) override {
    if (scene.width != width_ || scene.height != height_)
      throw capture::Error(NSS_E_ARGUMENT, NSS_STAGE_PROCESS, 0,
                           "CPU canvas cannot resize while recording");
    auto output = pool_.acquire();
    if (!output) {
      ++pressure_;
      return {};
    }
    const auto begin = qpc_hns();
    auto finish = [&] {
      output->seal();
      revision_ = scene.revision;
      auto frame =
          capture::Frame{std::make_shared<CpuSurface>(std::move(*output)),
                         ++frames_,
                         generation_,
                         begin,
                         qpc_hns(),
                         begin,
                         width_,
                         height_,
                         width_,
                         true};
      frame.presentation_hns = presentation;
      return frame;
    };
    if (detail::nv12(scene, sources, output->writable(), worker_)) {
      ++nv12_frames_;
      return finish();
    }
    for (size_t pixel = 0; pixel < canvas_.size(); pixel += 4) {
      canvas_[pixel] = scene.background[2];
      canvas_[pixel + 1] = scene.background[1];
      canvas_[pixel + 2] = scene.background[0];
      canvas_[pixel + 3] = 255;
    }
    for (const auto &layer : scene.layers) {
      if (!layer.visible || layer.opacity == 0)
        continue;
      const auto found =
          std::find_if(sources.begin(), sources.end(), [&](const auto &source) {
            return source.source == layer.source;
          });
      if (found == sources.end())
        continue;
      const auto &frame = found->frame;
      auto &decoded = decoded_[layer.source];
      if (decoded.width != frame.width || decoded.height != frame.height) {
        std::vector<uint8_t> replacement(size_t(frame.width) * frame.height *
                                         4);
        live_bytes -= decoded.bytes.size();
        bytes_ -= decoded.bytes.size();
        decoded.bytes.swap(replacement);
        live_bytes += decoded.bytes.size();
        bytes_ += decoded.bytes.size();
        decoded.width = frame.width;
        decoded.height = frame.height;
        decoded.id = 0;
      }
      if (decoded.id != frame.id || decoded.generation != frame.generation) {
        decode_bgra(frame, decoded.bytes);
        decoded.id = frame.id;
        decoded.generation = frame.generation;
        decoded_bytes_ += decoded.bytes.size();
      }
      const auto placement =
          place(scene, layer, frame.width, frame.height, frame.rotation);
      const auto &dest = placement.destination;
      if (frame.alpha == capture::Frame::Alpha::Opaque && layer.opacity == 1 &&
          placement.rotation == 0 && !placement.mirror &&
          placement.crop.x == 0 && placement.crop.y == 0 &&
          placement.crop.width == 1 && placement.crop.height == 1 &&
          dest.width == static_cast<int32_t>(frame.width) &&
          dest.height == static_cast<int32_t>(frame.height)) {
        for (int32_t y = 0; y < dest.height; ++y)
          std::memcpy(canvas_.data() +
                          (size_t(dest.y + y) * width_ + dest.x) * 4,
                      decoded.bytes.data() + size_t(y) * frame.width * 4,
                      size_t(dest.width) * 4);
        continue;
      }
      const auto origin =
                     source_uv(placement, .5 / dest.width, .5 / dest.height),
                 horizontal =
                     source_uv(placement, 1.5 / dest.width, .5 / dest.height),
                 vertical =
                     source_uv(placement, .5 / dest.width, 1.5 / dest.height);
      for (int32_t y = 0; y < dest.height; ++y)
        for (int32_t x = 0; x < dest.width; ++x) {
          const std::array<double, 2> uv{
              origin[0] + x * (horizontal[0] - origin[0]) +
                  y * (vertical[0] - origin[0]),
              origin[1] + x * (horizontal[1] - origin[1]) +
                  y * (vertical[1] - origin[1])};
          const double sx = (std::clamp)(uv[0] * frame.width - .5, 0.0,
                                         double(frame.width - 1)),
                       sy = (std::clamp)(uv[1] * frame.height - .5, 0.0,
                                         double(frame.height - 1));
          const auto x0 = static_cast<uint32_t>(sx),
                     y0 = static_cast<uint32_t>(sy),
                     x1 = (std::min)(x0 + 1, frame.width - 1),
                     y1 = (std::min)(y0 + 1, frame.height - 1);
          const double fx = sx - x0, fy = sy - y0;
          const auto *a =
              decoded.bytes.data() + (size_t(y0) * frame.width + x0) * 4;
          const auto *b =
              decoded.bytes.data() + (size_t(y0) * frame.width + x1) * 4;
          const auto *c =
              decoded.bytes.data() + (size_t(y1) * frame.width + x0) * 4;
          const auto *d =
              decoded.bytes.data() + (size_t(y1) * frame.width + x1) * 4;
          std::array<double, 4> sample{};
          for (size_t channel = 0; channel < 4; ++channel)
            sample[channel] =
                (a[channel] * (1 - fx) + b[channel] * fx) * (1 - fy) +
                (c[channel] * (1 - fx) + d[channel] * fx) * fy;
          const auto opacity = sample[3] / 255 * layer.opacity;
          auto *target = canvas_.data() + (size_t(dest.y + y) * width_ +
                                           static_cast<uint32_t>(dest.x + x)) *
                                              4;
          for (size_t channel = 0; channel < 3; ++channel)
            target[channel] = byte(sample[channel] * layer.opacity +
                                   target[channel] * (1 - opacity));
        }
    }
    auto writable = output->writable();
    // Canvas is already at output resolution. Convert each 2x2 block directly,
    // avoiding a redundant identity bilinear pass through the capture scaler.
    auto *nv12 = reinterpret_cast<uint8_t *>(writable.data());
    auto convert = [&](uint32_t begin_row, uint32_t end_row) {
      for (uint32_t y = begin_row; y < end_row; y += 2)
        for (uint32_t x = 0; x < width_; x += 2) {
          double r{}, g{}, b{};
          for (uint32_t dy = 0; dy < 2; ++dy)
            for (uint32_t dx = 0; dx < 2; ++dx) {
              const auto *pixel =
                  canvas_.data() + (size_t(y + dy) * width_ + x + dx) * 4;
              nv12[size_t(y + dy) * width_ + x + dx] =
                  byte(16 + .182586 * pixel[2] + .614231 * pixel[1] +
                       .062007 * pixel[0]);
              r += pixel[2] * .25;
              g += pixel[1] * .25;
              b += pixel[0] * .25;
            }
          const auto at = size_t(width_) * height_ + size_t(y / 2) * width_ + x;
          nv12[at] = byte(128 - .100644 * r - .338572 * g + .439216 * b);
          nv12[at + 1] = byte(128 + .439216 * r - .398942 * g - .040274 * b);
        }
    };
    if (uint64_t(width_) * height_ >= 640 * 480) {
      const auto middle = (height_ / 4) * 2;
      worker_.parallel([&] { convert(0, middle); },
                       [&] { convert(middle, height_); });
    } else
      convert(0, height_);
    return finish();
  }
  void close() override {}
  Json metrics() const override {
    const auto pool = pool_.stats();
    return {{"implementationId", "CPU shared scene BGRA blend to BT709 NV12"},
            {"hardware", false},
            {"software", true},
            {"frames", frames_},
            {"nativeNv12CompositionFrames", nv12_frames_},
            {"appliedRevision", revision_},
            {"poolPressure", pressure_},
            {"poolSlots", 6},
            {"poolBytes", pool.bytes},
            {"scratchBytes", bytes_},
            {"decodedSourceBytes", decoded_bytes_},
            {"gpuToCpuReadbackBytes", 0},
            {"sourceCaptureOpens", 0},
            {"processingWorkerThreads", 1},
            {"parallelConversions", worker_.completed}};
  }
};
} // namespace
void decode_bgra(const capture::Frame &frame, std::span<uint8_t> destination) {
  const auto input = frame.surface->cpu_pixels();
  if (destination.size() != uint64_t(frame.width) * frame.height * 4 ||
      input.empty())
    throw capture::Error(NSS_E_ARGUMENT, NSS_STAGE_PROCESS, 0,
                         "CPU compositor requires bounded RAM input");
  if (frame.format == capture::Frame::Format::Bgra) {
    if (frame.stride < frame.width * 4 ||
        input.size() < uint64_t(frame.stride) * frame.height)
      throw capture::Error(NSS_E_ARGUMENT, NSS_STAGE_PROCESS, 0,
                           "Invalid scene BGRA stride/length");
    for (uint32_t y = 0; y < frame.height; ++y) {
      const auto *row = reinterpret_cast<const uint8_t *>(input.data()) +
                        size_t(y) * frame.stride;
      auto *out = destination.data() + size_t(y) * frame.width * 4;
      for (uint32_t x = 0; x < frame.width; ++x) {
        const auto alpha =
            frame.alpha == capture::Frame::Alpha::Opaque ? 255 : row[x * 4 + 3];
        out[x * 4 + 3] = static_cast<uint8_t>(alpha);
        for (uint32_t channel = 0; channel < 3; ++channel)
          out[x * 4 + channel] =
              frame.alpha == capture::Frame::Alpha::Straight
                  ? byte(double(row[x * 4 + channel]) * alpha / 255)
                  : row[x * 4 + channel];
      }
    }
    return;
  }
  if (frame.stride < frame.width ||
      input.size() < uint64_t(frame.stride) * frame.height * 3 / 2 ||
      (frame.width & 1) || (frame.height & 1))
    throw capture::Error(NSS_E_ARGUMENT, NSS_STAGE_PROCESS, 0,
                         "Invalid scene NV12 bounds");
  const auto *luma = reinterpret_cast<const uint8_t *>(input.data());
  const auto *chroma = luma + size_t(frame.stride) * frame.height;
  const bool bt601 = frame.color == capture::Frame::Color::Bt601;
  for (uint32_t y = 0; y < frame.height; ++y)
    for (uint32_t x = 0; x < frame.width; ++x) {
      const double
          yy = double(luma[size_t(y) * frame.stride + x]) - 16,
          u = double(chroma[size_t(y / 2) * frame.stride + (x & ~1U)]) - 128,
          v = double(chroma[size_t(y / 2) * frame.stride + (x & ~1U) + 1]) -
              128;
      auto *out = destination.data() + (size_t(y) * frame.width + x) * 4;
      out[0] = byte(1.164383 * yy + (bt601 ? 2.017232 : 2.112402) * u);
      out[1] = byte(1.164383 * yy - (bt601 ? .391762 : .213249) * u -
                    (bt601 ? .812968 : .532909) * v);
      out[2] = byte(1.164383 * yy + (bt601 ? 1.596027 : 1.792741) * v);
      out[3] = 255;
    }
}
std::unique_ptr<Renderer> make_cpu(const Snapshot &value) {
  return std::make_unique<Cpu>(value);
}
} // namespace nss::scene
