#include "capture/capture.hpp"
#include "capture/acquisition_window.hpp"
#include <array>
#include <iostream>
#include <limits>
using namespace nss::capture;
static void check(bool value, const char *text) {
    if (!value)
        throw std::runtime_error(text);
}
int main(int argc, char **argv) {
    try {
        const std::string scope = argc > 1 ? argv[1] : "geometry";
        if (scope == "acquisition-window") {
            for (const auto cadence : {std::pair{24U, 1U}, std::pair{25U, 1U}, std::pair{30U, 1U},
                                       std::pair{48U, 1U}, std::pair{50U, 1U}, std::pair{60U, 1U},
                                       std::pair{30000U, 1001U}}) {
                WindowSampling sampling;
                for (uint64_t i = 0; i < 1000; ++i) {
                    const auto begin = nss::frame_time(i, cadence.first, cadence.second);
                    const auto end = nss::frame_time(i + 1, cadence.first, cadence.second);
                    const auto window = acquisition_window(begin, cadence.first, cadence.second);
                    check(window.index == i && window.begin == begin && window.end == end &&
                              window.due > begin && window.due < end &&
                              acquisition_window(end - 1, cadence.first, cadence.second).index == i &&
                              acquisition_window(end, cadence.first, cadence.second).index == i + 1,
                          "Acquisition windows drifted or misclassified a rational boundary");
                    check(!sampling.due(window, window.due - 1) && sampling.due(window, window.due),
                          "Acquisition started outside its scheduled output window");
                    check(sampling.minimum_source_time(window) ==
                              (i ? std::optional<int64_t>{begin} : std::nullopt),
                          "Copy filter did not follow the last served rational window");
                    check(sampling.idle_until(window, window.begin) == window.due &&
                              sampling.idle_until(window, window.due - 1) == window.due &&
                              !sampling.idle_until(window, window.due),
                          "Idle wait crossed a required sample or enabled polling before its deadline");
                    check(!sampling.observe(window, begin) && !sampling.observe(window, end + 1) &&
                              sampling.due(window, window.due + 1),
                          "Stale or future acquisition disabled the required retry");
                    check(sampling.observe(window, end) && !sampling.due(window, end),
                          "One valid acquisition did not suppress redundant polling");
                    check(sampling.idle_until(window, window.due) == window.end &&
                              sampling.idle_until(window, window.end - 1) == window.end &&
                              !sampling.idle_until(window, window.end) &&
                              !sampling.idle_until(window, window.begin - 1),
                          "Served-window wait escaped its rational window");
                    auto resumed = sampling;
                    resumed.reset();
                    check(resumed.due(window, window.due) && !resumed.minimum_source_time(window),
                          "Resume could not refresh a partially recorded window");
                }
                // Model a source presentation just before a boundary whose
                // acquisition is only available after the owner's next wake.
                // This recovery must precede the next window's normal sample,
                // while a successfully sampled previous window stays paced.
                WindowSampling recovery;
                const auto initial = acquisition_window(0, cadence.first, cadence.second);
                check(!recovery.due(initial, initial.begin) &&
                          recovery.observe(initial, initial.end),
                      "Fresh initial sampling bypassed its normal due time");
                const auto missed = acquisition_window(initial.end, cadence.first, cadence.second);
                check(!recovery.due(missed, missed.begin) && recovery.due(missed, missed.due),
                      "A served preceding window caused redundant early polling");
                check(!recovery.observe(missed, missed.begin),
                      "Stale source incorrectly served the missed window");
                const auto following = acquisition_window(missed.end, cadence.first, cadence.second);
                check(recovery.due(following, following.begin) &&
                          !recovery.minimum_source_time(following) &&
                          !recovery.observe(following, missed.end - 1) &&
                          recovery.due(following, following.begin + 1) &&
                          !recovery.idle_until(following, following.begin) &&
                          !recovery.idle_until(following, following.begin + 1),
                      "A late eligible image was blocked by the next window's sampling delay");
                check(recovery.observe(following, following.begin + 1) &&
                          !recovery.due(following, following.due),
                      "Recovery failed to return to one acquisition per served window");
                recovery.reset();
                check(recovery.due(following, following.begin) &&
                          !recovery.minimum_source_time(following) &&
                          !recovery.idle_until(following, following.begin),
                      "Resume suppressed recovery of an unserved partial window");
                const auto tail = acquisition_window(nss::maximum_session_hns - 1,
                                                       cadence.first, cadence.second);
                check(tail.begin < nss::maximum_session_hns && tail.end >= nss::maximum_session_hns,
                      "Final partial acquisition window overflowed the session domain");
            }
            WindowSampling sampling;
            const auto first = acquisition_window(0, 30, 1);
            check(sampling.observe(first, first.end), "Initial window was not admitted");
            const auto later = acquisition_window(nss::hns_per_second, 30, 1);
            check(sampling.due(later, later.due), "A delayed owner attempted to replay missed windows");
            check(!sampling.minimum_source_time(later), "A delayed owner filtered a recoverable image");
            nss::SessionClock clock;
            clock.start(10000000);
            clock.pause(10100000);
            check(!clock.map(10100000), "Closed cut did not bypass acquisition pacing");
            clock.resume(20000000);
            sampling.reset();
            const auto active = clock.map(20050000);
            check(active && *active == 150000 &&
                      acquisition_window(*active, 30, 1).index == 0,
                  "Acquisition cadence counted paused wall time");
        } else if (scope == "geometry") {
            const auto rejected_api = [](const Json &request, nss_result expected) {
                bool rejected{};
                try { (void)plan(request); }
                catch (const Error &error) { rejected = error.code == expected; }
                check(rejected, "Invalid or unsupported explicit capture API was accepted");
            };
            rejected_api({{"version", 1}, {"sourceApi", "Unknown"}}, NSS_E_ARGUMENT);
            rejected_api({{"version", 1}, {"mode", "FullSoftware"},
                          {"sourceApi", "DesktopDuplication"}, {"cursor", false}}, NSS_E_ARGUMENT);
            rejected_api({{"version", 1}, {"sourceApi", "DesktopDuplication"}}, NSS_E_UNAVAILABLE);
            const auto crop = local_crop({-1920, -200, 1920, 1200}, {-1800, -100, 333, 241}, true);
            check(crop.x == 120 && crop.y == 100 && crop.width == 333 && crop.height == 241,
                  "Negative global origin changed physical selection");
            for (const auto selection :
                 {Rect{-1921, 0, 2, 2}, Rect{-1, 0, 2, 2}, Rect{0, 0, 2147483647, 2}, Rect{0, 0, 0, 2}}) {
                bool rejected = false;
                try {
                    (void)local_crop({-1920, -200, 1920, 1200}, selection, true);
                } catch (const Error &e) {
                    rejected = e.code == NSS_E_ARGUMENT;
                }
                check(rejected, "Invalid crop was clamped or overflowed");
            }
            for (uint32_t sw = 1; sw < 101; ++sw)
                for (uint32_t sh = 1; sh < 101; ++sh) {
                    try {
                        const auto v = fit_viewport(sw, sh, 334, 242);
                        check(v.x >= 0 && v.y >= 0 && v.width > 0 && v.height > 0 && v.x + v.width <= 334 &&
                                  v.y + v.height <= 242 && !((v.x | v.y | v.width | v.height) & 1),
                              "Viewport escaped output");
                    } catch (const Error &e) {
                        check(e.code == NSS_E_ARGUMENT, "Unexpected geometry failure");
                    }
                }
        } else if (scope == "cpu-scene") {
            constexpr uint32_t width = 40, height = 30, stride = 176;
            std::vector<uint8_t> bgra(stride * height, 0xa5), output(34 * 24 * 3 / 2);
            const auto color = [&](uint8_t b, uint8_t g, uint8_t r) {
                for (uint32_t y = 0; y < height; ++y)
                    for (uint32_t x = 0; x < width; ++x) {
                        auto *p = bgra.data() + size_t(y) * stride + x * 4;
                        p[0] = b;
                        p[1] = g;
                        p[2] = r;
                        p[3] = 255;
                    }
            };
            BgraView view{bgra.data(), width, height, stride, bgra.size()};
            const std::array<std::array<uint8_t, 6>, 5> bars = {{{0, 0, 0, 16, 128, 128},
                                                                 {255, 255, 255, 235, 128, 128},
                                                                 {0, 0, 255, 63, 102, 240},
                                                                 {0, 255, 0, 173, 42, 26},
                                                                 {255, 0, 0, 32, 240, 118}}};
            for (const auto &bar : bars) {
                color(bar[0], bar[1], bar[2]);
                process_cpu(view, {3, 5, 33, 23}, {0, 0, 34, 24}, 34, 24, output);
                check(abs(int(output[0]) - bar[3]) <= 1 && abs(int(output[34 * 24]) - bar[4]) <= 1 &&
                          abs(int(output[34 * 24 + 1]) - bar[5]) <= 1,
                      "BT709 range/matrix/channel mismatch");
            }
            color(255, 0, 255);
            for (uint32_t y = 5; y < 28; ++y)
                for (uint32_t x = 3; x < 36; ++x) {
                    auto *p = bgra.data() + size_t(y) * stride + x * 4;
                    p[0] = p[1] = p[2] = 255;
                }
            const auto viewport = fit_viewport(33, 23, 34, 24);
            process_cpu(view, {3, 5, 33, 23}, viewport, 34, 24, output);
            for (size_t i = 34 * 24; i < output.size(); ++i)
                check(abs(int(output[i]) - 128) <= 1, "Crop filter sampled the magenta outside marker");
            color(0, 0, 0);
            std::vector<uint8_t> overlay(8 * 8 * 4, 255);
            for (size_t i = 0; i < overlay.size(); i += 4) {
                overlay[i] = 0;
                overlay[i + 1] = 0;
                overlay[i + 2] = 255;
                overlay[i + 3] = 128;
            }
            const Layer layer{{overlay.data(), 8, 8, 32, overlay.size()}, {18, 0, 16, 24}, 255, false};
            process_cpu(view, {0, 0, 40, 30}, {0, 0, 16, 24}, 34, 24, output, {&layer, 1});
            check(output[0] == 16 && abs(int(output[20]) - 39) <= 1, "Side-by-side or alpha scene failed");
            for (uint32_t y = 0; y < 8; ++y)
                for (uint32_t x = 0; x < 8; ++x) {
                    auto *p = overlay.data() + y * 32 + x * 4;
                    p[0] = p[1] = p[2] = x < 4 ? 0 : 255;
                    p[3] = 255;
                }
            auto mirror = layer;
            mirror.mirror = true;
            process_cpu(view, {0, 0, 40, 30}, {0, 0, 16, 24}, 34, 24, output, {&mirror, 1});
            check(output[18] == 235 && output[32] == 16, "Mirrored layer changed side or sampling");
            auto short_view = view;
            short_view.bytes = 1;
            bool rejected = false;
            try {
                process_cpu(short_view, {0, 0, 40, 30}, {0, 0, 34, 24}, 34, 24, output);
            } catch (const Error &) {
                rejected = true;
            }
            check(rejected, "Short BGRA plane accepted");
        } else if(scope == "identity") {
            // A transparent layer forces the independent general compositor
            // path, which must agree byte-for-byte with identity conversion.
            constexpr uint32_t source_width=42, source_height=32, stride=187;
            const std::array<uint8_t,16> transparent{};
            uint32_t random=9137;
            for(size_t alignment : {size_t{0},size_t{1},size_t{7},size_t{15}}) {
                std::vector<uint8_t> input(alignment+stride*source_height+32);
                for(auto &value:input) {random=random*1664525+1013904223;value=uint8_t(random>>24);}
                const auto original=input;
                const BgraView view{input.data()+alignment,source_width,source_height,stride,
                                    stride*(source_height-1)+source_width*4};
                for(uint32_t width=2;width<=34;width+=2)
                    for(uint32_t height : {2U,6U,22U})
                        for(uint32_t padding : {0U,2U}) {
                            const uint32_t w=width+2*padding,h=height+2*padding;
                            const size_t bytes=size_t(w)*h*3/2;
                            std::vector<uint8_t> guarded(bytes+19,0xa5),reference(bytes);
                            const Rect crop{3,5,int32_t(width),int32_t(height)},
                                viewport{int32_t(padding),int32_t(padding),int32_t(width),int32_t(height)};
                            const Layer layer{{transparent.data(),2,2,8,transparent.size()},
                                               {0,0,int32_t(w),int32_t(h)},0,false};
                            process_cpu(view,crop,viewport,w,h,{guarded.data()+3,bytes});
                            process_cpu(view,crop,viewport,w,h,reference,{&layer,1});
                            check(std::equal(reference.begin(),reference.end(),guarded.begin()+3),
                                  "Identity conversion changed pixels, crop, range or padding");
                            check(std::all_of(guarded.begin(),guarded.begin()+3,[](uint8_t b){return b==0xa5;}) &&
                                  std::all_of(guarded.begin()+3+bytes,guarded.end(),[](uint8_t b){return b==0xa5;}),
                                  "Identity conversion overwrote output guards");
                        }
                check(input==original,"Identity conversion modified its borrowed source");
            }
        } else if(scope == "pending-candidate") {
            const auto frame=[](uint64_t id,int64_t timestamp) {
                Frame value;value.id=id;value.capture_hns=timestamp;return value;
            };
            const auto map=[](int64_t timestamp)->std::optional<int64_t> {return timestamp;};
            const auto eligible=[](const Frame& value) {return value.id!=2;};
            auto pending=frame(1,100);
            std::deque<Frame> incoming{frame(2,110),frame(3,120),frame(4,151)};
            uint64_t decimated{};
            check(replace_repeated_candidate(pending,1,incoming,150,map,eligible,decimated),
                  "Cached repeated output ignored an already acquired same-window candidate");
            check(pending.id==3 && pending.capture_hns==120 && incoming.size()==1 &&
                  incoming.front().id==4 && decimated==1,"Replacement admitted a future/cut frame or lost future ownership");
            check(!replace_repeated_candidate(pending,3,incoming,150,map,eligible,decimated),
                  "Replacement borrowed pixels beyond the original output boundary");
            pending=frame(1,100);incoming={frame(2,110)};
            check(!replace_repeated_candidate(pending,1,incoming,150,map,eligible,decimated) && pending.id==1,
                  "Replacement admitted an acquisition from a closed active interval");
            incoming={frame(3,120)};
            check(!replace_repeated_candidate(pending,99,incoming,150,map,eligible,decimated) && incoming.size()==1,
                  "Replacement changed a pending frame that was not a repeat");
            incoming={frame(3,120)};incoming.front().presentation_hns=151;
            check(!replace_repeated_candidate(pending,1,incoming,150,map,eligible,decimated),
                  "Replacement ignored the program presentation clock");
        } else if (scope == "acquisition-interval") {
            const auto frame = [](uint64_t id, int64_t begin, int64_t end) {
                Frame value;
                value.id = id;
                value.begin_hns = begin;
                value.end_hns = end;
                value.capture_hns = begin + (end - begin) / 2;
                value.estimated = value.acquisition_interval_estimate = true;
                return value;
            };
            const auto map = [](int64_t at) -> std::optional<int64_t> { return at; };
            const auto eligible = [](const Frame&) { return true; };
            constexpr int64_t boundary = 1000000, period = 333333;
            const auto previous = frame(1, 500000, 833333);
            const auto next = frame(2, 840000, 1173333);
            auto pending = previous;
            std::deque<Frame> incoming{next, frame(3, 1173334, 1506667)};
            auto lead = replace_repeated_interval_candidate(pending, 1, incoming,
                boundary, period, map, eligible);
            check(lead && *lead == next.capture_hns - boundary && pending.id == 2 &&
                  pending.capture_hns == next.capture_hns && pending.begin_hns == next.begin_hns &&
                  pending.end_hns == next.end_hns && incoming.size() == 1 && incoming.front().id == 3,
                  "Interval selection changed timestamps, order or future ownership");
            const auto reject = [&](Frame candidate, auto clock, auto cut) {
                auto target = previous;
                std::deque<Frame> queue{candidate};
                check(!replace_repeated_interval_candidate(target, 1, queue, boundary,
                          period, clock, cut) && target.id == 1 && queue.size() == 1,
                      "Interval selection accepted an invalid clock, source, cut or interval");
            };
            auto invalid = next;
            invalid.acquisition_interval_estimate = false; reject(invalid, map, eligible);
            invalid = next; invalid.estimated = false; reject(invalid, map, eligible);
            invalid = next; invalid.presentation_hns = boundary; reject(invalid, map, eligible);
            invalid = next; invalid.id = 1; reject(invalid, map, eligible);
            invalid = next; invalid.capture_hns = invalid.end_hns + 1; reject(invalid, map, eligible);
            reject(frame(2, boundary + 1, boundary + period), map, eligible);
            reject(frame(2, boundary - 1, boundary + period + 3), map, eligible);
            reject(frame(2, boundary - period, boundary + period + 100), map, eligible);
            reject(frame(2, boundary - period, boundary), map, eligible);
            reject(next, [](int64_t) -> std::optional<int64_t> { return {}; }, eligible);
            reject(next, map, [](const Frame&) { return false; });
            // A capture crossing Pause/Stop remains inadmissible even though its
            // midpoint is close to a nominal output boundary.
            nss::SessionClock clock;
            clock.start(100);
            clock.pause(1150000);
            reject(next, [&](int64_t at) { return clock.map(at); },
                [&](const Frame& value) { return clock.eligible(value.begin_hns, value.end_hns); });
            pending = next;
            incoming = {frame(3, boundary - 100, boundary + 100)};
            check(!replace_repeated_interval_candidate(pending, 1, incoming, boundary,
                      period, map, eligible) && incoming.size() == 1,
                  "Interval selection replaced a distinct, already selected frame");
            // Equal-rate sampling with alternating midpoint jitter around every
            // boundary must retain each distinct input exactly once. Exercise
            // rational periods too, without changing either clock's timestamps.
            for (const auto cadence : {std::pair{24U, 1U}, std::pair{30U, 1U},
                                       std::pair{60U, 1U}, std::pair{30000U, 1001U}}) {
                auto last = frame(1, 1, 2);
                for (uint64_t i = 2; i < 1002; ++i) {
                    const auto edge = nss::frame_time(i, cadence.first, cadence.second);
                    const auto width = edge - nss::frame_time(i - 1, cadence.first, cadence.second);
                    const auto jitter = i % 2 ? -1000 : 1000;
                    const auto source = frame(i, edge + jitter - width / 2,
                                               edge + jitter + width / 2);
                    const auto prior = last.id;
                    std::deque<Frame> queued{source};
                    if (source.capture_hns <= edge) { last = source; queued.pop_front(); }
                    else check(replace_repeated_interval_candidate(last, prior, queued,
                                   edge, width, map, eligible).has_value(),
                               "Measured midpoint jitter caused an avoidable repeat");
                    check(last.id == i && last.capture_hns == source.capture_hns && queued.empty(),
                          "Equal-rate interval selection lost, repeated or retimed an acquisition");
                }
            }
        } else
            throw std::runtime_error("Unknown capture core scope");
        std::cout << nlohmann::json{{"status", "PASS"},
                                    {"scope", scope},
                                    {"hardwareTest", false},
                                    {"outsidePixelsIncluded", false}}
                         .dump()
                  << '\n';
        return 0;
    } catch (const std::exception &e) {
        std::cout << nlohmann::json{{"status", "FAIL"}, {"message", e.what()}}.dump() << '\n';
        return 1;
    }
}
