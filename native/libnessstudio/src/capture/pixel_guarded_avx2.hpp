#pragma once
#include "pixel_avx2.hpp"

namespace nss::capture::detail {
struct PixelPrecisionStats {
  uint64_t blocks{}, precise_blocks{};
};
// Same validated planes, even area and ISA preconditions as the double kernel.
// Eight float lanes are used only when their bounded rounding error cannot
// change a quantized byte. Ambiguous blocks use the original double kernel.
// Optional statistics select a separate instantiation; production has no
// per-block counters or shared mutable state.
void render_pixels_guarded_avx2(BgraView source, Rect crop, Rect viewport,
                                uint32_t width, uint32_t height, uint8_t *nv12,
                                Rect area, const ScaleTap *horizontal,
                                const ScaleTap *vertical,
                                PixelPrecisionStats *stats = nullptr);
} // namespace nss::capture::detail
