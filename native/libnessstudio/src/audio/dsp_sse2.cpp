#include "dsp.hpp"
#include <algorithm>
#include <emmintrin.h>
#include <cstring>

namespace nss::audio::dsp {
void interpolate_sse2(const double *a, const double *b, const double *c, const double *d,
                      const std::array<double, 4> &w, double *out) {
  const auto wa = _mm_set1_pd(w[0]), wb = _mm_set1_pd(w[1]),
             wc = _mm_set1_pd(w[2]), wd = _mm_set1_pd(w[3]);
  for (size_t i = 0; i < 32; i += 2) {
    auto value = _mm_add_pd(_mm_mul_pd(_mm_loadu_pd(a+i), wa), _mm_mul_pd(_mm_loadu_pd(b+i), wb));
    value = _mm_add_pd(value, _mm_mul_pd(_mm_loadu_pd(c+i), wc));
    value = _mm_add_pd(value, _mm_mul_pd(_mm_loadu_pd(d+i), wd));
    _mm_storeu_pd(out+i, value);
  }
}
Stereo filter_sse2(const Stereo *samples, size_t count, int64_t first, const double *weights) {
  auto sum = _mm_setzero_pd();
  double norm{};
  for (int64_t tap = 0; tap < 32; ++tap) {
    const auto &sample = samples[static_cast<size_t>(std::clamp(first + tap, int64_t{}, static_cast<int64_t>(count - 1)))];
    int64_t bits;
    static_assert(sizeof(sample) == sizeof(bits));
    std::memcpy(&bits, sample.data(), sizeof(bits));
    const auto values = _mm_cvtps_pd(_mm_castsi128_ps(_mm_cvtsi64_si128(bits)));
    sum = _mm_add_pd(sum, _mm_mul_pd(values, _mm_set1_pd(weights[tap])));
    norm += weights[tap];
  }
  std::array<double, 2> output;
  _mm_storeu_pd(output.data(), _mm_div_pd(sum, _mm_set1_pd(norm)));
  return {static_cast<float>(output[0]), static_cast<float>(output[1])};
}
} // namespace nss::audio::dsp
