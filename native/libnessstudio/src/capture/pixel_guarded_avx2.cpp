#include "pixel_guarded_avx2.hpp"
#include <immintrin.h>

namespace nss::capture::detail {
namespace {
struct Rgb8 { __m256 r, g, b; };
template<int Shift> __m256 component(__m256i pixels) {
  return _mm256_cvtepi32_ps(_mm256_and_si256(_mm256_srli_epi32(pixels, Shift), _mm256_set1_epi32(255)));
}
__m256i gather(BgraView source, const ScaleTap* x, const ScaleTap* y, bool right, bool lower) {
  const auto xa = [&](int index) { return right ? x[index].second : x[index].first; };
  const auto ya = [&](int index) { return lower ? y[index].second : y[index].first; };
  const auto offset = [&](int column, int row) { return static_cast<int>(ya(row) * source.stride + xa(column) * 4); };
  const auto offsets = _mm256_setr_epi32(offset(0,0),offset(1,0),offset(0,1),offset(1,1),
                                         offset(2,0),offset(3,0),offset(2,1),offset(3,1));
  return _mm256_i32gather_epi32(reinterpret_cast<const int*>(source.data), offsets, 1);
}
__m256i identity(BgraView source, uint32_t x, uint32_t y) {
  const auto* row = source.data + size_t(y) * source.stride + size_t(x) * 4;
  const auto top = _mm_loadu_si128(reinterpret_cast<const __m128i*>(row));
  const auto bottom = _mm_loadu_si128(reinterpret_cast<const __m128i*>(row + source.stride));
  return _mm256_inserti128_si256(_mm256_castsi128_si256(_mm_unpacklo_epi64(top,bottom)),
                                _mm_unpackhi_epi64(top,bottom),1);
}
__m256 bilinear(__m256 a, __m256 b, __m256 c, __m256 d, __m256 fx, __m256 fy) {
  const auto upper = _mm256_add_ps(a, _mm256_mul_ps(_mm256_sub_ps(b,a),fx));
  const auto lower = _mm256_add_ps(c, _mm256_mul_ps(_mm256_sub_ps(d,c),fx));
  return _mm256_add_ps(_mm256_mul_ps(upper,_mm256_sub_ps(_mm256_set1_ps(1),fy)),_mm256_mul_ps(lower,fy));
}
__m128 average(__m256 value) {
  value = _mm256_mul_ps(value,_mm256_set1_ps(.25f));
  const auto sum = [](__m128 v) {
    auto a = _mm_add_ss(v,_mm_shuffle_ps(v,v,_MM_SHUFFLE(1,1,1,1)));
    a = _mm_add_ss(a,_mm_shuffle_ps(v,v,_MM_SHUFFLE(2,2,2,2)));
    return _mm_add_ss(a,_mm_shuffle_ps(v,v,_MM_SHUFFLE(3,3,3,3)));
  };
  const auto low = sum(_mm256_castps256_ps128(value)), high = sum(_mm256_extractf128_ps(value,1));
  return _mm_shuffle_ps(low,high,_MM_SHUFFLE(0,0,0,0)); // avg0,avg0,avg1,avg1
}
bool sensitive(__m256 value) {
  // Conservative absolute guard for bounded 8-bit bilinear RGB and BT.709.
  // The absolute error bound is < 0.0008, including directed IEEE rounding.
  // Rounding and edge cases are covered by native/tests/pixel_precision.cpp.
  // Recompute both blocks in double within 0.002 of a byte boundary.
  const auto half = _mm256_add_ps(_mm256_floor_ps(value),_mm256_set1_ps(.5f));
  const auto distance = _mm256_andnot_ps(_mm256_set1_ps(-0.f),_mm256_sub_ps(value,half));
  return _mm256_movemask_ps(_mm256_cmp_ps(distance,_mm256_set1_ps(.002f),_CMP_LE_OQ)) != 0;
}
template<bool Trace>
void render(BgraView source, Rect crop, Rect viewport, uint32_t width, uint32_t height,
            uint8_t* output, Rect area, const ScaleTap* horizontal,
            const ScaleTap* vertical, PixelPrecisionStats* stats) {
  for (int y = area.y; y < area.y + area.height; y += 2) {
    int x = area.x;
    for (; x + 4 <= area.x + area.width; x += 4) {
      Rgb8 rgb;
      if (!horizontal) {
        const auto pixels = identity(source, static_cast<uint32_t>(crop.x + x - viewport.x),
                                     static_cast<uint32_t>(crop.y + y - viewport.y));
        rgb = {component<16>(pixels),component<8>(pixels),component<0>(pixels)};
      } else {
        const auto* tx = horizontal + x - viewport.x;
        const auto* ty = vertical + y - viewport.y;
        const auto a = gather(source,tx,ty,false,false), b = gather(source,tx,ty,true,false),
                   c = gather(source,tx,ty,false,true), d = gather(source,tx,ty,true,true);
        const auto fx = _mm256_setr_ps(float(tx[0].fraction),float(tx[1].fraction),float(tx[0].fraction),float(tx[1].fraction),
                                       float(tx[2].fraction),float(tx[3].fraction),float(tx[2].fraction),float(tx[3].fraction));
        const auto fy = _mm256_setr_ps(float(ty[0].fraction),float(ty[0].fraction),float(ty[1].fraction),float(ty[1].fraction),
                                       float(ty[0].fraction),float(ty[0].fraction),float(ty[1].fraction),float(ty[1].fraction));
        rgb = {bilinear(component<16>(a),component<16>(b),component<16>(c),component<16>(d),fx,fy),
               bilinear(component<8>(a),component<8>(b),component<8>(c),component<8>(d),fx,fy),
               bilinear(component<0>(a),component<0>(b),component<0>(c),component<0>(d),fx,fy)};
      }
      auto luma = _mm256_add_ps(_mm256_set1_ps(16),_mm256_mul_ps(rgb.r,_mm256_set1_ps(.182586f)));
      luma = _mm256_add_ps(luma,_mm256_mul_ps(rgb.g,_mm256_set1_ps(.614231f)));
      luma = _mm256_add_ps(luma,_mm256_mul_ps(rgb.b,_mm256_set1_ps(.062007f)));
      auto uv = _mm_add_ps(_mm_set1_ps(128),_mm_mul_ps(average(rgb.r),_mm_setr_ps(-.100644f,.439216f,-.100644f,.439216f)));
      uv = _mm_add_ps(uv,_mm_mul_ps(average(rgb.g),_mm_setr_ps(-.338572f,-.398942f,-.338572f,-.398942f)));
      uv = _mm_add_ps(uv,_mm_mul_ps(average(rgb.b),_mm_setr_ps(.439216f,-.040274f,.439216f,-.040274f)));
      if constexpr (Trace) ++stats->blocks;
      if (sensitive(luma) || sensitive(_mm256_insertf128_ps(_mm256_setzero_ps(),uv,0))) {
        if constexpr (Trace) ++stats->precise_blocks;
        render_pixels_avx2(source,crop,viewport,width,height,output,{x,y,4,2},horizontal,vertical);
        continue;
      }
      alignas(32) int ybytes[8];
      alignas(16) int uvbytes[4];
      luma = _mm256_min_ps(_mm256_max_ps(luma,_mm256_setzero_ps()),_mm256_set1_ps(255));
      uv = _mm_min_ps(_mm_max_ps(uv,_mm_setzero_ps()),_mm_set1_ps(255));
      _mm256_store_si256(reinterpret_cast<__m256i*>(ybytes),_mm256_cvttps_epi32(_mm256_add_ps(luma,_mm256_set1_ps(.5f))));
      _mm_store_si128(reinterpret_cast<__m128i*>(uvbytes),_mm_cvttps_epi32(_mm_add_ps(uv,_mm_set1_ps(.5f))));
      const auto at = size_t(y) * width + static_cast<size_t>(x);
      const auto chroma = size_t(width) * height + size_t(y / 2) * width + static_cast<size_t>(x);
      for (int i = 0; i < 2; ++i) {
        output[at + size_t(i)] = static_cast<uint8_t>(ybytes[i]);
        output[at + 2 + size_t(i)] = static_cast<uint8_t>(ybytes[4+i]);
        output[at + width + size_t(i)] = static_cast<uint8_t>(ybytes[2+i]);
        output[at + width + 2 + size_t(i)] = static_cast<uint8_t>(ybytes[6+i]);
      }
      for (int i = 0; i < 4; ++i) output[chroma + size_t(i)] = static_cast<uint8_t>(uvbytes[i]);
    }
    if (x < area.x + area.width)
      render_pixels_avx2(source,crop,viewport,width,height,output,{x,y,2,2},horizontal,vertical);
  }
  _mm256_zeroupper();
}
} // namespace
void render_pixels_guarded_avx2(BgraView source, Rect crop, Rect viewport,
                                uint32_t width, uint32_t height, uint8_t* output,
                                Rect area, const ScaleTap* horizontal,
                                const ScaleTap* vertical, PixelPrecisionStats* stats) {
  if (stats) render<true>(source,crop,viewport,width,height,output,area,horizontal,vertical,stats);
  else render<false>(source,crop,viewport,width,height,output,area,horizontal,vertical,nullptr);
}
} // namespace nss::capture::detail
