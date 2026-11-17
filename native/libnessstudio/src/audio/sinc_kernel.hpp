#pragma once
#include "dsp.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <limits>
#include <stdexcept>
#include <vector>

namespace nss::audio {
// Original 32-tap Blackman-windowed sinc. Four-point Lagrange interpolation
// evaluates continuous fractional phase; coefficients are never snapped to a
// phase bin. The table is generated from the existing analytical equation.
class SincKernel final {
    static constexpr size_t phases = 2048;
    using Weights = std::array<double, 32>;
    std::vector<Weights> coefficients_;
    dsp::Kernel implementation_;
    double maximum_gain_ = std::numeric_limits<double>::infinity();

public:
    explicit SincKernel(double cutoff, dsp::Isa isa = dsp::Isa::Automatic)
        : implementation_(dsp::kernel(isa)) {
        if (!(cutoff > 0 && cutoff <= 1))
            throw std::invalid_argument("Sinc cutoff must be in (0, 1]");
        coefficients_.resize(phases + 4);
        for (size_t phase = 0; phase < coefficients_.size(); ++phase) {
            // Analytical ghost points keep the same tap support at both ends.
            const double fraction = (static_cast<double>(phase) - 1) / double(phases);
            for (size_t tap = 0; tap < 32; ++tap) {
                const double d = static_cast<double>(tap) - 15 - fraction;
                const double x = d * cutoff;
                const double sinc = std::abs(x) < 1e-12 ? 1.0
                    : std::sin(std::numbers::pi * x) / (std::numbers::pi * x);
                const double window = .42 + .5 * std::cos(std::numbers::pi * d / 16)
                                          + .08 * std::cos(2 * std::numbers::pi * d / 16);
                coefficients_[phase][tap] = sinc * window * cutoff;
            }
        }
        double minimum_sum = std::numeric_limits<double>::infinity();
        double maximum_sum = -std::numeric_limits<double>::infinity();
        double maximum_absolute_sum{};
        for (const auto &row : coefficients_) {
            double sum{}, absolute_sum{};
            for (const auto value : row) {
                sum += value;
                absolute_sum += std::abs(value);
            }
            minimum_sum = (std::min)(minimum_sum, sum);
            maximum_sum = (std::max)(maximum_sum, sum);
            maximum_absolute_sum = (std::max)(maximum_absolute_sum, absolute_sum);
        }
        // For t in [0,1], the four local Lagrange weights sum to 1,
        // their negative mass is <=1/8 and absolute mass is <=5/4.
        const double minimum_normalization = minimum_sum - (maximum_sum - minimum_sum) / 8;
        if (minimum_normalization > 0)
            maximum_gain_ = 1.25 * maximum_absolute_sum / minimum_normalization * (1 + 1e-10);
    }

    Weights weights(double fraction) const {
        if (!(fraction >= 0 && fraction <= 1))
            throw std::invalid_argument("Sinc fractional phase must be in [0, 1]");
        const double position = fraction * double(phases);
        const auto phase = static_cast<size_t>(position);
        const double t = position - double(phase);
        const double a = -t * (t - 1) * (t - 2) / 6;
        const double b = (t + 1) * (t - 1) * (t - 2) / 2;
        const double c = -(t + 1) * t * (t - 2) / 2;
        const double d = (t + 1) * t * (t - 1) / 6;
        Weights result{};
        implementation_.interpolate(coefficients_[phase].data(), coefficients_[phase + 1].data(),
            coefficients_[phase + 2].data(), coefficients_[phase + 3].data(), {a,b,c,d}, result.data());
        return result;
    }
    dsp::Stereo filter(const dsp::Stereo *samples, size_t count, int64_t first, const Weights &values) const {
        if (!samples || !count) throw std::invalid_argument("Sinc filtering requires input samples");
        return implementation_.filter(samples, count, first, values.data());
    }
    const char *instruction_set() const noexcept { return implementation_.name; }
    size_t storage_bytes() const noexcept {
        return coefficients_.size() * sizeof(Weights);
    }
    double maximum_normalized_gain() const noexcept { return maximum_gain_; }
    double pcm16_zero_input_limit() const noexcept {
        // A quarter-LSB bound leaves ample room for floating-point rounding
        // before the existing gain<=1 ramps and nearest-integer PCM16 commit.
        return .25 / (32768 * maximum_gain_);
    }
};
} // namespace nss::audio
