#pragma once
#include <cstdint>
#include <mutex>
#include <optional>
#include <vector>

namespace nss {
constexpr int64_t hns_per_second = 10000000;
constexpr int64_t maximum_session_hns = 24LL * 60 * 60 * hns_per_second;
// Product output presets. Source cadence is negotiated independently, and the
// scheduler retains the exact rational timestamps for each accepted preset.
constexpr bool recording_cadence_supported(uint32_t numerator, uint32_t denominator) noexcept {
    return (denominator == 1 && (numerator == 24 || numerator == 25 || numerator == 30 ||
            numerator == 48 || numerator == 50 || numerator == 60)) ||
           (numerator == 30000 && denominator == 1001);
}
// Floor rounding, checked 128-bit intermediate, no repeated truncated additions.
uint64_t scale_floor(uint64_t value, uint64_t numerator, uint64_t denominator);
int64_t qpc_hns();
int64_t frame_time(uint64_t index, uint32_t numerator, uint32_t denominator);
int64_t sample_time(uint64_t index, uint32_t rate);

struct ActiveInterval { int64_t begin{}, end{}, active_begin{}; uint64_t generation{}; };
struct AudioSlice { uint64_t first{}, count{}; int64_t pts_hns{}; };
class SessionClock final {
    mutable std::mutex mutex_;
    std::vector<ActiveInterval> intervals_;
    int64_t epoch_{}, last_{}, active_{};
    bool started_{}, open_{}, stopped_{};
    static constexpr size_t interval_limit = 4096;
    void check_time(int64_t at) const;
public:
    SessionClock();
    void start(int64_t at);
    void pause(int64_t at);
    void resume(int64_t at);
    void stop(int64_t at);
    std::optional<int64_t> map(int64_t acquisition) const;
    bool eligible(int64_t begin, int64_t end) const;
    std::vector<AudioSlice> trim_audio(int64_t packet_begin, uint64_t samples, uint32_t rate) const;
    int64_t active_time(int64_t at) const;
    int64_t wall_time(int64_t at) const;
    bool can_resume() const;
    std::vector<ActiveInterval> history() const;
};

enum class TimeOrigin { QpcTicks, WgcHns, WasapiQpcHns, DeviceHns };
struct TimestampAdapter {
    TimeOrigin origin;
    uint64_t frequency{};
    std::optional<int64_t> device_offset;
    bool estimated{};
    int64_t uncertainty_hns{};
    int64_t convert(uint64_t value) const;
};
struct VideoIdentity { uint64_t id{}; int64_t begin{}, end{}; bool confirmed_static{}; };
class EligibleVideoCache final {
    std::optional<VideoIdentity> last_;
public:
    uint64_t accepted{}, rejected{}, reused_static{}, forced_repeats{};
    bool admit(const SessionClock& clock, VideoIdentity frame);
    std::optional<VideoIdentity> select(bool new_content);
};
struct ScheduledFrame { uint64_t index{}, frame_id{}; int64_t pts{}, duration{}; bool reused{}, forced{}, missing{}; };
class CfrScheduler final {
    uint32_t numerator_, denominator_; uint64_t next_{};
public:
    uint64_t expected_slots{}, emitted{}, missing{}, reused{}, forced{};
    CfrScheduler(uint32_t numerator, uint32_t denominator) : numerator_(numerator), denominator_(denominator) { (void)frame_time(0, numerator, denominator); }
    std::optional<ScheduledFrame> next(int64_t active_time, EligibleVideoCache& cache, bool new_content);
};
}
