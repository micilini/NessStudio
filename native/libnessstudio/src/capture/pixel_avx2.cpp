#include "pixel_avx2.hpp"
#include <algorithm>
#include <immintrin.h>

namespace nss::capture::detail {
namespace {
struct Rgb4 { __m256d r, g, b; };

Rgb4 unpack(__m128i bgra) {
  const auto mask = _mm_set1_epi32(255);
  return {_mm256_cvtepi32_pd(_mm_and_si128(_mm_srli_epi32(bgra, 16), mask)),
          _mm256_cvtepi32_pd(_mm_and_si128(_mm_srli_epi32(bgra, 8), mask)),
          _mm256_cvtepi32_pd(_mm_and_si128(bgra, mask))};
}
Rgb4 gather(BgraView source, uint32_t x0, uint32_t x1,
            uint32_t y0, uint32_t y1) {
  // The validated limits (stride <= 65536, height <= 16384) keep every
  // byte offset below INT32_MAX. Each gather reads exactly one BGRA pixel.
  const auto offsets = _mm_setr_epi32(
      static_cast<int>(y0 * source.stride + x0 * 4),
      static_cast<int>(y0 * source.stride + x1 * 4),
      static_cast<int>(y1 * source.stride + x0 * 4),
      static_cast<int>(y1 * source.stride + x1 * 4));
  return unpack(_mm_i32gather_epi32(reinterpret_cast<const int *>(source.data), offsets, 1));
}
Rgb4 identity_block(BgraView source, uint32_t x, uint32_t y) {
  const auto *top = source.data + size_t(y) * source.stride + size_t(x) * 4;
  const auto first = _mm_loadl_epi64(reinterpret_cast<const __m128i *>(top));
  const auto second = _mm_loadl_epi64(reinterpret_cast<const __m128i *>(top + source.stride));
  // Two complete BGRA pixels from each row, including unaligned rows. No
  // gather or wider speculative read is needed at the last image block.
  return unpack(_mm_unpacklo_epi64(first, second));
}
__m256d bilinear(__m256d a, __m256d b, __m256d c, __m256d d,
                 __m256d fx, __m256d fy) {
  // Preserve the scalar expression and operation order. /fp:strict keeps FMA
  // contraction/reassociation disabled, including at quantization boundaries.
  const auto upper = _mm256_add_pd(a, _mm256_mul_pd(_mm256_sub_pd(b, a), fx));
  const auto lower = _mm256_add_pd(c, _mm256_mul_pd(_mm256_sub_pd(d, c), fx));
  return _mm256_add_pd(_mm256_mul_pd(upper, _mm256_sub_pd(_mm256_set1_pd(1), fy)),
                       _mm256_mul_pd(lower, fy));
}
double average(__m256d values) {
  alignas(32) double part[4];
  _mm256_store_pd(part, _mm256_mul_pd(values, _mm256_set1_pd(.25)));
  // Match the scalar compositor's left-to-right 2x2 accumulation, rather
  // than a differently associated horizontal SIMD sum.
  return ((part[0] + part[1]) + part[2]) + part[3];
}
uint8_t quantize(double value) {
  return static_cast<uint8_t>(std::clamp(value, 0.0, 255.0) + .5);
}
void store(Rgb4 rgb, uint8_t *top, uint8_t *bottom, uint8_t *uv) {
  auto y = _mm256_add_pd(_mm256_set1_pd(16), _mm256_mul_pd(rgb.r, _mm256_set1_pd(.182586)));
  y = _mm256_add_pd(y, _mm256_mul_pd(rgb.g, _mm256_set1_pd(.614231)));
  y = _mm256_add_pd(y, _mm256_mul_pd(rgb.b, _mm256_set1_pd(.062007)));
  y = _mm256_min_pd(_mm256_max_pd(y, _mm256_setzero_pd()), _mm256_set1_pd(255));
  const auto bytes = _mm256_cvttpd_epi32(_mm256_add_pd(y, _mm256_set1_pd(.5)));
  alignas(16) int luma[4];
  _mm_store_si128(reinterpret_cast<__m128i *>(luma), bytes);
  top[0] = static_cast<uint8_t>(luma[0]);
  top[1] = static_cast<uint8_t>(luma[1]);
  bottom[0] = static_cast<uint8_t>(luma[2]);
  bottom[1] = static_cast<uint8_t>(luma[3]);
  const auto r = average(rgb.r), g = average(rgb.g), b = average(rgb.b);
  uv[0] = quantize(128 - .100644 * r - .338572 * g + .439216 * b);
  uv[1] = quantize(128 + .439216 * r - .398942 * g - .040274 * b);
}
} // namespace

void render_pixels_avx2(BgraView source, Rect crop, Rect viewport,
                        uint32_t width, uint32_t height, uint8_t *nv12,
                        Rect area, const ScaleTap *horizontal,
                        const ScaleTap *vertical) {
  const auto chroma = size_t(width) * height;
  for (int32_t y = area.y; y < area.y + area.height; y += 2) {
    for (int32_t x = area.x; x < area.x + area.width; x += 2) {
      Rgb4 rgb;
      if (!horizontal) {
        const auto sx = static_cast<uint32_t>(crop.x + x - viewport.x);
        const auto sy = static_cast<uint32_t>(crop.y + y - viewport.y);
        rgb = identity_block(source, sx, sy);
      } else {
        const auto &tx0 = horizontal[x - viewport.x], &tx1 = horizontal[x + 1 - viewport.x];
        const auto &ty0 = vertical[y - viewport.y], &ty1 = vertical[y + 1 - viewport.y];
        const auto a = gather(source, tx0.first, tx1.first, ty0.first, ty1.first);
        const auto b = gather(source, tx0.second, tx1.second, ty0.first, ty1.first);
        const auto c = gather(source, tx0.first, tx1.first, ty0.second, ty1.second);
        const auto d = gather(source, tx0.second, tx1.second, ty0.second, ty1.second);
        const auto fx = _mm256_setr_pd(tx0.fraction, tx1.fraction, tx0.fraction, tx1.fraction);
        const auto fy = _mm256_setr_pd(ty0.fraction, ty0.fraction, ty1.fraction, ty1.fraction);
        rgb = {bilinear(a.r, b.r, c.r, d.r, fx, fy),
               bilinear(a.g, b.g, c.g, d.g, fx, fy),
               bilinear(a.b, b.b, c.b, d.b, fx, fy)};
      }
      const auto offset = size_t(y) * width + static_cast<size_t>(x);
      store(rgb, nv12 + offset, nv12 + offset + width,
            nv12 + chroma + size_t(y / 2) * width + static_cast<size_t>(x));
    }
  }
  _mm256_zeroupper();
}
} // namespace nss::capture::detail
