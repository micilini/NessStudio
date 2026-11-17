#pragma once
#include "runtime/time.hpp"
#include "runtime/buffers.hpp"
#include <nessstudio/nessstudio.h>
#include "json.hpp"
#include <atomic>
#include <deque>
#include <iterator>
#include <memory>
#include <span>
#include <string>

namespace nss::camera { struct Plan; }
namespace nss::capture {
using Json = nlohmann::json;
// Allow bounded GPU completion after a capture window closes, without moving
// source timestamps or admitting acquisitions outside the active cut.
inline constexpr int64_t completion_grace_hns = hns_per_second / 30 + 1;
struct Error : std::runtime_error {
    nss_result code;
    uint32_t stage;
    int32_t native;
    Error(nss_result c, uint32_t s, int32_t n, const char *text)
        : std::runtime_error(text), code(c), stage(s), native(n) {}
};
void checked(int32_t result, uint32_t stage, const char *operation);
struct Rect {
    int32_t x{}, y{}, width{}, height{};
};
struct Display {
    std::string id, name;
    void *monitor{};
    Rect bounds;
    uint64_t adapter_luid{};
    uint32_t source_id{}, target_id{}, dpi{96};
    bool primary{}, color_known{}, hdr{};
};
std::vector<Display> displays();
void require_interactive_desktop();
Json display_json(const Display &display);
struct Plan {
    Display display;
    Rect crop, viewport;
    uint32_t width{}, height{}, requested_width{}, requested_height{}, fps_num{30}, fps_den{1}, slots{3},
        duration_ms{2000};
    uint64_t max_bytes{128ULL * 1024 * 1024};
    bool gpu{true}, cursor{true}, trace{}, pad_even{}, full_source{true};
    // Internal recording policy: retain bounded source acquisitions, then copy
    // and convert selected output/preview frames. Generic capture remains eager.
    bool defer_conversion{};
    // Direct software encoding consumes a CPU staging copy on this owner.
    // Prefer nonblocking event queries for that ordered stream; hardware and
    // shared scene consumers retain their timeline-fence policy.
    bool prefer_query_completion{};
    // Explicit experimental monitor route; WGC remains the normal GPU source.
    bool desktop_duplication{};
    // Internal SDR recording policy. Generic capture retains format negotiation.
    bool prefer_legacy_duplication{};
};
Rect local_crop(Rect monitor, Rect selection, bool global);
Rect fit_viewport(uint32_t source_width, uint32_t source_height, uint32_t width, uint32_t height);
Plan plan(const Json &request);
Json plan_json(const Plan &value);
inline std::atomic<uint64_t> live_pipelines{}, live_surfaces{}, live_capture_bytes{};
struct Surface {
    Surface() { ++live_surfaces; }
    virtual std::span<const std::byte> cpu_pixels() const { return {}; }
    virtual void *gpu_texture() const noexcept { return nullptr; }
    virtual void *gpu_device() const noexcept { return nullptr; }
    virtual uint64_t adapter_luid() const noexcept { return 0; }
    virtual ~Surface() { --live_surfaces; }
};
struct Frame {
    std::shared_ptr<const Surface> surface;
    uint64_t id{}, generation{};
    int64_t begin_hns{}, end_hns{}, capture_hns{};
    uint32_t width{}, height{}, stride{};
    bool estimated{};
    // Internal graphics metadata; the public preview ABI remains BGRA/D3D9.
    enum class Format { Nv12, Bgra } format{Format::Nv12};
    enum class Color { Bt709, Bt601 } color{Color::Bt709};
    enum class Alpha { Opaque, Straight, Premultiplied } alpha{Alpha::Opaque};
    uint32_t rotation{};
    // Program output presentation clock is separate from actual render acquisition.
    std::optional<int64_t> presentation_hns;
    // GDI has no presentation timestamp: capture_hns is the measured
    // acquisition interval's midpoint. Other estimated clocks (for example a
    // camera stream clock) must not opt into interval-based selection.
    bool acquisition_interval_estimate{};
};
// Keep the newest eligible acquisition for the current window, plus future
// acquisitions. Redundant candidates must release their surface before the
// three-slot CPU pool fills; they are intentional input decimation, not loss.
template<class MapTimestamp>
void coalesce_window(std::deque<Frame>& pending, int64_t boundary,
                     MapTimestamp map, uint64_t& decimated) {
    while(pending.size()>1) {
        const auto next=map(pending[1].presentation_hns.value_or(pending[1].capture_hns));
        if(!next || *next>boundary)break;
        pending.pop_front();
        ++decimated;
    }
}
// A GPU conversion or encoder retry can outlive the original selection. If
// that selection repeats the previous source, prefer an already acquired newer
// candidate from the SAME output window before accepting the repeated frame.
// This never waits for a future acquisition or changes the output timestamp.
template<class MapTimestamp, class Eligible>
bool replace_repeated_candidate(Frame& target, uint64_t previous,
                                std::deque<Frame>& incoming, int64_t boundary,
                                MapTimestamp map, Eligible eligible, uint64_t& decimated) {
    if(target.id!=previous) return false;
    const auto initial=map(target.presentation_hns.value_or(target.capture_hns));
    if(!initial) return false;
    auto newest=*initial;
    auto selected=incoming.end();
    for(auto it=incoming.begin();it!=incoming.end();++it) {
        const auto at=map(it->presentation_hns.value_or(it->capture_hns));
        if(at && *at>boundary) break;
        if(at && *at>newest && eligible(*it)) { selected=it;newest=*at; }
    }
    if(selected==incoming.end()) return false;
    target=*selected;
    decimated+=static_cast<uint64_t>(std::distance(incoming.begin(),selected));
    incoming.erase(incoming.begin(),std::next(selected));
    return true;
}
// A midpoint on either side of a CFR boundary is not evidence that a GDI
// acquisition missed that window. When ordinary point selection would repeat,
// accept the next already completed acquisition only if its measured interval
// contains the boundary. Bound the midpoint lead to half an output period and
// reject long acquisitions. This is a selection policy, not a clock correction:
// source timestamps and output PTS remain unchanged. Full active-cut eligibility
// is still required, including the acquisition end.
template<class MapTimestamp, class Eligible>
std::optional<int64_t> replace_repeated_interval_candidate(
    Frame& target, uint64_t previous, std::deque<Frame>& incoming,
    int64_t boundary, int64_t period, MapTimestamp map, Eligible eligible) {
    if (target.id != previous || incoming.empty() || period <= 0)
        return {};
    const auto& next = incoming.front();
    if (!next.acquisition_interval_estimate || !next.estimated ||
        next.presentation_hns || next.id <= previous ||
        next.begin_hns > next.capture_hns || next.capture_hns > next.end_hns ||
        next.end_hns - next.begin_hns <= 0 ||
        next.end_hns - next.begin_hns > period * 2 || !eligible(next))
        return {};
    const auto at = map(next.capture_hns), begin = map(next.begin_hns),
               end = map(next.end_hns), old = map(target.capture_hns);
    if (!at || !begin || !end || !old || *at <= *old || *at <= boundary ||
        *at - boundary > period / 2 || *begin > boundary || *end < boundary)
        return {};
    const auto lead = *at - boundary;
    target = std::move(incoming.front());
    incoming.pop_front();
    return lead;
}
class Readback;
class Pipeline {
  public:
    enum class ReadbackPreparation { Unsupported, Pending, Ready };
    struct WaitHint {
        // Borrowed auto-reset event. The pipeline owns it through close();
        // only the owner thread polls, waits on and closes this pipeline.
        void *event{};
        bool processing{};
        // Borrowed manual-reset GPU notification, armed by flush_work().
        // The pipeline resets it only after observing the registered signal.
        void *completion_event{};
        // A source without an arrival event can request bounded idle polling.
        // Processing without a completion event still uses the 1 ms fallback.
        uint32_t idle_poll_ms{1};
    };
    Pipeline() {
        if (live_pipelines.fetch_add(1) >= 8) {
            --live_pipelines;
            throw Error(NSS_E_BUSY, NSS_STAGE_CAPTURE, 0, "Native capture instance budget exhausted");
        }
    }
    virtual std::optional<Frame> poll() = 0;
    // Optional owner hint: newly acquired images at/before this QPC timestamp
    // belong to an already served output window. Implementations may return
    // them without copying; previously queued frames remain deliverable.
    virtual std::optional<Frame> poll_after(int64_t) { return poll(); }
    // Owner-thread preparation of a selected frame. False means bounded GPU
    // work is still pending; callers keep the frame lease and retry.
    virtual bool prepare_frame(Frame&) { return true; }
    // Optional same-owner conversion/staging submission. Generic GPU getters
    // stay unavailable until conversion completes; Ready publishes CPU pixels
    // only after the final staging fence and a successful nonblocking Map.
    virtual ReadbackPreparation prepare_readback(Frame&, Readback&) {
        return ReadbackPreparation::Unsupported;
    }
    // Submit queued owner-thread GPU work before waiting for its completion.
    virtual void flush_work() {}
    // Retire completed borrowed resources after waking, before the owner can
    // perform unrelated durable I/O. This does not wait or acquire new frames.
    virtual void retire_completed_work() {}
    virtual WaitHint wait_hint() const noexcept { return {}; }
    // Optional bounded acquisition history, copied only for an observed miss
    // in diagnostic mode. It does not participate in frame selection.
    virtual Json diagnostic_acquisition_history() const { return Json::object(); }
    virtual Json metrics() const = 0;
    virtual void close() = 0;
    virtual ~Pipeline() { --live_pipelines; }
};
std::unique_ptr<Pipeline> make_cpu(const Plan &value);
std::unique_ptr<Pipeline> make_gpu(const Plan &value);
std::unique_ptr<Pipeline> make_camera_gpu(const camera::Plan& source, const Plan& output);
std::unique_ptr<Pipeline> make_camera_gpu_shared(const camera::Plan& source, const Plan& output, const Frame& reference);
Json probe(const Json &request);

// Shared scene semantics. BGRA is straight alpha; the screen is normalized opaque.
struct BgraView {
    const uint8_t *data{};
    uint32_t width{}, height{}, stride{};
    uint64_t bytes{};
};
struct Layer {
    BgraView image;
    Rect destination;
    uint8_t opacity{255};
    bool mirror{};
};
enum class CpuPixelKernel { Automatic, Scalar, Avx2, Avx2Double };
void process_cpu(BgraView source, Rect crop, Rect viewport, uint32_t width, uint32_t height,
                 std::span<uint8_t> nv12, std::span<const Layer> layers = {},
                 CpuPixelKernel kernel = CpuPixelKernel::Automatic);
// A DIB lease can outlive the capture owner and is immutable after GdiFlush.
class GdiSource {
    struct Impl;
    std::unique_ptr<Impl> impl_;

  public:
    // Value snapshot copied by the acquisition owner without allocation.
    // JSON formatting belongs to the diagnostics consumer, outside the queue lock.
    struct Statistics {
        uint64_t frames{}, pressure{}, flushes{}, health_checks{};
        uint64_t cursor_drawn{}, cursor_omitted{}, cursor_requeries{}, cursor_restores{};
        int64_t capture_total{}, capture_max{}, health_total{}, health_max{}, gap_max{};
        uint32_t owner{}, dib_slots{}, mmcss_error{}, cursor_last_error{};
        bool opaque_alpha{}, mmcss_registered{};
        const char *cursor_last_failure = "none";
    };
    struct Image {
        std::shared_ptr<const Surface> owner;
        BgraView pixels;
        int64_t begin{}, end{};
        uint64_t id{};
    };
    explicit GdiSource(const Plan &value);
    ~GdiSource();
    std::optional<Image> acquire();
    Statistics statistics() const noexcept;
    static Json describe(const Statistics &value);
    Json metrics() const;
};
} // namespace nss::capture
