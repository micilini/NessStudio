#pragma once
#include "capture/cpu_worker.hpp"
#include "scene.hpp"
#include <cstring>

namespace nss::scene::detail {
// Opaque BT709 sources can be composed in their existing NV12 color space.
// Alpha/other input colors use the premultiplied RGB path. Pixel centers and
// the immutable placement remain identical; Y and interleaved UV have separate
// bounded bilinear tables. No frame storage is retained by the worker.
inline bool nv12(const Snapshot &scene, std::span<const Input> sources,
                 std::span<std::byte> output,
                 capture::detail::CpuWorker &worker) {
  for (const auto &layer : scene.layers) {
    if (!layer.visible || layer.opacity == 0)
      continue;
    if (layer.opacity != 1)
      return false;
    const auto found =
        std::find_if(sources.begin(), sources.end(), [&](const auto &value) {
          return value.source == layer.source;
        });
    if (found == sources.end())
      continue;
    const auto &frame = found->frame;
    if (frame.format != capture::Frame::Format::Nv12 ||
        frame.color != capture::Frame::Color::Bt709 ||
        frame.alpha != capture::Frame::Alpha::Opaque ||
        frame.surface->cpu_pixels().empty())
      return false;
  }
  auto byte = [](double value) {
    return static_cast<uint8_t>((std::clamp)(value, 0.0, 255.0) + .5);
  };
  auto *destination = reinterpret_cast<uint8_t *>(output.data());
  const size_t chroma = size_t(scene.width) * scene.height;
  const double r = scene.background[0], g = scene.background[1],
               b = scene.background[2];
  std::fill_n(destination, chroma,
              byte(16 + .182586 * r + .614231 * g + .062007 * b));
  const auto u = byte(128 - .100644 * r - .338572 * g + .439216 * b),
             v = byte(128 + .439216 * r - .398942 * g - .040274 * b);
  for (size_t at = chroma; at < output.size(); at += 2) {
    destination[at] = u;
    destination[at + 1] = v;
  }
  struct Tap {
    uint32_t first{}, second{};
    double fraction{};
  };
  std::array<Tap, 1920> horizontal;
  std::array<Tap, 1080> vertical;
  for (const auto &layer : scene.layers) {
    if (!layer.visible || layer.opacity == 0)
      continue;
    const auto found =
        std::find_if(sources.begin(), sources.end(), [&](const auto &value) {
          return value.source == layer.source;
        });
    if (found == sources.end())
      continue;
    const auto &frame = found->frame;
    const auto memory = frame.surface->cpu_pixels();
    if (frame.stride < frame.width ||
        memory.size() < uint64_t(frame.stride) * frame.height * 3 / 2 ||
        (frame.width | frame.height) & 1)
      throw capture::Error(NSS_E_ARGUMENT, NSS_STAGE_PROCESS, 0,
                           "Invalid NV12 scene input planes");
    const auto placement =
        place(scene, layer, frame.width, frame.height, frame.rotation);
    const auto &rect = placement.destination;
    const auto *input = reinterpret_cast<const uint8_t *>(memory.data());
    for (uint32_t plane = 0; plane < 2; ++plane) {
      const uint32_t divisor = plane ? 2 : 1, channels = plane ? 2 : 1,
                     sw = frame.width / divisor, sh = frame.height / divisor,
                     dw = static_cast<uint32_t>(rect.width) / divisor,
                     dh = static_cast<uint32_t>(rect.height) / divisor;
      const auto *source =
          input + (plane ? size_t(frame.stride) * frame.height : 0);
      auto *target = destination + (plane ? chroma : 0) +
                     size_t(rect.y / divisor) * scene.width +
                     static_cast<uint32_t>(rect.x);
      if ((placement.rotation == 0 || placement.rotation == 180) &&
          placement.crop.x == 0 && placement.crop.y == 0 &&
          placement.crop.width == 1 && placement.crop.height == 1 && sw == dw &&
          sh == dh) {
        const bool reverse_x = (placement.rotation == 180) != placement.mirror,
                   reverse_y = placement.rotation == 180;
        for (uint32_t y = 0; y < dh; ++y) {
          const auto *row =
              source + size_t(reverse_y ? sh - 1 - y : y) * frame.stride;
          auto *output_row = target + size_t(y) * scene.width;
          if (!reverse_x)
            std::memcpy(output_row, row, size_t(dw) * channels);
          else
            for (uint32_t x = 0; x < dw; ++x)
              for (uint32_t channel = 0; channel < channels; ++channel)
                output_row[x * channels + channel] =
                    row[(sw - 1 - x) * channels + channel];
        }
        continue;
      }
      const bool rotated =
          placement.rotation == 90 || placement.rotation == 270;
      auto tap = [](double value, uint32_t count) {
        value = (std::clamp)(value, 0.0, double(count - 1));
        const auto first = static_cast<uint32_t>(value);
        return Tap{first, (std::min)(first + 1, count - 1), value - first};
      };
      for (uint32_t x = 0; x < dw; ++x) {
        auto uv = source_uv(placement, (double(x) + .5) / dw, .5);
        horizontal[x] =
            rotated ? tap(uv[1] * sh - .5, sh) : tap(uv[0] * sw - .5, sw);
      }
      for (uint32_t y = 0; y < dh; ++y) {
        auto uv = source_uv(placement, .5, (double(y) + .5) / dh);
        vertical[y] =
            rotated ? tap(uv[0] * sw - .5, sw) : tap(uv[1] * sh - .5, sh);
      }
      auto rows = [&](uint32_t begin, uint32_t end) {
        for (uint32_t y = begin; y < end; ++y)
          for (uint32_t x = 0; x < dw; ++x) {
            const auto &tx = rotated ? vertical[y] : horizontal[x];
            const auto &ty = rotated ? horizontal[x] : vertical[y];
            for (uint32_t channel = 0; channel < channels; ++channel) {
              const double a = source[size_t(ty.first) * frame.stride +
                                      tx.first * channels + channel],
                           b0 = source[size_t(ty.first) * frame.stride +
                                       tx.second * channels + channel],
                           c0 = source[size_t(ty.second) * frame.stride +
                                       tx.first * channels + channel],
                           d = source[size_t(ty.second) * frame.stride +
                                      tx.second * channels + channel];
              target[size_t(y) * scene.width + x * channels + channel] =
                  byte((a + (b0 - a) * tx.fraction) * (1 - ty.fraction) +
                       (c0 + (d - c0) * tx.fraction) * ty.fraction);
            }
          }
      };
      if (uint64_t(dw) * dh >= 640 * 480)
        worker.parallel([&] { rows(0, dh / 2); }, [&] { rows(dh / 2, dh); });
      else
        rows(0, dh);
    }
  }
  return true;
}
} // namespace nss::scene::detail
