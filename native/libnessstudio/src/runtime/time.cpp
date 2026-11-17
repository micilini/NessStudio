#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <intrin.h>
#include <immintrin.h>
#include "time.hpp"
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace nss {
uint64_t scale_floor(uint64_t value, uint64_t numerator, uint64_t denominator) {
    if (!denominator) throw std::invalid_argument("Zero time denominator");
    uint64_t high{}, remainder{};
    const auto low = _umul128(value, numerator, &high);
    if (high >= denominator) throw std::overflow_error("Time quotient overflow");
    return _udiv128(high, low, denominator, &remainder);
}
static int64_t signed_time(uint64_t value) {
    if (value > static_cast<uint64_t>((std::numeric_limits<int64_t>::max)())) throw std::overflow_error("Signed time overflow");
    return static_cast<int64_t>(value);
}
int64_t qpc_hns() {
    static const uint64_t frequency = [] { LARGE_INTEGER value{}; if (!QueryPerformanceFrequency(&value) || value.QuadPart <= 0) throw std::runtime_error("QPC unavailable"); return static_cast<uint64_t>(value.QuadPart); }();
    LARGE_INTEGER value{};
    if (!QueryPerformanceCounter(&value) || value.QuadPart < 0) throw std::runtime_error("QPC read failed");
    return signed_time(scale_floor(static_cast<uint64_t>(value.QuadPart), hns_per_second, frequency));
}
int64_t frame_time(uint64_t index, uint32_t numerator, uint32_t denominator) {
    if (!numerator || !denominator || denominator > 100000 || static_cast<uint64_t>(numerator) < denominator || static_cast<uint64_t>(numerator) > 120ULL * denominator)
        throw std::invalid_argument("Unsupported rational frame rate");
    const auto value = signed_time(scale_floor(index, static_cast<uint64_t>(hns_per_second) * denominator, numerator));
    if (value > maximum_session_hns) throw std::out_of_range("Session exceeds 24 hours");
    return value;
}
int64_t sample_time(uint64_t index, uint32_t rate) {
    if (rate != 44100 && rate != 48000) throw std::invalid_argument("Unsupported sample rate");
    const auto value = signed_time(scale_floor(index, hns_per_second, rate));
    if (value > maximum_session_hns) throw std::out_of_range("Session exceeds 24 hours");
    return value;
}
SessionClock::SessionClock() { intervals_.reserve(interval_limit); }
void SessionClock::check_time(int64_t at) const {
    if (at < last_ || at < epoch_ || at - epoch_ > maximum_session_hns) throw std::out_of_range("Nonmonotonic or out-of-domain session time");
}
void SessionClock::start(int64_t at) {
    std::lock_guard lock(mutex_);
    if (started_ || stopped_ || at < 0) throw std::logic_error("Clock already started/stopped or invalid epoch");
    started_ = open_ = true; epoch_ = last_ = at;
    intervals_.push_back({at, -1, 0, 1});
}
void SessionClock::pause(int64_t at) {
    std::lock_guard lock(mutex_); check_time(at);
    if (!started_ || !open_ || stopped_) throw std::logic_error("Clock is not active");
    intervals_.back().end = at; active_ += at - intervals_.back().begin;
    last_ = at; open_ = false;
}
void SessionClock::resume(int64_t at) {
    std::lock_guard lock(mutex_); check_time(at);
    if (!started_ || open_ || stopped_) throw std::logic_error("Clock is not paused");
    if (intervals_.size() == interval_limit) throw std::length_error("Pause history budget exhausted");
    intervals_.push_back({at, -1, active_, intervals_.size() + 1}); last_ = at; open_ = true;
}
void SessionClock::stop(int64_t at) {
    std::lock_guard lock(mutex_);
    if (stopped_) return;
    if (started_) {
        check_time(at);
        if (open_) { intervals_.back().end = at; active_ += at - intervals_.back().begin; }
        last_ = at;
    }
    stopped_ = true; open_ = false;
}
std::optional<int64_t> SessionClock::map(int64_t at) const {
    std::lock_guard lock(mutex_);
    if (!started_ || at < epoch_ || at - epoch_ > maximum_session_hns) return {};
    const auto next = std::upper_bound(intervals_.begin(), intervals_.end(), at, [](int64_t value, const ActiveInterval& interval) { return value < interval.begin; });
    if (next == intervals_.begin()) return {};
    const auto& interval = *std::prev(next);
    if (interval.end >= 0 && at >= interval.end) return {};
    return interval.active_begin + at - interval.begin;
}
bool SessionClock::eligible(int64_t begin, int64_t end) const {
    std::lock_guard lock(mutex_);
    if (!started_ || begin < epoch_ || end < begin || end - epoch_ > maximum_session_hns) return false;
    const auto next = std::upper_bound(intervals_.begin(), intervals_.end(), begin, [](int64_t value, const ActiveInterval& interval) { return value < interval.begin; });
    if (next == intervals_.begin()) return false;
    const auto& interval = *std::prev(next);
    return interval.end < 0 || (begin < interval.end && end <= interval.end);
}
std::vector<AudioSlice> SessionClock::trim_audio(int64_t begin, uint64_t samples, uint32_t rate) const {
    if (samples > rate / 5) throw std::length_error("Audio packet exceeds the 200 ms budget");
    const auto duration = sample_time(samples, rate);
    if (begin < 0 || begin > (std::numeric_limits<int64_t>::max)() - duration) throw std::overflow_error("Audio timestamp overflow");
    const auto intervals = history(); std::vector<AudioSlice> result;
    // Packet size is bounded; searching integer sample starts avoids inclusion-rounding ambiguity.
    for (const auto& interval : intervals) {
        uint64_t low = 0, high = samples;
        while (low < high) { const auto middle = low + (high - low) / 2; if (begin + sample_time(middle, rate) < interval.begin) low = middle + 1; else high = middle; }
        const auto first = low; high = samples;
        while (low < high) { const auto middle = low + (high - low) / 2; if (interval.end < 0 || begin + sample_time(middle, rate) < interval.end) low = middle + 1; else high = middle; }
        if (low > first) result.push_back({first, low - first, interval.active_begin + begin + sample_time(first, rate) - interval.begin});
    }
    return result;
}
int64_t SessionClock::active_time(int64_t at) const {
    std::lock_guard lock(mutex_);
    if (!started_ || at < epoch_) return 0;
    // A caller samples QPC before acquiring this mutex. A concurrent control
    // can advance last_ in between; that does not make the sampled time invalid.
    // Project it through the retained intervals instead of rewriting its time
    // or weakening the strictly monotonic command validation in check_time().
    if (at < last_) {
        const auto next = std::upper_bound(intervals_.begin(), intervals_.end(), at,
            [](int64_t value, const ActiveInterval& interval) { return value < interval.begin; });
        if (next == intervals_.begin()) return 0;
        const auto& interval = *std::prev(next);
        const auto end = interval.end < 0 ? at : (std::min)(at, interval.end);
        return interval.active_begin + end - interval.begin;
    }
    if (!stopped_ && at - epoch_ > maximum_session_hns)
        throw std::out_of_range("Session time exceeds its domain");
    return active_ + (open_ ? at - intervals_.back().begin : 0);
}
int64_t SessionClock::wall_time(int64_t at) const {
    std::lock_guard lock(mutex_);
    return started_ ? (std::max)(int64_t{}, (stopped_ ? (std::min)(at,last_) : at) - epoch_) : 0;
}
bool SessionClock::can_resume() const { std::lock_guard lock(mutex_); return intervals_.size() < interval_limit; }
std::vector<ActiveInterval> SessionClock::history() const { std::lock_guard lock(mutex_); return intervals_; }
int64_t TimestampAdapter::convert(uint64_t value) const {
    int64_t result = origin == TimeOrigin::QpcTicks ? signed_time(scale_floor(value, hns_per_second, frequency)) : signed_time(value);
    if (origin == TimeOrigin::DeviceHns) {
        if (!device_offset) throw std::invalid_argument("Device timestamp requires an explicit correlation");
        if (*device_offset > 0 && result > (std::numeric_limits<int64_t>::max)() - *device_offset) throw std::overflow_error("Device correlation overflow");
        if (*device_offset < 0 && static_cast<uint64_t>(result) < static_cast<uint64_t>(-(*device_offset + 1)) + 1) throw std::overflow_error("Negative correlated timestamp");
        result += *device_offset;
    }
    if (uncertainty_hns < 0) throw std::invalid_argument("Negative acquisition uncertainty");
    return result;
}
bool EligibleVideoCache::admit(const SessionClock& clock, VideoIdentity frame) {
    if (!clock.eligible(frame.begin, frame.end) || (last_ && frame.end < last_->end)) { ++rejected; return false; }
    last_ = frame; ++accepted; return true;
}
std::optional<VideoIdentity> EligibleVideoCache::select(bool new_content) {
    if (last_ && !new_content) { if (last_->confirmed_static) ++reused_static; else ++forced_repeats; }
    return last_;
}
std::optional<ScheduledFrame> CfrScheduler::next(int64_t active, EligibleVideoCache& cache, bool new_content) {
    if (active < 0 || active > maximum_session_hns) throw std::out_of_range("Scheduler time outside session domain");
    const auto pts = frame_time(next_, numerator_, denominator_);
    if (pts > active) return {};
    const auto end = frame_time(next_ + 1, numerator_, denominator_);
    ScheduledFrame result{next_++, 0, pts, end - pts, !new_content, false, false}; ++expected_slots;
    const auto frame = cache.select(new_content);
    if (!frame) { result.missing = true; ++missing; return result; }
    result.frame_id = frame->id; ++emitted;
    if (result.reused) { ++reused; if (!frame->confirmed_static) { result.forced = true; ++forced; } }
    return result;
}
}
