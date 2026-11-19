#include "audio/audio.hpp"
#include "audio/sinc_kernel.hpp"
#include "audio/packet_queue.hpp"
#include "audio/clock_rate.hpp"
#include <algorithm>
#include <cstdint>
#include <iostream>
#include <semaphore>
namespace nss::audio {
Json qualify(const std::string &, const std::string &);
}
static nss::Json handoff_quality() {
  using namespace nss::audio;
  const auto check = [](bool value, const char *message) {
    if (!value) throw std::runtime_error(message);
  };
  PacketQueue queue(48000, 8);
  std::counting_semaphore<8> credits(8);
  uint64_t received{};
  PacketProcessor processor(queue, 9600 * 8,
      [&](const std::vector<uint8_t> &bytes, const PacketQueue::Packet &packet) {
        // Return credit on failure too: the producer must be able to observe
        // the worker's original exception instead of waiting forever.
        struct Credit { std::counting_semaphore<8> &value; ~Credit() { value.release(); } } credit{credits};
        check(packet.position == received && packet.qpc == 1000000 + received * 10000,
              "Audio queue changed packet identity or timestamp");
        check(packet.flags == (received % 7 == 0 ? 2u : 0u), "Audio queue changed flags");
        if (!packet.silent)
          for (size_t i = 0; i < bytes.size(); ++i)
            check(bytes[i] == uint8_t((received * 17 + i) & 255),
                  "Audio queue reused borrowed input or corrupted a wrapped payload");
        ++received;
      });
  std::vector<uint8_t> borrowed;
  for (uint64_t i = 0; i < 20000; ++i) {
    credits.acquire();
    queue.check();
    const auto frames = uint32_t(127 + i % 337);
    borrowed.resize(size_t(frames) * 8);
    for (size_t j = 0; j < borrowed.size(); ++j) borrowed[j] = uint8_t((i * 17 + j) & 255);
    const bool silent = i % 7 == 0;
    queue.push(silent ? nullptr : borrowed.data(), {frames, silent ? 2u : 0u,
        i, 1000000 + i * 10000, 0, 0, 0, silent});
    std::fill(borrowed.begin(), borrowed.end(), uint8_t{0xCC});
  }
  processor.drain();
  check(received == 20000 && queue.stats().queued_frames == 0, "Audio queue did not drain every packet");
  PacketQueue saturated(8000, 2);
  std::vector<uint8_t> input(800 * 2, uint8_t{0x57}), output;
  for (uint64_t i = 0; i < 5; ++i) saturated.push(input.data(), {800, 0, i});
  bool rejected{};
  try { saturated.push(input.data(), {800, 0, 5}); }
  catch (const nss::capture::Error &e) { rejected = e.code == NSS_E_BACKPRESSURE; }
  check(rejected && saturated.stats().queued_frames == 4000 && saturated.stats().pushed == 5,
        "Audio overload was not bounded or evicted a mandatory packet");
  saturated.close();
  PacketQueue::Packet packet;
  for (uint64_t i = 0; i < 5; ++i) {
    check(saturated.pop(output, packet) && packet.position == i && output == input,
          "Audio overload altered retained packets");
  }
  check(!saturated.pop(output, packet), "Closed audio queue did not terminate");
  PacketQueue failed(48000, 8);
  PacketProcessor faulted(failed, 76800, [](const auto &, const auto &) {
    throw nss::capture::Error(NSS_E_IO, NSS_STAGE_STORAGE, 1234, "owned writer failure");
  });
  failed.push(nullptr, {480, 2, 0, 10000, 0, 0, 0, true});
  bool causal{};
  try { faulted.drain(); }
  catch (const nss::capture::Error &e) {
    causal = e.code == NSS_E_IO && e.stage == NSS_STAGE_STORAGE && e.native == 1234;
  }
  check(causal, "Audio worker failure lost its causal stage/native code");
  PacketQueue burst(48000, 8, PacketQueue::recording_pending_ms);
  std::binary_semaphore began(0), resume(0);
  uint64_t burst_received{};
  PacketProcessor stalled(burst, 480 * 8,
      [&](const std::vector<uint8_t> &bytes, const PacketQueue::Packet &input) {
        if (!burst_received) { began.release(); resume.acquire(); }
        check(input.position == burst_received * 480 && input.qpc == 1000000 + burst_received * 100000,
              "Storage backlog changed source timestamps or position");
        check(bytes.size() == 480 * 8 &&
              std::all_of(bytes.begin(), bytes.end(), [&](uint8_t byte) { return byte == uint8_t(burst_received); }),
              "Storage backlog changed or reused retained PCM");
        ++burst_received;
      });
  struct ResumeOnExit {
    std::binary_semaphore &signal;
    bool pending = true;
    void release() { if (pending) { pending = false; signal.release(); } }
    ~ResumeOnExit() { release(); }
  } unblock{resume};
  // Deterministically retain 1.5 seconds behind a blocked file consumer. This
  // exceeds the old 500ms limit while remaining inside the recording budget.
  std::vector<uint8_t> burst_bytes(480 * 8);
  for (uint64_t i = 0; i < 151; ++i) {
    std::fill(burst_bytes.begin(), burst_bytes.end(), uint8_t(i));
    burst.push(burst_bytes.data(), {480, 0, i * 480, 1000000 + i * 100000});
    if (!i) began.acquire();
    std::fill(burst_bytes.begin(), burst_bytes.end(), uint8_t{0xCE});
  }
  check(burst.stats().queued_frames == 150 * 480 &&
        burst.stats().peak_frames <= uint64_t(48000) * PacketQueue::recording_pending_ms / 1000,
        "Storage burst was not retained within its bounded allocation");
  unblock.release(); stalled.drain();
  check(burst_received == 151 && burst.stats().queued_frames == 0,
        "Storage burst did not drain every original packet");
  PacketQueue persistent(48000, 8, PacketQueue::recording_pending_ms);
  for (uint64_t i = 0; i < 500; ++i) persistent.push(burst_bytes.data(), {480, 0, i * 480});
  bool persistent_rejected{};
  try { persistent.push(burst_bytes.data(), {480, 0, 500 * 480}); }
  catch (const nss::capture::Error &e) { persistent_rejected = e.code == NSS_E_BACKPRESSURE; }
  check(persistent_rejected && persistent.stats().queued_frames == 48000 * 5,
        "Persistent storage overload became unbounded or evicted retained packets");
  return {{"status", "PASS"}, {"packetsVerified", received}, {"borrowedInputOverwritten", true},
          {"ringWrapVerified", true}, {"silenceFlagsPreserved", true},
          {"boundedOverloadWithoutEviction", true}, {"causalFailurePropagated", true},
          {"queueBytes", queue.stats().bytes}, {"peakQueuedFrames", queue.stats().peak_frames},
          {"storageBurstOriginalPacketsVerified", burst_received}, {"storageBurstSeconds", 1.5},
          {"recordingQueueBytes", burst.stats().bytes}, {"recordingBudgetMs", PacketQueue::recording_pending_ms},
          {"persistentOverloadStillRejected", true}};
}
static nss::Json kernel_quality() {
  double maximum_coefficient_error{}, maximum_normalized_l1{},
      maximum_actual_gain{}, maximum_certified_gain{}, maximum_quiet_output_lsb{};
  uint64_t phases{};
  // A prime grid also exercises phases between power-of-two table anchors.
  constexpr size_t phase_denominator = 10007;
  const std::array edge_phases{1e-15, 1e-12, 1e-10, 1e-8, 1e-5,
      .5 - 1e-12, .5 + 1e-12, 1.0 - 1e-5, 1.0 - 1e-8,
      1.0 - 1e-10, 1.0 - 5e-12, 1.0 - 2e-12, 1.0 - 1e-12,
      std::nextafter(1.0, 0.0)};
  for (const unsigned rate : {8000u, 11025u, 16000u, 22050u, 32000u,
                               44100u, 48000u, 88200u, 96000u, 192000u}) {
    const double cutoff = (std::min)(1.0, 48000.0 / rate) * .94;
    const nss::audio::SincKernel kernel(cutoff);
    if (!std::isfinite(kernel.maximum_normalized_gain()) ||
        !(kernel.pcm16_zero_input_limit() > 0))
      throw std::runtime_error("PCM16 zero proof needs a finite positive bound");
    maximum_certified_gain = (std::max)(maximum_certified_gain, kernel.maximum_normalized_gain());
    for (const uint64_t offset : {uint64_t{0}, uint64_t{48000} * 3600 * 8,
                                  uint64_t{192000} * 3600 * 24}) {
      for (size_t phase = 0; phase < phase_denominator + 1 + edge_phases.size(); ++phase) {
        const double fraction = phase <= phase_denominator ? double(phase) / double(phase_denominator)
                                             : edge_phases[phase - phase_denominator - 1];
        const double position = double(offset) + fraction;
        const auto center = static_cast<int64_t>(std::floor(position));
        const auto fast = kernel.weights(position - double(center));
        std::array<double, 32> reference{};
        double fast_sum{}, reference_sum{};
        for (int tap = -15; tap <= 16; ++tap) {
          // Independent analytical reference: the original filter equation,
          // including its absolute sample-index subtraction at long durations.
          const double d = double(center + tap) - position;
          const double x = d * cutoff;
          const double sinc = std::abs(x) < 1e-12 ? 1.0
              : std::sin(std::numbers::pi * x) / (std::numbers::pi * x);
          const double window = .42 + .5 * std::cos(std::numbers::pi * d / 16.0)
                                    + .08 * std::cos(2 * std::numbers::pi * d / 16.0);
          const auto index = static_cast<size_t>(tap + 15);
          reference[index] = sinc * window * cutoff;
          fast_sum += fast[index];
          reference_sum += reference[index];
          maximum_coefficient_error = (std::max)(maximum_coefficient_error,
              std::abs(fast[index] - reference[index]));
        }
        if (!std::isfinite(fast_sum) || !std::isfinite(reference_sum) ||
            std::abs(fast_sum) < 1e-12 || std::abs(reference_sum) < 1e-12)
          throw std::runtime_error("Sinc normalization must remain finite and nonzero");
        double normalized_l1{}, fast_gain{}, reference_gain{};
        for (size_t index = 0; index < fast.size(); ++index) {
          normalized_l1 += std::abs(fast[index] / fast_sum - reference[index] / reference_sum);
          fast_gain += std::abs(fast[index] / fast_sum);
          reference_gain += std::abs(reference[index] / reference_sum);
        }
        const auto actual_gain = (std::max)(fast_gain, reference_gain);
        if (actual_gain > kernel.maximum_normalized_gain())
          throw std::runtime_error("Sinc L1 gain exceeded the certified quiet-input bound");
        // L1 is attained by adversarial signs of bounded input, and bounds
        // every other arrangement, including clamped edge samples.
        const auto quiet_output_lsb = kernel.pcm16_zero_input_limit() * actual_gain * 32768;
        if (std::lround(quiet_output_lsb) != 0 || quiet_output_lsb > .25)
          throw std::runtime_error("Quiet-input fast path can change PCM16 output");
        maximum_actual_gain = (std::max)(maximum_actual_gain, actual_gain);
        maximum_quiet_output_lsb = (std::max)(maximum_quiet_output_lsb, quiet_output_lsb);
        maximum_normalized_l1 = (std::max)(maximum_normalized_l1, normalized_l1);
        ++phases;
      }
    }
  }
  // L1 bounds the output difference for every unit-bounded input window,
  // including impulses, full-scale transients and arbitrary stereo signals.
  if (maximum_coefficient_error > 1e-12 || maximum_normalized_l1 > 1e-11)
    throw std::runtime_error("Optimized sinc differs from the original analytical filter");
  return {{"status", "PASS"}, {"phasesCompared", phases}, {"ratesCompared", 10},
          {"phaseGridDenominator", phase_denominator},
          {"extremeFractionalPhasesCompared", edge_phases.size() * 30},
          {"maximumCoefficientError", maximum_coefficient_error},
          {"maximumNormalizedL1Error", maximum_normalized_l1},
          {"maximumPcm16PrequantizationErrorLsb", maximum_normalized_l1 * 32768},
          {"maximumActualNormalizedL1Gain", maximum_actual_gain},
          {"maximumCertifiedNormalizedL1Gain", maximum_certified_gain},
          {"maximumQuietOutputLsb", maximum_quiet_output_lsb},
          {"quietFastPathPcm16Equivalent", true},
          {"longestAbsoluteInputPositionHours", 24}, {"phaseQuantized", false}};
}
static nss::Json clock_rate_quality() {
  nss::Json cases=nss::Json::array();
  // Include a biased initial timestamp and deterministic scheduling jitter.
  // A clock based on that first anchor integrates a false rate over time.
  for(const double expected:{-150.,0.,150.,300.}) {
    nss::audio::ClockRate estimate;
    constexpr int64_t epoch=8000000000000000LL;
    for(uint64_t packet=0;packet<=15000;++packet) {
      const uint64_t position=packet*480;
      const auto ideal=int64_t(std::llround(double(position)*10000000/(48000*(1+expected*1e-6))));
      const auto jitter=packet==0?-1000:int64_t(packet%11)*200-1000;
      estimate.observe(position,epoch+ideal+jitter,48000);
    }
    const auto measured=estimate.ppm();
    if(!measured || std::abs(*measured-expected)>.15 || estimate.observations()>640 || estimate.span_seconds()>30)
      throw std::runtime_error("Bounded audio clock regression failed under first-anchor bias/jitter");
    cases.push_back({{"expectedPpm",expected},{"measuredPpm",*measured},{"observations",estimate.observations()}});
    estimate.reset();
    if(estimate.ppm() || estimate.observations()) throw std::runtime_error("Idle-gap reset retained old clock history");
  }
  nss::Json long_cases=nss::Json::array();
  for(const double expected:{-150.,-20.,0.,150.}) {
    nss::audio::ClockRate long_clock;
    double maximum_error{};
    for(uint64_t sample=0;sample<=864000;++sample) {
      const auto qpc=1000000000000LL+int64_t(std::llround(double(sample)*1000000/(1+expected*1e-6)));
      long_clock.observe(sample*4800,qpc,48000);
      if(sample>=600 && sample%600==0) {
        if(!long_clock.ppm()) throw std::runtime_error("Long-running clock lost its estimate");
        maximum_error=(std::max)(maximum_error,std::abs(*long_clock.ppm()-expected));
      }
    }
    long_cases.push_back({{"expectedPpm",expected},{"maximumErrorPpm",maximum_error},{"finalPpm",*long_clock.ppm()}});
    if(maximum_error>.15)
      throw std::runtime_error("Rolling audio clock accumulated numerical drift across 24 hours: "+long_cases.dump());
  }
  return {{"status","PASS"},{"cases",cases},{"longCases",long_cases},{"simulatedHours",24},{"actualClockCorrectionBudgetPpm",200}};
}
int main(int argc, char **argv) {
  try {
    if (argc != 3)
      throw std::invalid_argument("Expected scope and artifacts directory");
    std::cout << (std::string(argv[1]) == "kernel" ? kernel_quality()
                    : std::string(argv[1]) == "handoff" ? handoff_quality()
                    : std::string(argv[1]) == "clock-rate" ? clock_rate_quality()
                    : nss::audio::qualify(argv[1], argv[2])).dump() << '\n';
    return 0;
  } catch (const std::exception &e) {
    std::cout << nss::Json{{"status", "FAIL"}, {"message", e.what()}}.dump()
              << '\n';
    return 1;
  }
}
