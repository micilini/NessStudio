#pragma once
#include "runtime/time.hpp"
#include <optional>
#include <stdexcept>

namespace nss::capture {
inline constexpr uint32_t acquisition_sample_numerator = 1;
inline constexpr uint32_t acquisition_sample_denominator = 3;
struct AcquisitionWindow {
    uint64_t index{};
    int64_t begin{}, end{}, due{};
};
inline AcquisitionWindow acquisition_window(int64_t active, uint32_t numerator, uint32_t denominator) {
    if (active < 0 || active >= maximum_session_hns ||
        !recording_cadence_supported(numerator, denominator))
        throw std::invalid_argument("Acquisition cadence is outside the recording clock domain");
    const auto ticks = uint64_t(hns_per_second) * denominator;
    auto index = scale_floor(static_cast<uint64_t>(active), numerator, ticks);
    // Invert the floor-rounded rational clock, including an exact boundary.
    const auto boundary = [&](uint64_t i) { return static_cast<int64_t>(scale_floor(i, ticks, numerator)); };
    if (boundary(index + 1) <= active) ++index;
    const auto begin = boundary(index), end = boundary(index + 1);
    // Start in the first third: a nonblocking duplication query can still
    // spend milliseconds inside the driver. Leave the rest of the window for
    // that call and retries when the next source refresh is not ready yet.
    // Never change source timestamps.
    // The final window can extend beyond a partial Stop cut; eligibility still
    // belongs to SessionClock and the unchanged output scheduler.
    return {index, begin, end, begin + (end - begin) * acquisition_sample_numerator /
                                                    acquisition_sample_denominator};
}
class WindowSampling final {
    std::optional<uint64_t> acquired_;
public:
    std::optional<int64_t> minimum_source_time(const AcquisitionWindow &window) const noexcept {
        // A newer image of an already served preceding window cannot serve
        // this one. Let the capture owner return it before an owned GPU copy.
        // Missing windows and resume must retain the late-recovery path.
        return window.index > 0 && acquired_ == window.index - 1
            ? std::optional<int64_t>{window.begin} : std::nullopt;
    }
    bool due(const AcquisitionWindow &window, int64_t active) const noexcept {
        // A presentation near the previous window's end can become available
        // just after the owner crosses that boundary. If that window was not
        // served, poll now instead of waiting for this window's normal sample:
        // by then the next desktop update may have replaced the eligible image.
        // A recovered older image stays eligible for the original CFR window;
        // observe() keeps retries enabled until this window also has an image.
        const bool previous_missing = window.index > 0 &&
            (!acquired_ || *acquired_ < window.index - 1);
        return acquired_ != window.index &&
            (active >= window.due || (active >= window.begin && previous_missing));
    }
    std::optional<int64_t> idle_until(const AcquisitionWindow &window, int64_t active) const noexcept {
        // Only an already served window, or time before the first sample, can
        // suppress polling. Stop at the boundary so recovery of a missing
        // preceding window is reconsidered using the next rational window.
        if (active < window.begin || active >= window.end || due(window, active))
            return {};
        return acquired_ == window.index ? window.end : window.due;
    }
    bool observe(const AcquisitionWindow &window, int64_t source_active) noexcept {
        // The existing scheduler assigns an exact end boundary to the earlier
        // output window. A stale/future acquisition must leave retries enabled.
        if (source_active <= window.begin || source_active > window.end) return false;
        acquired_ = window.index;
        return true;
    }
    void reset() noexcept { acquired_.reset(); }
};
} // namespace nss::capture
