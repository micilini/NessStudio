#include "runtime/time.hpp"
#include "runtime/buffers.hpp"
#include "json.hpp"
#include <atomic>
#include <iostream>
#include <limits>
#include <thread>
#include <set>
using namespace nss;
static void check(bool value, const char* text) { if (!value) throw std::runtime_error(text); }
template<class F> static void rejects(F operation) { bool rejected = false; try { operation(); } catch (const std::exception&) { rejected = true; } check(rejected, "Invalid input was accepted"); }
int main(int argc, char** argv) {
    try {
        const std::string scope = argc > 1 ? argv[1] : "time";
        nlohmann::json report{{"status", "PASS"}, {"scope", scope}, {"synthetic", true}};
        if (scope == "time") {
            uint64_t checks = 0;
            for (const auto [num, den] : {std::pair{24u,1u}, {25u,1u}, {30u,1u}, {48u,1u}, {50u,1u}, {60u,1u}, {30000u,1001u}}) {
                int64_t previous = -1;
                for (uint64_t i = 0; i < 2000000; ++i) {
                    const auto expected = (i / num) * 10000000ULL * den + ((i % num) * 10000000ULL * den) / num;
                    const auto actual = frame_time(i, num, den);
                    check(actual == static_cast<int64_t>(expected) && actual > previous, "Rational frame clock drifted"); previous = actual; ++checks;
                }
            }
            for (const auto rate : {8000u,16000u,44100u,48000u,96000u,192000u}) {
                for (uint64_t i = 0; i < 2000000; ++i) { const auto expected = (i / rate) * 10000000ULL + (i % rate) * 10000000ULL / rate; check(sample_time(i, rate) == static_cast<int64_t>(expected), "Sample clock drifted"); ++checks; }
                check(sample_time(static_cast<uint64_t>(rate) * 8 * 3600, rate) == 8LL * 3600 * hns_per_second, "Eight-hour sample endpoint drifted");
            }
            check(scale_floor(UINT64_MAX, UINT64_MAX, UINT64_MAX) == UINT64_MAX, "Wide intermediate overflowed");
            rejects([] { (void)scale_floor(UINT64_MAX, 2, 1); }); rejects([] { (void)scale_floor(1, 1, 0); });
            rejects([] { (void)frame_time(1, 0, 1); }); rejects([] { (void)frame_time(3000000, 30, 1); });
            rejects([] { (void)sample_time(1, 7999); }); rejects([] { (void)sample_time(1, 192001); });
            TimestampAdapter qpc{TimeOrigin::QpcTicks, 3000000, {}, false, 0};
            TimestampAdapter wasapi{TimeOrigin::WasapiQpcHns, 3000000, {}, false, 0};
            check(qpc.convert(3000000) == 10000000 && wasapi.convert(3000000) == 3000000, "WASAPI QPC was reconverted as raw ticks");
            rejects([] { (void)TimestampAdapter{TimeOrigin::DeviceHns, 0, {}, false, 0}.convert(10); });
            check(TimestampAdapter{TimeOrigin::DeviceHns,0,100,false,0}.convert(20) == 120, "Device correlation was ignored");
            rejects([] { (void)TimestampAdapter{TimeOrigin::DeviceHns,0,INT64_MIN,false,0}.convert(20); });
            report["indices_verified"] = checks; report["eight_hour_math_only"] = true;
        } else if (scope == "pause") {
            // Reproduce a worker sampling QPC before a concurrent Pause/Resume
            // obtains the clock lock. Historical reads must not fail a session.
            SessionClock sampled; sampled.start(10000000); sampled.pause(11000000);
            check(sampled.active_time(10500000)==500000, "A read racing Pause rejected its previously sampled timestamp");
            sampled.resume(12000000);
            check(sampled.active_time(11500000)==1000000 && sampled.active_time(9000000)==0,
                "Historical paused/pre-start duration was changed by Resume");
            sampled.stop(13000000);
            check(sampled.active_time(12500000)==1500000 && sampled.active_time(14000000)==2000000,
                "Historical/final duration changed after Stop");
            SessionClock ordered; ordered.start(10000000); ordered.pause(11000000);
            rejects([&] {ordered.resume(10999999);});
            check(ordered.active_time(11000000)==1000000, "Rejected control changed the active cut");
            SessionClock clock; clock.start(10000000); clock.pause(11000000); clock.resume(12000000); clock.pause(13000000); clock.stop(14000000);
            check(clock.map(10500000) == 500000 && clock.map(12500000) == 1500000, "Late packet historical map changed");
            check(!clock.map(11000000) && !clock.map(11500000) && !clock.map(13000000), "Paused timestamp was admitted");
            check(clock.active_time(15000000) == 2000000 && clock.wall_time(15000000) == 4000000, "Stop in pause added duration");
            check(!clock.eligible(10999999, 11000001) && clock.eligible(10999990, 11000000), "Acquisition crossing cut was admitted");
            for (const auto rate : {8000u, 16000u, 44100u, 48000u, 96000u, 192000u}) {
                const int64_t begin = 10950003; const uint64_t count = rate / 5;
                const auto slices = clock.trim_audio(begin, count, rate); std::set<uint64_t> kept;
                for (const auto& slice : slices) for (uint64_t i = 0; i < slice.count; ++i) check(kept.insert(slice.first + i).second, "An audio sample was duplicated");
                for (uint64_t i = 0; i < count; ++i) check(kept.contains(i) == clock.map(begin + sample_time(i, rate)).has_value(), "Audio inclusion at pause edge is incorrect");
            }
            SessionClock video; video.start(0); EligibleVideoCache cache;
            check(cache.admit(video, {1, 10, 10, false}), "Initial frame rejected"); video.pause(20);
            check(!cache.admit(video, {999, 25, 25, false}), "Paused marker polluted eligible cache"); video.resume(40);
            check(cache.select(false)->id == 1 && cache.forced_repeats == 1, "Delayed resume reused paused content");
            check(cache.admit(video, {2,45,45,true}) && cache.select(false)->id == 2 && cache.reused_static == 1, "Static reuse not distinguished from forced repeat");
            check(!cache.admit(video, {3,15,15,false}), "Late old frame replaced newer resumed content");
            CfrScheduler scheduler(30000,1001); uint64_t slots=0;
            while (const auto slot=scheduler.next(10000000,cache,false)) { check(slot->index==slots++ && slot->frame_id==2 && slot->pts==frame_time(slot->index,30000,1001),"CFR slots were driven by callback arrival or stale content"); }
            check(slots==30 && scheduler.expected_slots==30 && scheduler.reused==30 && scheduler.forced==0,"Scheduler slot denominator/accounting");
            report["late_packet_and_audio_edges"] = true; report["paused_marker_rejected"] = true;
        } else if (scope == "queues") {
            BoundedQueue<uint64_t> queue(3, 24, 300, PressurePolicy::DropOldest);
            for (uint64_t i = 0; i < 1000000; ++i) check(queue.push(i, 8, 100, static_cast<int64_t>(i)), "Drop-oldest push rejected");
            auto stats = queue.stats(); check(stats.count == 3 && stats.peak_bytes == 24 && stats.dropped_old == 999997, "Capture pressure counters mismatch");
            BoundedQueue<int> encoder(1, 8, 10, PressurePolicy::DropNewest);
            check(encoder.push(1,8,10,0) && !encoder.push(2,8,10,1) && encoder.saturation_exceeded(20000001), "Encoder saturation not detected");
            check(encoder.pop() == 1 && encoder.push(3,8,10,20000002) && !encoder.saturation_exceeded(40000002), "Recovered queue still reported saturation");
            BoundedQueue<int> packets(16, 16 * 1024 * 1024, 20000000, PressurePolicy::Fail);
            check(!packets.push(1, 16 * 1024 * 1024 + 1,1,0) && !packets.push(1,1,20000001,0), "Compressed packet budget ignored");
            BoundedQueue<int> audio(16, 192000, 2000000, PressurePolicy::Fail);
            check(!audio.push(1,1,2000001,0), "Audio 200 ms budget ignored");
            BoundedQueue<uint64_t> concurrent(8,64,800,PressurePolicy::DropOldest); std::atomic<bool> finished{};
            std::thread producer([&] { for (uint64_t i=0;i<200000;++i) concurrent.push(i,8,100,static_cast<int64_t>(i)); finished=true; });
            std::thread consumer([&] { while (!finished || concurrent.stats().count) { (void)concurrent.pop(); std::this_thread::yield(); } });
            producer.join(); consumer.join(); const auto raced=concurrent.stats();
            check(raced.count==0 && raced.pushed==raced.popped+raced.dropped_old && raced.peak_bytes<=64, "Concurrent bounded queue lost accounting");
            report["capture_items"] = 1000000; report["concurrent_items"] = 200000; report["capture_dropped"] = stats.dropped_old;
        } else if (scope == "ownership") {
            std::optional<FramePool::Lease> retained;
            {
                FramePool pool(2,384); auto a=pool.acquire(), b=pool.acquire(); check(a && b && !pool.acquire(), "Pool did not honor slot budget");
                a->writable().front()=std::byte{42}; a->seal(); rejects([&] { (void)a->writable(); });
                retained=std::move(a); b.reset(); check(pool.stats().released==1, "Lease release not counted");
            }
            check(retained->view().front()==std::byte{42} && live_pool_bytes==768 && live_frame_leases==1, "Borrowed frame did not retain its pool");
            retained.reset(); check(live_pool_bytes==0 && live_frame_leases==0, "CPU frame ownership leaked");
            rejects([] { FramePool invalid(9,1); }); rejects([] { (void)FramePool::nv12_bytes(17,16,17); });
            rejects([] { FramePool oversized(8, 64*1024*1024); });
            report["immutable_lease_survives_pool_owner"] = true; report["gpu_device_created"] = false;
        } else throw std::invalid_argument("Unknown test scope");
        std::cout << report.dump() << '\n'; return 0;
    } catch(const std::exception& error) { std::cout << nlohmann::json{{"status","FAIL"},{"message",error.what()}}.dump() << '\n'; return 1; }
}
