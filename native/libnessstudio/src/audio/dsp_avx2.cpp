#include "dsp.hpp"
#include <immintrin.h>

namespace nss::audio::dsp {
// This is the audio AVX2 translation unit. No dispatcher, loader or scalar
// fallback is compiled with /arch:AVX2. FMA contraction stays disabled.
void interpolate_avx2(const double *a, const double *b, const double *c, const double *d,
                      const std::array<double, 4> &w, double *out) {
  const auto wa = _mm256_set1_pd(w[0]), wb = _mm256_set1_pd(w[1]),
             wc = _mm256_set1_pd(w[2]), wd = _mm256_set1_pd(w[3]);
  for (size_t i = 0; i < 32; i += 4) {
    auto value = _mm256_add_pd(_mm256_mul_pd(_mm256_loadu_pd(a+i), wa), _mm256_mul_pd(_mm256_loadu_pd(b+i), wb));
    value = _mm256_add_pd(value, _mm256_mul_pd(_mm256_loadu_pd(c+i), wc));
    value = _mm256_add_pd(value, _mm256_mul_pd(_mm256_loadu_pd(d+i), wd));
    _mm256_storeu_pd(out+i, value);
  }
}
Stereo filter_avx2(const Stereo *samples, size_t count, int64_t first, const double *weights) {
  if (first < 0 || static_cast<uint64_t>(first) + 32 > count)
    return filter_sse2(samples, count, first, weights);
  auto sum = _mm_setzero_pd();
  double norm{};
  for (size_t tap = 0; tap < 32; tap += 2) {
    const auto values = _mm256_cvtps_pd(_mm_loadu_ps(samples[static_cast<size_t>(first) + tap].data()));
    const auto w = _mm256_setr_pd(weights[tap], weights[tap], weights[tap+1], weights[tap+1]);
    const auto product = _mm256_mul_pd(values, w);
    // Retain the scalar accumulation order independently for both channels.
    sum = _mm_add_pd(sum, _mm256_castpd256_pd128(product));
    sum = _mm_add_pd(sum, _mm256_extractf128_pd(product, 1));
    norm += weights[tap];
    norm += weights[tap+1];
  }
  std::array<double, 2> output;
  _mm_storeu_pd(output.data(), _mm_div_pd(sum, _mm_set1_pd(norm)));
  return {static_cast<float>(output[0]), static_cast<float>(output[1])};
}
} // namespace nss::audio::dsp
