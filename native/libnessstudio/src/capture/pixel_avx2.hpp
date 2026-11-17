#pragma once
#include "capture.hpp"

namespace nss::capture::detail {
struct ScaleTap {
  uint32_t first{}, second{};
  double fraction{};
};
// Caller validates source/output bounds, the even viewport/row range and ISA
// support before entering this separately compiled translation unit. Null tap
// arrays select identity scaling. Each iteration owns one complete 2x2 block.
void render_pixels_avx2(BgraView source, Rect crop, Rect viewport,
                        uint32_t width, uint32_t height, uint8_t *nv12,
                        Rect area, const ScaleTap *horizontal,
                        const ScaleTap *vertical);
} // namespace nss::capture::detail
