#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "capture.hpp"
#include "cpu_worker.hpp"
#include "gdi_worker.hpp"
#include "gdi_cursor.hpp"
#include "pixel_avx2.hpp"
#include "pixel_guarded_avx2.hpp"
#include "scheduling.hpp"
#include "../cpu_features.hpp"
#include <array>
#include <cmath>
#include <windows.h>

namespace nss::capture {
namespace {
struct DpiScope {
  DPI_AWARENESS_CONTEXT previous =
      SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  ~DpiScope() {
    if (previous)
      SetThreadDpiAwarenessContext(previous);
  }
};
[[noreturn]] void gdi_error(const char *text) {
  throw Error(NSS_E_UNAVAILABLE, NSS_STAGE_CAPTURE,
              HRESULT_FROM_WIN32(GetLastError()), text);
}
struct Dib final : Surface {
  HDC dc{};
  HBITMAP bitmap{};
  HGDIOBJ previous{};
  void *bits{};
  uint32_t width{}, height{};
  uint64_t bytes{};
  Dib(HDC compatible, uint32_t w, uint32_t h)
      : width(w), height(h), bytes(uint64_t(w) * h * 4) {
    dc = CreateCompatibleDC(compatible);
    if (!dc)
      gdi_error("CreateCompatibleDC failed");
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = static_cast<LONG>(w);
    info.bmiHeader.biHeight = -static_cast<LONG>(h);
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    bitmap =
        CreateDIBSection(compatible, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bitmap) {
      DeleteDC(dc);
      dc = nullptr;
      gdi_error("CreateDIBSection failed");
    }
    previous = SelectObject(dc, bitmap);
    if (!previous || previous == HGDI_ERROR) {
      DeleteObject(bitmap);
      DeleteDC(dc);
      bitmap = nullptr;
      dc = nullptr;
      gdi_error("DIB selection failed");
    }
    live_capture_bytes += bytes;
  }
  ~Dib() override {
    if (dc && previous)
      SelectObject(dc, previous);
    if (bitmap)
      DeleteObject(bitmap);
    if (dc)
      DeleteDC(dc);
    live_capture_bytes -= bytes;
  }
  std::span<const std::byte> cpu_pixels() const override {
    return {static_cast<const std::byte *>(bits), static_cast<size_t>(bytes)};
  }
};
struct CpuSurface final : Surface {
  FramePool::Lease bytes;
  explicit CpuSurface(FramePool::Lease value) : bytes(std::move(value)) {}
  std::span<const std::byte> cpu_pixels() const override {
    return bytes.view();
  }
};
// Recording retains the original DIB until its CFR window selects it. All
// copies of the frame share one conversion, then release that DIB immediately.
// Only the pipeline owner prepares it; readers receive sealed NV12 afterwards.
struct SelectedCpuSurface final : Surface {
  const std::shared_ptr<const std::byte> owner;
  mutable std::optional<GdiSource::Image> image;
  mutable std::shared_ptr<const CpuSurface> converted;
  SelectedCpuSurface(std::shared_ptr<const std::byte> pipeline,
                     GdiSource::Image value)
      : owner(std::move(pipeline)), image(std::move(value)) {}
  std::span<const std::byte> cpu_pixels() const override {
    return converted ? converted->cpu_pixels() : std::span<const std::byte>{};
  }
};
using Rgb = std::array<double, 3>;
Rgb sample(BgraView image, double x, double y, bool mirror = false) {
  x = std::clamp(x, 0.0, double(image.width - 1));
  y = std::clamp(y, 0.0, double(image.height - 1));
  if (mirror)
    x = double(image.width - 1) - x;
  const auto x0 = static_cast<uint32_t>(x), y0 = static_cast<uint32_t>(y);
  const auto x1 = (std::min)(x0 + 1, image.width - 1),
             y1 = (std::min)(y0 + 1, image.height - 1);
  const auto fx = x - x0, fy = y - y0;
  Rgb result{};
  for (size_t channel = 0; channel < 3; ++channel) {
    const auto offset = 2 - channel;
    const auto
        a = image.data[size_t(y0) * image.stride + size_t(x0) * 4 + offset],
        b = image.data[size_t(y0) * image.stride + size_t(x1) * 4 + offset],
        c = image.data[size_t(y1) * image.stride + size_t(x0) * 4 + offset],
        d = image.data[size_t(y1) * image.stride + size_t(x1) * 4 + offset];
    result[channel] = (a + (b - a) * fx) * (1 - fy) + (c + (d - c) * fx) * fy;
  }
  return result;
}
uint8_t byte(double value) {
  // Bounded half-up quantization avoids a CRT call for every output component.
  return static_cast<uint8_t>(std::clamp(value, 0.0, 255.0) + 0.5);
}
std::array<double, 4> premultiplied(BgraView image, double x, double y,
                                    bool mirror) {
  x = std::clamp(x, 0.0, double(image.width - 1));
  y = std::clamp(y, 0.0, double(image.height - 1));
  if (mirror)
    x = double(image.width - 1) - x;
  const auto x0 = static_cast<uint32_t>(x), y0 = static_cast<uint32_t>(y),
             x1 = (std::min)(x0 + 1, image.width - 1),
             y1 = (std::min)(y0 + 1, image.height - 1);
  const auto fx = x - x0, fy = y - y0;
  std::array<double, 4> result{};
  for (int i = 0; i < 4; ++i) {
    const auto px = (i & 1) ? x1 : x0, py = (i & 2) ? y1 : y0;
    const auto weight = ((i & 1) ? fx : 1 - fx) * ((i & 2) ? fy : 1 - fy);
    const auto *pixel = image.data + size_t(py) * image.stride + px * 4;
    const auto alpha = pixel[3] / 255.0;
    result[3] += alpha * weight;
    for (size_t c = 0; c < 3; ++c)
      result[c] += pixel[2 - c] * alpha * weight;
  }
  return result;
}
} // namespace
struct GdiSource::Impl {
  detail::CaptureTask scheduling;
  Plan config;
  HDC screen{};
  DWORD owner = GetCurrentThreadId();
  std::vector<std::shared_ptr<Dib>> slots;
  uint64_t frames{}, pressure{}, flushes{};
  uint64_t cursor_drawn{}, cursor_omitted{}, cursor_requeries{}, cursor_restores{};
  DWORD cursor_last_error{};
  detail::CursorStage cursor_last_failure = detail::CursorStage::None;
  int64_t last_health{},capture_total{},capture_max{},health_total{},health_max{},previous_end{},gap_max{};
  uint64_t health_checks{};
  explicit Impl(const Plan &value) : config(value) {
    DpiScope scope;
    screen = GetDC(nullptr);
    if (!screen)
      gdi_error("Desktop DC unavailable");
    try {
      for (uint32_t i = 0; i < config.slots; ++i)
        slots.push_back(std::make_shared<Dib>(
            screen, static_cast<uint32_t>(config.crop.width),
            static_cast<uint32_t>(config.crop.height)));
    } catch (...) {
      ReleaseDC(nullptr, screen);
      screen = nullptr;
      throw;
    }
  }
  ~Impl() {
    slots.clear();
    if (screen)
      ReleaseDC(nullptr, screen);
  }
};
GdiSource::GdiSource(const Plan &value)
    : impl_(std::make_unique<Impl>(value)) {}
GdiSource::~GdiSource() = default;
std::optional<GdiSource::Image> GdiSource::acquire() {
  auto &p = *impl_;
  if (GetCurrentThreadId() != p.owner)
    throw Error(NSS_E_STATE, NSS_STAGE_CAPTURE, 0,
                "GDI acquisition must use its owner thread");
  DpiScope scope;
  const auto now = qpc_hns();
  if (now - p.last_health >= hns_per_second) {
    require_interactive_desktop();
    const auto list = displays();
    const auto found =
        std::find_if(list.begin(), list.end(), [&](const Display &d) {
          return d.id == p.config.display.id;
        });
    if (found == list.end() || !found->color_known || found->hdr ||
        found->bounds.width != p.config.display.bounds.width ||
        found->bounds.height != p.config.display.bounds.height ||
        found->bounds.x != p.config.display.bounds.x ||
        found->bounds.y != p.config.display.bounds.y)
      throw Error(NSS_E_UNAVAILABLE, NSS_STAGE_CAPTURE, 0,
                  "Desktop geometry/color changed; stop and prepare a new "
                  "capture generation");
    const auto elapsed=qpc_hns()-now;
    p.health_total+=elapsed;
    p.health_max=(std::max)(p.health_max,elapsed);
    ++p.health_checks;
    p.last_health = now;
  }
  const auto slot =
      std::find_if(p.slots.begin(), p.slots.end(),
                   [](const auto &value) { return value.use_count() == 1; });
  if (slot == p.slots.end()) {
    ++p.pressure;
    return {};
  }
  const auto &image = *slot;
  const auto begin = qpc_hns();
  if(p.previous_end)p.gap_max=(std::max)(p.gap_max,begin-p.previous_end);
  const auto x = p.config.display.bounds.x + p.config.crop.x,
             y = p.config.display.bounds.y + p.config.crop.y;
  if (!BitBlt(image->dc, 0, 0, p.config.crop.width, p.config.crop.height,
              p.screen, x, y, SRCCOPY | CAPTUREBLT))
    gdi_error("Desktop BitBlt failed");
  if (p.config.cursor) {
    detail::SystemCursorApi api;
    const auto cursor = detail::draw_cursor_snapshot(api, image->dc, x, y);
    p.cursor_requeries += cursor.requeries;
    p.cursor_drawn += cursor.drawn;
    if (cursor.failure != detail::CursorStage::None) {
      ++p.cursor_omitted;
      p.cursor_last_error = cursor.error;
      p.cursor_last_failure = cursor.failure;
      // A failed draw may have touched the DIB. Restore real desktop pixels
      // before publishing the frame; failure of capture itself remains fatal.
      if (cursor.restore_background) {
        if (!BitBlt(image->dc, 0, 0, p.config.crop.width, p.config.crop.height,
                    p.screen, x, y, SRCCOPY | CAPTUREBLT))
          gdi_error("Desktop restore after cursor failure failed");
        ++p.cursor_restores;
      }
    }
  }
  if (!GdiFlush())
    gdi_error("GdiFlush failed before CPU access");
  ++p.flushes;
  const auto end = qpc_hns();
  p.previous_end=end;
  p.capture_total+=end-begin;
  p.capture_max=(std::max)(p.capture_max,end-begin);
  auto *pixels = static_cast<uint8_t *>(image->bits);
  // Deferred screen frames expose no raw BGRA plane. Their only pixel consumer
  // is the opaque RGB-to-NV12 converter, which ignores alpha. Avoid writing the
  // whole DIB solely to materialize a channel that will never be consumed.
  // Generic acquisitions retain the normalized BGRA contract.
  if (!p.config.defer_conversion)
    for (uint64_t i = 3; i < image->bytes; i += 4)
      pixels[i] = 255;
  return Image{
      image,
      {pixels, image->width, image->height, image->width * 4, image->bytes},
      begin,
      end,
      ++p.frames};
}
GdiSource::Statistics GdiSource::statistics() const noexcept {
  const auto &p = *impl_;
  Statistics result;
  result.frames = p.frames;
  result.pressure = p.pressure;
  result.flushes = p.flushes;
  result.health_checks = p.health_checks;
  result.cursor_drawn = p.cursor_drawn;
  result.cursor_omitted = p.cursor_omitted;
  result.cursor_requeries = p.cursor_requeries;
  result.cursor_restores = p.cursor_restores;
  result.capture_total = p.capture_total;
  result.capture_max = p.capture_max;
  result.health_total = p.health_total;
  result.health_max = p.health_max;
  result.gap_max = p.gap_max;
  result.owner = p.owner;
  result.dib_slots = static_cast<uint32_t>(p.slots.size());
  result.mmcss_error = p.scheduling.error();
  result.cursor_last_error = p.cursor_last_error;
  result.opaque_alpha = !p.config.defer_conversion;
  result.mmcss_registered = p.scheduling.registered();
  result.cursor_last_failure = detail::cursor_stage_name(p.cursor_last_failure);
  return result;
}
Json GdiSource::describe(const Statistics &value) {
  return {{"backend", "gdi-dib-cpu"},
          {"acquisitionOwnerThread", value.owner},
          {"opaqueAlphaMaterialized", value.opaque_alpha},
          {"acquisitionMmcssRegistered", value.mmcss_registered},
          {"acquisitionMmcssError", value.mmcss_error},
          {"acquisitionMeanMs", value.frames ? double(value.capture_total) / value.frames / 10000 : 0.0},
          {"acquisitionMaxMs", double(value.capture_max) / 10000},
          {"healthChecks", value.health_checks},
          {"healthMeanMs", value.health_checks ? double(value.health_total) / value.health_checks / 10000 : 0.0},
          {"healthMaxMs", double(value.health_max) / 10000},
          {"acquisitionGapMaxMs", double(value.gap_max) / 10000},
          {"frames", value.frames},
          {"poolPressureDrops", value.pressure},
          {"gdiFlushes", value.flushes},
          {"dibSlots", value.dib_slots},
          {"estimatedTimestamp", true},
          {"timestampMethod", "QPC midpoint; acquisition interval retained"},
          {"cursorCompositions", "at most one DrawIconEx per BitBlt"},
          {"cursorImageOwnership", "per-acquisition owned CopyIcon snapshot"},
          {"cursorDrawn", value.cursor_drawn},
          {"cursorOmissions", value.cursor_omitted},
          {"cursorRequeries", value.cursor_requeries},
          {"cursorBackgroundRestores", value.cursor_restores},
          {"cursorLastError", value.cursor_last_error},
          {"cursorLastFailure", value.cursor_last_failure},
          {"d3dDeviceCreations", 0},
          {"wgcInitializations", 0}};
}
Json GdiSource::metrics() const { return describe(statistics()); }
static void process_cpu_impl(BgraView source, Rect crop, Rect viewport,
                             uint32_t width, uint32_t height,
                             std::span<uint8_t> nv12,
                             std::span<const Layer> layers,
                             detail::CpuWorker *worker,
                             CpuPixelKernel kernel = CpuPixelKernel::Automatic) {
  if (!source.data || !source.width || !source.height ||
      source.stride < uint64_t(source.width) * 4 || source.width > 16384 ||
      source.height > 16384 || source.stride > 65536 ||
      source.bytes < uint64_t(source.height - 1) * source.stride +
                         uint64_t(source.width) * 4)
    throw Error(NSS_E_ARGUMENT, NSS_STAGE_PROCESS, 0,
                "Invalid source BGRA planes/stride");
  (void)local_crop({0, 0, static_cast<int32_t>(source.width),
                    static_cast<int32_t>(source.height)},
                   crop, false);
  if ((width | height) & 1 || !width || !height || width > 4096 ||
      height > 4096 || nv12.size() < uint64_t(width) * height * 3 / 2 ||
      viewport.x < 0 || viewport.y < 0 || viewport.width <= 0 ||
      viewport.height <= 0 || viewport.x + int64_t(viewport.width) > width ||
      viewport.y + int64_t(viewport.height) > height ||
      ((viewport.x | viewport.y | viewport.width | viewport.height) & 1))
    throw Error(NSS_E_ARGUMENT, NSS_STAGE_PROCESS, 0,
                "Invalid NV12 destination/viewport");
  if (layers.size() > 8)
    throw Error(NSS_E_ARGUMENT, NSS_STAGE_PROCESS, 0,
                "CPU scene exceeds its layer budget");
  const bool avx2 = nss::cpu::has_avx2(nss::cpu::capabilities());
  if ((kernel == CpuPixelKernel::Avx2 || kernel == CpuPixelKernel::Avx2Double) && !avx2)
    throw Error(NSS_E_UNAVAILABLE, NSS_STAGE_PROCESS, 0,
                "AVX2 pixel processing requires CPU support and enabled XMM/YMM OS state");
  if (kernel != CpuPixelKernel::Automatic && kernel != CpuPixelKernel::Scalar &&
      kernel != CpuPixelKernel::Avx2 && kernel != CpuPixelKernel::Avx2Double)
    throw Error(NSS_E_ARGUMENT, NSS_STAGE_PROCESS, 0, "Unknown CPU pixel kernel");
  for (const auto &layer : layers)
    if (!layer.image.data || !layer.image.width || !layer.image.height ||
        layer.image.width > 16384 || layer.image.height > 16384 ||
        layer.image.stride < uint64_t(layer.image.width) * 4 ||
        layer.image.stride > 65536 ||
        layer.image.bytes <
            uint64_t(layer.image.height - 1) * layer.image.stride +
                uint64_t(layer.image.width) * 4 ||
        layer.destination.x < 0 || layer.destination.y < 0 ||
        layer.destination.width <= 0 || layer.destination.height <= 0 ||
        int64_t(layer.destination.x) + layer.destination.width > width ||
        int64_t(layer.destination.y) + layer.destination.height > height)
      throw Error(NSS_E_ARGUMENT, NSS_STAGE_PROCESS, 0,
                  "Invalid CPU scene layer");
  const auto chroma = size_t(width) * height;
  std::fill_n(nv12.data(), chroma, uint8_t{16});
  std::fill_n(nv12.data() + chroma, chroma / 2, uint8_t{128});
  auto pixel = [&](int32_t x, int32_t y) {
    const auto sx = crop.x +
                    (double(x - viewport.x) + 0.5) * crop.width /
                        viewport.width -
                    0.5,
               sy = crop.y +
                    (double(y - viewport.y) + 0.5) * crop.height /
                        viewport.height -
                    0.5;
    // Clamp to the selected crop, not the surrounding monitor pixels.
    Rgb color{};
    if (x >= viewport.x && y >= viewport.y && x < viewport.x + viewport.width &&
        y < viewport.y + viewport.height)
      color = sample(
          source,
          std::clamp(sx, double(crop.x), double(crop.x + crop.width - 1)),
          std::clamp(sy, double(crop.y), double(crop.y + crop.height - 1)));
    for (const auto &layer : layers) {
      const auto &dst = layer.destination;
      if (x < dst.x || y < dst.y || int64_t(x) >= int64_t(dst.x) + dst.width ||
          int64_t(y) >= int64_t(dst.y) + dst.height)
        continue;
      const auto lx =
                     (double(x - dst.x) + 0.5) * layer.image.width / dst.width -
                     0.5,
                 ly = (double(y - dst.y) + 0.5) * layer.image.height /
                          dst.height -
                      0.5;
      const auto overlay = premultiplied(layer.image, lx, ly, layer.mirror);
      const auto opacity = layer.opacity / 255.0;
      const auto alpha = overlay[3] * opacity;
      for (size_t channel = 0; channel < 3; ++channel)
        color[channel] =
            overlay[channel] * opacity + color[channel] * (1 - alpha);
    }
    return color;
  };
  auto render = [&](const auto &get_pixel, Rect area) {
    for (int32_t y = area.y; y < area.y + area.height; y += 2)
      for (int32_t x = area.x; x < area.x + area.width; x += 2) {
        Rgb sum{};
        for (int32_t dy = 0; dy < 2; ++dy)
          for (int32_t dx = 0; dx < 2; ++dx) {
            const auto rgb = get_pixel(x + dx, y + dy);
            nv12[size_t(y + dy) * width + static_cast<size_t>(x + dx)] = byte(
                16 + 0.182586 * rgb[0] + 0.614231 * rgb[1] + 0.062007 * rgb[2]);
            for (size_t c = 0; c < 3; ++c)
              sum[c] += rgb[c] * 0.25;
          }
        const auto at = chroma + size_t(y / 2) * width + static_cast<size_t>(x);
        nv12[at] = byte(128 - 0.100644 * sum[0] - 0.338572 * sum[1] +
                        0.439216 * sum[2]);
        nv12[at + 1] = byte(128 + 0.439216 * sum[0] - 0.398942 * sum[1] -
                            0.040274 * sum[2]);
      }
  };
  if (layers.empty()) {
    const bool vector_pixels = avx2 && kernel != CpuPixelKernel::Scalar;
    const auto render_vector = [&](const detail::ScaleTap *horizontal,
                                    const detail::ScaleTap *vertical) {
      const auto draw = [&](Rect area) {
        if (kernel == CpuPixelKernel::Avx2Double)
          detail::render_pixels_avx2(source, crop, viewport, width, height,
                                     nv12.data(), area, horizontal, vertical);
        else
          detail::render_pixels_guarded_avx2(source, crop, viewport, width, height,
                                             nv12.data(), area, horizontal, vertical);
      };
      if (worker && uint64_t(viewport.width) * viewport.height >= 200000 &&
          viewport.height >= 4) {
        const auto split = (viewport.height / 4) * 2;
        worker->parallel([&] { draw({viewport.x, viewport.y, viewport.width, split}); },
                           [&] { draw({viewport.x, viewport.y + split, viewport.width,
                                       viewport.height - split}); });
      } else draw(viewport);
    };
    auto render_viewport = [&](const auto &get_pixel) {
      if (worker && uint64_t(viewport.width) * viewport.height >= 200000 &&
          viewport.height >= 4) {
        const auto split = (viewport.height / 4) * 2;
        const Rect top{viewport.x, viewport.y, viewport.width, split},
            bottom{viewport.x, viewport.y + split, viewport.width,
                   viewport.height - split};
        worker->parallel([&] { render(get_pixel, top); },
                         [&] { render(get_pixel, bottom); });
      } else {
        render(get_pixel, viewport);
      }
    };
    if (crop.width == viewport.width && crop.height == viewport.height) {
      if (vector_pixels) {
        render_vector(nullptr, nullptr);
        return;
      }
      // An identity scale selects the exact source pixel. Keep the same
      // conversion/rounding and omit four bilinear reads and tap tables.
      render_viewport([&](int32_t x, int32_t y) {
        const auto *p = source.data + size_t(crop.y + y - viewport.y) * source.stride +
                        size_t(crop.x + x - viewport.x) * 4;
        return Rgb{double(p[2]), double(p[1]), double(p[0])};
      });
      return;
    }
    // The screen-only profile does not need scene traversal or four floating
    // divisions and repeated clamp/bounds calculations for every pixel.
    // These bounded tables preserve the same pixel-center bilinear filter.
    std::array<detail::ScaleTap, 4096> horizontal{}, vertical{};
    auto tap = [](int32_t index, int32_t origin, int32_t count,
                  int32_t output) {
      const auto value =
          std::clamp(origin + (double(index) + .5) * count / output - .5,
                     double(origin), double(origin + count - 1));
      const auto first = static_cast<uint32_t>(value);
      return detail::ScaleTap{
          first,
          (std::min)(first + 1, static_cast<uint32_t>(origin + count - 1)),
          value - first};
    };
    for (int32_t x = 0; x < viewport.width; ++x)
      horizontal[static_cast<size_t>(x)] =
          tap(x, crop.x, crop.width, viewport.width);
    for (int32_t y = 0; y < viewport.height; ++y)
      vertical[static_cast<size_t>(y)] =
          tap(y, crop.y, crop.height, viewport.height);
    if (vector_pixels) {
      render_vector(horizontal.data(), vertical.data());
      return;
    }
    const auto fast_pixel = [&](int32_t x, int32_t y) {
      const auto &tx = horizontal[static_cast<size_t>(x - viewport.x)],
                 &ty = vertical[static_cast<size_t>(y - viewport.y)];
      const auto *a = source.data + size_t(ty.first) * source.stride +
                      size_t(tx.first) * 4,
                 *b = source.data + size_t(ty.first) * source.stride +
                      size_t(tx.second) * 4,
                 *c = source.data + size_t(ty.second) * source.stride +
                      size_t(tx.first) * 4,
                 *d = source.data + size_t(ty.second) * source.stride +
                      size_t(tx.second) * 4;
      Rgb rgb{};
      for (size_t channel = 0; channel < 3; ++channel) {
        const auto offset = 2 - channel;
        rgb[channel] =
            (a[offset] + (b[offset] - a[offset]) * tx.fraction) *
                (1 - ty.fraction) +
            (c[offset] + (d[offset] - c[offset]) * tx.fraction) * ty.fraction;
      }
      return rgb;
    };
    render_viewport(fast_pixel);
  } else {
    render(pixel,
           {0, 0, static_cast<int32_t>(width), static_cast<int32_t>(height)});
  }
}
void process_cpu(BgraView source, Rect crop, Rect viewport, uint32_t width,
                 uint32_t height, std::span<uint8_t> nv12,
                 std::span<const Layer> layers, CpuPixelKernel kernel) {
  process_cpu_impl(source, crop, viewport, width, height, nv12, layers,
                   nullptr, kernel);
}
namespace {
class CpuPipeline final : public Pipeline {
  Plan plan_;
  detail::GdiWorker source_;
  FramePool output_;
  detail::CpuWorker worker_;
  const std::shared_ptr<const std::byte> identity_ = std::make_shared<const std::byte>();
  uint64_t delivered_{}, converted_{}, pressure_{}, conversion_pressure_{};
  DWORD owner_ = GetCurrentThreadId();
  bool closed_{};

public:
  explicit CpuPipeline(const Plan &value)
      : plan_(value), source_(value),
        output_(value.slots,
                FramePool::nv12_bytes(value.width, value.height, value.width)) {
  }
  WaitHint wait_hint() const noexcept override {
    // CPU conversion is synchronous. Wait for an actual acquisition while
    // retaining the recording owner's existing bounded CFR/control timer.
    return closed_ ? WaitHint{} : WaitHint{source_.wake_event(), false, nullptr};
  }
  bool prepare_frame(Frame &frame) override {
    if (!plan_.defer_conversion)
      return true;
    if (GetCurrentThreadId() != owner_)
      throw Error(NSS_E_STATE, NSS_STAGE_PROCESS, 0,
                  "CPU frame preparation must use its owner thread");
    const auto *selected = dynamic_cast<const SelectedCpuSurface *>(frame.surface.get());
    if (!selected || selected->owner != identity_)
      throw Error(NSS_E_ARGUMENT, NSS_STAGE_PROCESS, 0,
                  "Selected conversion requires a frame owned by this CPU pipeline");
    if (!selected->converted) {
      if (closed_)
        throw Error(NSS_E_STATE, NSS_STAGE_PROCESS, 0,
                    "Cannot start conversion after CPU capture closes");
      auto destination = output_.acquire();
      if (!destination) {
        ++conversion_pressure_;
        return false;
      }
      const auto writable = destination->writable();
      process_cpu_impl(
          selected->image->pixels, {0, 0, plan_.crop.width, plan_.crop.height},
          plan_.viewport, plan_.width, plan_.height,
          {reinterpret_cast<uint8_t *>(writable.data()), writable.size()}, {},
          &worker_);
      destination->seal();
      selected->converted = std::make_shared<CpuSurface>(std::move(*destination));
      selected->image.reset();
      ++converted_;
    }
    frame.format = Frame::Format::Nv12;
    frame.width = plan_.width;
    frame.height = plan_.height;
    frame.stride = plan_.width;
    return true;
  }
  std::optional<Frame> poll() override {
    if (GetCurrentThreadId() != owner_ || closed_)
      throw Error(NSS_E_STATE, NSS_STAGE_CAPTURE, 0,
                  "CPU capture is closed or used from a foreign thread");
    auto destination = plan_.defer_conversion ? std::optional<FramePool::Lease>{} : output_.acquire();
    if (!plan_.defer_conversion && !destination) {
      ++pressure_;
      return {};
    }
    auto image = source_.acquire();
    if (!image)
      return {};
    if (plan_.defer_conversion) {
      Frame frame{std::make_shared<SelectedCpuSurface>(identity_, *image),
                  image->id, 1, image->begin, image->end,
                  image->begin + (image->end - image->begin) / 2,
                  image->pixels.width, image->pixels.height, image->pixels.stride,
                  true, Frame::Format::Bgra};
      frame.acquisition_interval_estimate = true;
      ++delivered_;
      return frame;
    }
    const auto writable = destination->writable();
    process_cpu_impl(
        image->pixels, {0, 0, plan_.crop.width, plan_.crop.height},
        plan_.viewport, plan_.width, plan_.height,
        {reinterpret_cast<uint8_t *>(writable.data()), writable.size()}, {},
        &worker_);
    destination->seal();
    ++delivered_;
    ++converted_;
    Frame frame{std::make_shared<CpuSurface>(std::move(*destination)),
                 image->id,
                 1,
                 image->begin,
                 image->end,
                 image->begin + (image->end - image->begin) / 2,
                 plan_.width,
                 plan_.height,
                 plan_.width,
                 true};
    frame.acquisition_interval_estimate = true;
    return frame;
  }
  Json metrics() const override {
    auto report = source_.metrics();
    report["processor"] = "bounded-parallel-bilinear-BT709-limited-NV12";
    report["pixelKernel"] = nss::cpu::has_avx2(nss::cpu::capabilities()) ? "avx2" : "scalar";
    report["auxiliaryCpuThreadsMax"] = 1;
    report["processingOwnerThread"] = owner_;
    report["auxiliaryCpuThread"] = worker_.thread_id();
    report["parallelCpuJobsCompleted"] = worker_.completed;
    report["delivered"] = delivered_;
    report["converted"] = converted_;
    report["conversionPolicy"] = plan_.defer_conversion
        ? "selected-output-or-preview" : "every-acquired-frame";
    report["outputPoolPressureDrops"] = pressure_;
    report["selectedConversionPressureRetries"] = conversion_pressure_;
    const auto stats = output_.stats();
    report["outputPool"] = {{"bytes", stats.bytes},
                            {"inUse", stats.in_use},
                            {"peak", stats.peak},
                            {"slots", plan_.slots}};
    report["recordingReadbackBytes"] = 0;
    report["diagnosticReadbackBytes"] = 0;
    report["gpuCopies"] = 0;
    report["ramOutputBytes"] =
        converted_ * uint64_t(plan_.width) * plan_.height * 3 / 2;
    return report;
  }
  void close() override {
    source_.close();
    closed_ = true;
  }
};
} // namespace
std::unique_ptr<Pipeline> make_cpu(const Plan &value) {
  if (value.gpu)
    throw Error(NSS_E_ARGUMENT, NSS_STAGE_PLAN, 0,
                "CPU factory requires an explicit FullSoftware plan");
  return std::make_unique<CpuPipeline>(value);
}
} // namespace nss::capture
