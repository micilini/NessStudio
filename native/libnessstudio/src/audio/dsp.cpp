#include "dsp.hpp"
#include <algorithm>
#include <stdexcept>

namespace nss::audio::dsp {
Capabilities capabilities() {
  return nss::cpu::capabilities();
}
Isa select(Capabilities value) noexcept {
  if (nss::cpu::has_avx2(value))
    return Isa::Avx2;
  return value.sse2 ? Isa::Sse2 : Isa::Scalar;
}
Kernel kernel(Isa requested) {
  const auto available = select(capabilities());
  if (requested == Isa::Automatic) requested = available;
  if (requested == Isa::Avx2 && available != Isa::Avx2)
    throw std::invalid_argument("AVX2 requires CPU support and enabled XMM/YMM OS state");
  if (requested == Isa::Sse2 && available == Isa::Scalar)
    throw std::invalid_argument("SSE2 is unavailable");
  switch (requested) {
  case Isa::Avx2: return {requested, "avx2", interpolate_avx2, filter_avx2};
  case Isa::Sse2: return {requested, "sse2", interpolate_sse2, filter_sse2};
  case Isa::Scalar: return {requested, "scalar", interpolate_scalar, filter_scalar};
  default: throw std::invalid_argument("Unknown audio instruction set");
  }
}
void interpolate_scalar(const double *a, const double *b, const double *c, const double *d,
                        const std::array<double, 4> &w, double *out) {
  for (size_t i = 0; i < 32; ++i) out[i] = a[i]*w[0] + b[i]*w[1] + c[i]*w[2] + d[i]*w[3];
}
Stereo filter_scalar(const Stereo *samples, size_t count, int64_t first, const double *weights) {
  std::array<double, 2> sum{};
  double norm{};
  for (int64_t tap = 0; tap < 32; ++tap) {
    const auto &sample = samples[static_cast<size_t>(std::clamp(first + tap, int64_t{}, static_cast<int64_t>(count - 1)))];
    norm += weights[tap];
    for (size_t channel = 0; channel < 2; ++channel) sum[channel] += sample[channel] * weights[tap];
  }
  return {static_cast<float>(sum[0] / norm), static_cast<float>(sum[1] / norm)};
}
} // namespace nss::audio::dsp
