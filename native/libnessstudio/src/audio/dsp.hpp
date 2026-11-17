#pragma once
#include "../cpu_features.hpp"
#include <array>
#include <cstddef>
#include <cstdint>

namespace nss::audio::dsp {
using Stereo = std::array<float, 2>;
enum class Isa { Automatic, Scalar, Sse2, Avx2 };
using Capabilities = nss::cpu::Capabilities;
using Interpolate = void (*)(const double *, const double *, const double *, const double *,
                            const std::array<double, 4> &, double *);
using Filter = Stereo (*)(const Stereo *, size_t, int64_t, const double *);
struct Kernel { Isa isa; const char *name; Interpolate interpolate; Filter filter; };
Capabilities capabilities();
Isa select(Capabilities value) noexcept;
Kernel kernel(Isa requested = Isa::Automatic);
void interpolate_scalar(const double *, const double *, const double *, const double *, const std::array<double, 4> &, double *);
void interpolate_sse2(const double *, const double *, const double *, const double *, const std::array<double, 4> &, double *);
void interpolate_avx2(const double *, const double *, const double *, const double *, const std::array<double, 4> &, double *);
Stereo filter_scalar(const Stereo *, size_t, int64_t, const double *);
Stereo filter_sse2(const Stereo *, size_t, int64_t, const double *);
Stereo filter_avx2(const Stereo *, size_t, int64_t, const double *);
} // namespace nss::audio::dsp
