#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "audio.hpp"
#include "sinc_kernel.hpp"
#include "packet_queue.hpp"
#include "clock_rate.hpp"
#include "capture/capture.hpp"
#include <windows.h>
// clang-format off
#include <audioclient.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <ksmedia.h>
// clang-format on
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <future>
#include <limits>
#include <numbers>
#include <optional>
#include <utility>
#include <wrl/client.h>

namespace nss::audio {
namespace {
using capture::Error;
using Microsoft::WRL::ComPtr;
void hr(HRESULT value, const char *text) {
  if (FAILED(value))
    throw Error(NSS_E_IO, NSS_STAGE_AUDIO, value, text);
}
std::wstring wide(const std::string &s) {
  if (s.empty() || s.size() > 32760 || s.find('\0') != std::string::npos)
    throw std::invalid_argument("Invalid audio path/device ID");
  const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(),
                                    static_cast<int>(s.size()), nullptr, 0);
  if (!n)
    throw std::invalid_argument("Invalid UTF8");
  std::wstring result(static_cast<size_t>(n), 0);
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(),
                      static_cast<int>(s.size()), result.data(), n);
  return result;
}
std::string utf8(const wchar_t *s) {
  const int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s, -1,
                                    nullptr, 0, nullptr, nullptr);
  if (!n)
    throw std::invalid_argument("Invalid endpoint name");
  std::string result(static_cast<size_t>(n), 0);
  WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s, -1, result.data(), n,
                      nullptr, nullptr);
  result.pop_back();
  return result;
}
struct Apartment {
  Apartment() {
    hr(CoInitializeEx(nullptr, COINIT_MULTITHREADED),
       "Audio MTA initialization failed");
  }
  ~Apartment() { CoUninitialize(); }
};
struct Format {
  uint32_t rate{}, channels{}, bits{}, align{}, mask{}, valid_bits{};
  bool floating{};
  explicit Format(const WAVEFORMATEX *f) {
    rate = f->nSamplesPerSec;
    channels = f->nChannels;
    bits = f->wBitsPerSample;
    align = f->nBlockAlign;
    valid_bits = bits;
    auto tag = f->wFormatTag;
    if (tag == WAVE_FORMAT_EXTENSIBLE && f->cbSize >= 22) {
      const auto *e = reinterpret_cast<const WAVEFORMATEXTENSIBLE *>(f);
      mask = e->dwChannelMask;
      valid_bits = e->Samples.wValidBitsPerSample;
      if (IsEqualGUID(e->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT))
        tag = WAVE_FORMAT_IEEE_FLOAT;
      else if (IsEqualGUID(e->SubFormat, KSDATAFORMAT_SUBTYPE_PCM))
        tag = WAVE_FORMAT_PCM;
    }
    floating = tag == WAVE_FORMAT_IEEE_FLOAT;
    if (rate < 8000 || rate > 192000 || channels < 1 || channels > 2 ||
        align != channels * (bits / 8) ||
        (floating ? bits != 32
                  : (tag != WAVE_FORMAT_PCM ||
                     (bits != 16 && bits != 24 && bits != 32))))
      throw Error(
          NSS_E_UNAVAILABLE, NSS_STAGE_AUDIO, 0,
          "Endpoint requires PCM16/24/32 or float32, mono/stereo, 8-192 kHz");
  }
  Json json() const {
    return {{"sampleRate", rate},
            {"channels", channels},
            {"bitsPerSample", bits},
            {"validBits", valid_bits},
            {"blockAlign", align},
            {"channelMask", mask},
            {"sampleType", floating ? "float" : "signed_pcm"}};
  }
  float read(const uint8_t *p) const {
    if (floating) {
      float v;
      std::memcpy(&v, p, 4);
      return std::isfinite(v) ? std::clamp(v, -1.f, 1.f) : 0.f;
    }
    if (bits == 16) {
      int16_t v;
      std::memcpy(&v, p, 2);
      return float(v) / 32768.f;
    }
    if (bits == 24) {
      auto v = uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16;
      if (v & 0x800000)
        v |= 0xff000000;
      return float(static_cast<int32_t>(v)) / 8388608.f;
    }
    int32_t v;
    std::memcpy(&v, p, 4);
    return static_cast<float>(double(v) / 2147483648.0);
  }
};
ComPtr<IMMDeviceEnumerator> enumerator() {
  ComPtr<IMMDeviceEnumerator> e;
  hr(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                      CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&e)),
     "Cannot enumerate audio endpoints");
  return e;
}
// RF64 is used from the outset; ds64 counts never wrap at the RIFF 4 GiB
// boundary.
class Wave {
  HANDLE file_{INVALID_HANDLE_VALUE};
  std::array<std::array<float, 2>,96> tail_{};
  size_t tail_begin_{},tail_count_{};
  std::vector<int16_t> block_;
  uint64_t committed_{}, total_{}, ramp_{};
  int64_t maximum_write_hns_{};
  bool finalized_{};
  void io(const void *data, DWORD size) {
    const auto started = qpc_hns();
    DWORD done{};
    const bool written = WriteFile(file_, data, size, &done, nullptr) != FALSE;
    const auto error = written ? (done == size ? ERROR_SUCCESS : ERROR_WRITE_FAULT) : GetLastError();
    maximum_write_hns_ = (std::max)(maximum_write_hns_, qpc_hns() - started);
    if (error != ERROR_SUCCESS)
      throw Error(NSS_E_IO, NSS_STAGE_STORAGE,
                  HRESULT_FROM_WIN32(error), "WAV write failed");
  }
  void flush() {
    if (!block_.empty()) {
      io(block_.data(), static_cast<DWORD>(block_.size() * 2));
      committed_ += block_.size() / 2;
      block_.clear();
    }
  }
  void commit(std::array<float, 2> frame, float gain = 1.f) {
    for (float v : frame)
      block_.push_back(v==0.f ? int16_t{} : static_cast<int16_t>(std::lround(
          std::clamp(v * gain, -1.f, 32767.f / 32768.f) * 32768.f)));
    if (block_.size() >= 16384)
      flush();
  }

public:
  static std::array<uint64_t, 3> sizes(uint64_t samples) {
    return {72 + samples * 4, samples * 4, samples};
  }
#ifdef NSS_AUDIO_QUALIFICATION
  void invalidate_for_test() {
    CloseHandle(std::exchange(file_, INVALID_HANDLE_VALUE));
  }
#endif
  ~Wave() {
    if (file_ != INVALID_HANDLE_VALUE)
      CloseHandle(file_);
  }
  void open(const std::string &path) {
    file_ = CreateFileW(wide(path).c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                        nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file_ == INVALID_HANDLE_VALUE)
      throw Error(NSS_E_IO, NSS_STAGE_STORAGE,
                  HRESULT_FROM_WIN32(GetLastError()),
                  "Cannot create exclusive WAV");
    std::array<uint8_t, 80> h{};
    auto put = [&](size_t o, auto v) {
      std::memcpy(h.data() + o, &v, sizeof(v));
    };
    std::memcpy(h.data(), "RF64", 4);
    put(4, uint32_t{0xffffffff});
    std::memcpy(h.data() + 8, "WAVEds64", 8);
    put(16, uint32_t{28});
    std::memcpy(h.data() + 48, "fmt ", 4);
    put(52, uint32_t{16});
    put(56, uint16_t{1});
    put(58, uint16_t{2});
    put(60, uint32_t{48000});
    put(64, uint32_t{192000});
    put(68, uint16_t{4});
    put(70, uint16_t{16});
    std::memcpy(h.data() + 72, "data", 4);
    put(76, uint32_t{0xffffffff});
    io(h.data(), static_cast<DWORD>(h.size()));
  }
  uint64_t count() const { return total_; }
  double maximum_write_ms() const { return double(maximum_write_hns_) / 10000; }
  void append(std::array<float, 2> frame) {
    const float gain =
        static_cast<float>((std::min)(++ramp_, uint64_t{96})) / 96.f;
    for (auto &v : frame)
      v *= gain;
    ++total_;
    if(tail_count_==tail_.size()) {
      commit(tail_[tail_begin_]);
      tail_[tail_begin_]=frame;
      tail_begin_=(tail_begin_+1)%tail_.size();
    } else
      tail_[(tail_begin_+tail_count_++)%tail_.size()]=frame;
  }
  void end_segment() {
    const auto n = tail_count_;
    for(size_t i=0;i<n;++i)
      commit(tail_[(tail_begin_+i)%tail_.size()], static_cast<float>(n - 1 - i) /
                    static_cast<float>((std::max)(n, size_t{1})));
    tail_begin_=tail_count_=0;
    ramp_ = 0;
  }
  void silence_until(uint64_t at) {
    if (at > total_) {
      end_segment();
      while (total_ < at) {
        commit({0, 0});
        ++total_;
      }
    }
  }
  void finish(uint64_t samples) {
    end_segment();
    silence_until(samples);
    flush();
    // A late Stop/pause cut can retract the bounded last packet; the clock owns
    // the cut.
    total_ = samples;
    LARGE_INTEGER p{};
    p.QuadPart = static_cast<LONGLONG>(80 + samples * 4);
    if (!SetFilePointerEx(file_, p, nullptr, FILE_BEGIN) ||
        !SetEndOfFile(file_))
      hr(HRESULT_FROM_WIN32(GetLastError()), "WAV truncation failed");
    if (samples) {
      const uint64_t n = (std::min)(samples, uint64_t{96});
      std::vector<int16_t> last(static_cast<size_t>(n * 2));
      p.QuadPart = static_cast<LONGLONG>(80 + (samples - n) * 4);
      DWORD read{};
      hr(SetFilePointerEx(file_, p, nullptr, FILE_BEGIN)
             ? S_OK
             : HRESULT_FROM_WIN32(GetLastError()),
         "WAV tail seek failed");
      if (!ReadFile(file_, last.data(), static_cast<DWORD>(n * 4), &read,
                    nullptr) ||
          read != n * 4)
        hr(E_FAIL, "WAV tail read failed");
      for (uint64_t i = 0; i < n; ++i)
        for (size_t c = 0; c < 2; ++c)
          last[static_cast<size_t>(i * 2 + c)] =
              static_cast<int16_t>(last[static_cast<size_t>(i * 2 + c)] *
                                   double(n - 1 - i) / double(n));
      hr(SetFilePointerEx(file_, p, nullptr, FILE_BEGIN) ? S_OK : E_FAIL,
         "WAV tail seek failed");
      io(last.data(), static_cast<DWORD>(n * 4));
    }
    const auto lengths = sizes(samples);
    p.QuadPart = 20;
    hr(SetFilePointerEx(file_, p, nullptr, FILE_BEGIN) ? S_OK : E_FAIL,
       "RF64 seek failed");
    io(lengths.data(), 24);
    if (!FlushFileBuffers(file_))
      hr(HRESULT_FROM_WIN32(GetLastError()), "WAV flush failed");
    const auto f = std::exchange(file_, INVALID_HANDLE_VALUE);
    if (!CloseHandle(f))
      hr(HRESULT_FROM_WIN32(GetLastError()), "WAV close failed");
    finalized_ = true;
  }
  bool finalized() const { return finalized_; }
};
class Stream {
  Json plan_, report_, mix_format_, packet_timestamps_ = Json::array(),
                                    recent_timestamps_ = Json::array();
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  std::thread thread_;
  bool ready_{}, start_{}, started_{}, stop_{}, done_{};
  Fault fault_;
  SessionClock *clock_{};
  Wave wave_;
  std::unique_ptr<PacketQueue> packet_queue_;
  std::atomic<int64_t> capture_service_gap_max_hns_{};
  int64_t processing_max_hns_{}, queued_max_hns_{};
  uint32_t capture_buffer_frames_{};
#ifdef NSS_AUDIO_QUALIFICATION
  bool qualification_stall_{}, qualification_stall_done_{};
#endif
  std::vector<std::array<float, 2>> data_;
  std::vector<uint32_t> nonzero_prefix_;
  std::vector<uint32_t> above_pcm_zero_prefix_;
  uint64_t base_{}, next_device_{}, packets_{}, captured_{},
      silent_{}, discontinuities_{}, paused_samples_{}, gap_samples_{},
      timestamp_errors_{}, idle_samples_{};
  int64_t last_qpc_{}, last_valid_qpc_{};
  ClockRate clock_rate_;
  uint64_t last_valid_position_{}, previous_frames_{}, idle_intervals_{},
      lost_samples_{}, terminal_pad_{}, keepalive_frames_{}, exact_silence_outputs_{},
      pcm_zero_outputs_{}, filtered_outputs_{};
  double phase_{}, ppm_{}, observed_ppm_{};
  uint64_t segment_{~uint64_t{}};
  bool segment_open_{}, loopback_{};
  uint32_t rate_{};
  std::optional<SincKernel> kernel_;
  int64_t last_report_at_{};
  uint64_t report_publications_{};
  Json handoff_report() const {
    const auto stats = packet_queue_ ? packet_queue_->stats() : PacketQueue::Stats{};
    return {{"captureBufferFrames", capture_buffer_frames_},
            {"queuedFrames", stats.queued_frames}, {"peakQueuedFrames", stats.peak_frames},
            {"queuedPackets", stats.queued_packets}, {"peakQueuedPackets", stats.peak_packets},
            {"pushedPackets", stats.pushed}, {"processedOrInProgressPackets", stats.popped},
            {"allocatedQueueBytes", stats.bytes}, {"pendingDurationLimitMs", 500},
            {"packetDurationLimitMs", 200}, {"processingThreadsMax", 1},
            {"captureServiceGapMaxMs", double(capture_service_gap_max_hns_.load()) / 10000},
            {"packetProcessingMaxMs", double(processing_max_hns_) / 10000},
            {"queueDelayMaxMs", double(queued_max_hns_) / 10000},
            {"waveWriteMaxMs", wave_.maximum_write_ms()}};
  }
  void publish(const Format &format, bool force = true) {
    const auto now = qpc_hns();
    if (!force && now - last_report_at_ < hns_per_second)
      return;
    std::lock_guard lock(mutex_);
    last_report_at_ = now;
    ++report_publications_;
    report_ = {{"path", plan_.at("path")},
               {"kind", plan_.at("kind")},
               {"endpointId", plan_.at("endpointId")},
               {"nativeFormat", format.json()},
               {"outputRate", 48000},
               {"outputChannels", 2},
               {"outputFormat", "pcm_s16le_rf64"},
               {"channelMapping", format.channels == 1
                                      ? "mono duplicated to L/R"
                                      : "L/R preserved"},
               {"deviceMixFormat", mix_format_},
               {"wasapiFormatConversion", false},
               {"reportPublishedQpcHns", now},
               {"regularReportIntervalMs", 1000},
               {"reportPublicationCount", report_publications_},
               {"packetHandoff", handoff_report()},
               {"packetTimestamps", packet_timestamps_},
               {"recentPacketTimestamps", recent_timestamps_},
               {"packets", packets_},
               {"inputSamples", captured_},
               {"outputSamples", wave_.count()},
               {"silentFlagSamples", silent_},
               {"pausedInputSamplesRemoved", paused_samples_},
               {"deviceGapSamples", gap_samples_},
               {"discontinuities", discontinuities_},
               {"timestampErrors", timestamp_errors_},
               {"initialOrLoopbackIdleSilenceSamples", idle_samples_},
               {"driftCorrectionPpm", ppm_},
               {"observedDriftPpm", observed_ppm_},
               {"clockRateEstimator", {{"method", "centered rolling device-position/QPC regression"},
                    {"maximumWindowSeconds",30},{"observationLimit",640},
                    {"momentRebuildEveryObservations",256},
                    {"observations",clock_rate_.observations()},{"spanSeconds",clock_rate_.span_seconds()},
                    {"rawTimestampsChanged",false}}},
               {"loopbackNoPacketIdleIntervals", idle_intervals_},
               {"lostSamples", lost_samples_},
               {"terminalAlignmentSilenceSamples", terminal_pad_},
               {"loopbackClockKeepalive", loopback_},
               {"keepaliveSubmittedSilentFrames", keepalive_frames_},
               {"resampler", "continuous 32-tap windowed sinc; ratio slewed "
                             "<=20 ppm/s; bound +/-200 ppm"},
               {"resamplerPhaseEvaluation", "continuous four-point Lagrange interpolation of original analytical coefficients"},
               {"resamplerCoefficientBytes", kernel_ ? kernel_->storage_bytes() : 0},
               {"resamplerInstructionSet", kernel_ ? kernel_->instruction_set() : dsp::kernel().name},
               {"resamplerExactSilenceSamples", exact_silence_outputs_},
               {"resamplerPcmZeroSamples", pcm_zero_outputs_},
               {"resamplerPcmZeroInputLimit", kernel_ ? kernel_->pcm16_zero_input_limit() : 0},
               {"resamplerFilteredSamples", filtered_outputs_},
               {"declickSamples", 96},
               {"qpcUnit", "100ns supplied by WASAPI; no tick conversion"},
               {"finalized", wave_.finalized()}};
  }
  void flush_segment(uint64_t end, bool final) {
    if (!segment_open_)
      return;
    const auto limit = final ? double(end) : double(end > 16 ? end - 16 : 0);
    const double step = double(rate_) / 48000.0 * (1.0 + ppm_ * 1e-6);
    const double cutoff = (std::min)(1.0, 48000.0 / double(rate_)) * .94;
    // Prefix counts examine the complete tap support, including filter tails.
    // Original float inputs stay intact for later nonquiet windows.
    nonzero_prefix_.resize(data_.size() + 1);
    nonzero_prefix_[0] = 0;
    for (size_t i = 0; i < data_.size(); ++i)
      nonzero_prefix_[i + 1] = nonzero_prefix_[i] +
          uint32_t(data_[i][0] != 0.f || data_[i][1] != 0.f);
    if (nonzero_prefix_.back()) {
      if (!kernel_)
        kernel_.emplace(cutoff);
      const double zero_limit = kernel_->pcm16_zero_input_limit();
      above_pcm_zero_prefix_.resize(data_.size() + 1);
      above_pcm_zero_prefix_[0] = 0;
      for (size_t i = 0; i < data_.size(); ++i)
        above_pcm_zero_prefix_[i + 1] = above_pcm_zero_prefix_[i] +
            uint32_t(!(std::abs(data_[i][0]) <= zero_limit &&
                       std::abs(data_[i][1]) <= zero_limit));
    }
    while (phase_ < limit && !data_.empty()) {
      const int64_t center = static_cast<int64_t>(std::floor(phase_));
      const auto last = static_cast<int64_t>(data_.size() - 1);
      const auto first_tap = static_cast<size_t>(
          std::clamp<int64_t>(center - 15 - static_cast<int64_t>(base_), 0, last));
      const auto last_tap = static_cast<size_t>(
          std::clamp<int64_t>(center + 16 - static_cast<int64_t>(base_), 0, last));
      if (nonzero_prefix_[last_tap + 1] == nonzero_prefix_[first_tap]) {
        wave_.append({0, 0});
        ++exact_silence_outputs_;
        phase_ += step;
        continue;
      }
      if (above_pcm_zero_prefix_[last_tap + 1] == above_pcm_zero_prefix_[first_tap]) {
        // The coefficient L1 bound proves |output| <= 1/4 PCM16 LSB.
        // Existing gain<=1 ramps and nearest-integer commit produce exactly
        // the same zero sample; this does not gate audible input or time.
        wave_.append({0, 0});
        ++pcm_zero_outputs_;
        phase_ += step;
        continue;
      }
      const auto coefficients = kernel_->weights(phase_ - static_cast<double>(center));
      wave_.append(kernel_->filter(data_.data(), data_.size(),
          center - 15 - static_cast<int64_t>(base_), coefficients));
      ++filtered_outputs_;
      phase_ += step;
    }
    if (final) {
      wave_.end_segment();
      data_.clear();
      segment_open_ = false;
    } else if (phase_ > double(base_ + 32)) {
      const auto erase =
          (std::min)(data_.size(),
                     static_cast<size_t>(phase_ - double(base_)) - 16);
      data_.erase(data_.begin(), data_.begin() + static_cast<ptrdiff_t>(erase));
      base_ += erase;
    }
  }
  void packet(const std::vector<uint8_t> &copy, uint32_t frames, DWORD flags,
              uint64_t position, uint64_t qpc, const Format &format,
              int64_t observed_idle_hns = 0) {
    if (packet_timestamps_.size() < 16)
      packet_timestamps_.push_back({{"position", position},
                                    {"qpcHns", qpc},
                                    {"frames", frames},
                                    {"flags", flags}});
    if (recent_timestamps_.size() == 16)
      recent_timestamps_.erase(recent_timestamps_.begin());
    recent_timestamps_.push_back({{"position", position},
                                  {"qpcHns", qpc},
                                  {"frames", frames},
                                  {"flags", flags}});
    if (!(flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) &&
        qpc > static_cast<uint64_t>((std::numeric_limits<int64_t>::max)()))
      throw Error(NSS_E_IO, NSS_STAGE_AUDIO, 0,
                  "WASAPI QPC timestamp exceeds signed 100ns domain");
    if (flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) {
      ++timestamp_errors_;
      if (!last_valid_qpc_ || position < last_valid_position_ ||
          position - last_valid_position_ > rate_ / 5)
        throw Error(
            NSS_E_IO, NSS_STAGE_AUDIO, 0,
            "WASAPI timestamp error exceeds bounded 200ms extrapolation");
      qpc = static_cast<uint64_t>(last_valid_qpc_) +
            static_cast<uint64_t>(double(position - last_valid_position_) *
                                  hns_per_second / (rate_ * (1 + ppm_ * 1e-6)));
      if (timestamp_errors_ > 3)
        throw Error(NSS_E_IO, NSS_STAGE_AUDIO, 0,
                    "Repeated WASAPI timestamp errors");
    } else {
      last_valid_qpc_ = static_cast<int64_t>(qpc);
      last_valid_position_ = position;
    }
    if (last_qpc_ && static_cast<int64_t>(qpc) < last_qpc_)
      throw Error(NSS_E_IO, NSS_STAGE_AUDIO, 0,
                  "Audio timestamp moved backwards");
    const bool qpc_gap =
        last_qpc_ && static_cast<int64_t>(qpc) - last_qpc_ -
                             sample_time(previous_frames_, rate_) >
                         200000;
    const bool position_gap = next_device_ && position != next_device_;
    if (position_gap || qpc_gap) {
      if (position < next_device_)
        throw Error(NSS_E_IO, NSS_STAGE_AUDIO, 0,
                    "Audio device position moved backwards");
      gap_samples_ += position - next_device_;
      // Idle loopback is established by repeated empty polls, never inferred
      // from a dropped packet alone.
      const bool observed_idle =
          loopback_ && observed_idle_hns >= 300000;
      if (!observed_idle) {
        lost_samples_ += position - next_device_;
        const auto detail =
            "Mandatory audio timestamp/sample gap: position=" +
            std::to_string(position) +
            " expected=" + std::to_string(next_device_) + " qpcDeltaHns=" +
            std::to_string(static_cast<int64_t>(qpc) - last_qpc_) +
            " previousFrames=" + std::to_string(previous_frames_) +
            " flags=" + std::to_string(flags);
        throw Error(NSS_E_IO, NSS_STAGE_AUDIO, 0, detail.c_str());
      }
      ++idle_intervals_;
      flush_segment(next_device_, true);
      clock_rate_.reset();
    }
    if ((flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) && packets_ &&
        !(loopback_ && observed_idle_hns >= 300000))
      throw Error(NSS_E_IO, NSS_STAGE_AUDIO, 0,
                  "Audio reported an unexplained discontinuity after its "
                  "initial baseline");
    if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY)
      ++discontinuities_;
    if (flags & AUDCLNT_BUFFERFLAGS_SILENT)
      silent_ += frames;
    // Extrapolated timestamps may preserve a bounded damaged packet, but must
    // not train the estimator from its own previous correction.
    if (!(flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR))
      clock_rate_.observe(position, static_cast<int64_t>(qpc), rate_);
    if (const auto measured=clock_rate_.ppm()) {
      observed_ppm_ = *measured;
      if (clock_rate_.span_seconds() >= 5 && std::abs(observed_ppm_) > 200)
        throw Error(NSS_E_IO, NSS_STAGE_AUDIO, 0,
                    "Audio clock drift exceeds +/-200 ppm correction budget");
      const double desired = std::clamp(observed_ppm_, -200.0, 200.0);
      const double slew =
          20.0 * double(static_cast<int64_t>(qpc) - last_qpc_) / hns_per_second;
      ppm_ += std::clamp(desired - ppm_, -slew, slew);
    }
    const auto slices =
        clock_->trim_audio(static_cast<int64_t>(qpc), frames, rate_);
    uint64_t used{};
    const auto history = clock_->history();
    for (const auto &slice : slices) {
      used += slice.count;
      const int64_t first_qpc =
          static_cast<int64_t>(qpc) + sample_time(slice.first, rate_);
      uint64_t generation{};
      for (const auto &cut : history)
        if (first_qpc >= cut.begin &&
            (!cut.end || cut.end < 0 || first_qpc < cut.end))
          generation = cut.generation;
      const uint64_t begin = position + slice.first, end = begin + slice.count;
      if (!segment_open_ || segment_ != generation ||
          begin != base_ + data_.size()) {
        flush_segment(base_ + data_.size(), true);
        const auto target = scale_floor(static_cast<uint64_t>(slice.pts_hns),
                                        48000, hns_per_second);
        if (target > wave_.count()) {
          idle_samples_ += target - wave_.count();
          wave_.silence_until(target);
        }
        base_ = begin;
        phase_ = double(begin);
        segment_ = generation;
        segment_open_ = true;
      }
      if (data_.size() + slice.count > uint64_t(rate_) / 2 + 64)
        throw Error(NSS_E_BACKPRESSURE, NSS_STAGE_AUDIO, 0,
                    "Audio DSP buffer exceeded 500ms budget");
      for (uint64_t i = slice.first; i < slice.first + slice.count; ++i) {
        std::array<float, 2> f{};
        if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT)) {
          const auto *p = copy.data() + static_cast<size_t>(i * format.align);
          f[0] = format.read(p);
          f[1] = format.channels == 1 ? f[0] : format.read(p + format.bits / 8);
        }
        data_.push_back(f);
      }
      flush_segment(end, slice.first + slice.count < frames);
    }
    paused_samples_ += frames - used;
    captured_ += frames;
    ++packets_;
    next_device_ = position + frames;
    last_qpc_ = static_cast<int64_t>(qpc);
    previous_frames_ = frames;
  }
  void run() noexcept {
    ++live_sources;
    try {
      Apartment apartment;
      auto e = enumerator();
      ComPtr<IMMDevice> device;
      hr(e->GetDevice(wide(plan_.at("endpointId").get<std::string>()).c_str(),
                      &device),
         "Selected audio endpoint is unavailable");
      DWORD state{};
      hr(device->GetState(&state), "Endpoint state unavailable");
      if (state != DEVICE_STATE_ACTIVE)
        throw Error(NSS_E_UNAVAILABLE, NSS_STAGE_AUDIO, 0,
                    "Selected endpoint is not active");
      ComPtr<IMMEndpoint> endpoint;
      hr(device.As(&endpoint), "Invalid audio endpoint");
      EDataFlow flow{};
      hr(endpoint->GetDataFlow(&flow), "Audio endpoint flow unavailable");
      loopback_ = plan_.at("kind") == "loopback";
      if (flow != (loopback_ ? eRender : eCapture))
        throw std::invalid_argument("Audio endpoint kind does not match");
      ComPtr<IAudioClient> client;
      hr(device->Activate(__uuidof(IAudioClient), CLSCTX_INPROC_SERVER, nullptr,
                          &client),
         "Cannot activate selected audio endpoint");
      WAVEFORMATEX *raw{};
      hr(client->GetMixFormat(&raw), "Endpoint mix format unavailable");
      const std::unique_ptr<WAVEFORMATEX, decltype(&CoTaskMemFree)> owner(
          raw, CoTaskMemFree);
      mix_format_ = Format(raw).json();
      // Keep GetBuffer device positions and frames in the endpoint's actual mix
      // format. Engine DSP owns rate/channel conversion, including 44.1 -> 48
      // kHz.
      const DWORD streamflags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                                (loopback_ ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0);
      const Format format(raw);
      rate_ = format.rate;
      hr(client->Initialize(AUDCLNT_SHAREMODE_SHARED, streamflags, 2000000, 0,
                            raw, nullptr),
         "WASAPI shared initialization failed");
      hr(client->GetBufferSize(&capture_buffer_frames_), "Capture capacity unavailable");
      packet_queue_ = std::make_unique<PacketQueue>(format.rate, format.align);
      HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
      if (!event)
        hr(HRESULT_FROM_WIN32(GetLastError()), "Audio event allocation failed");
      struct EventOwner {
        HANDLE h;
        ~EventOwner() { CloseHandle(h); }
      } event_owner{event};
      hr(client->SetEventHandle(event), "Cannot bind audio event");
      ComPtr<IAudioCaptureClient> capture;
      hr(client->GetService(IID_PPV_ARGS(&capture)),
         "Cannot obtain audio capture client");
      // A silent render stream on the same endpoint keeps the physical mixer
      // clock running when other applications stop playback. This is actual
      // endpoint capture; silence is never substituted for lost input packets.
      ComPtr<IAudioClient> keep_client;
      ComPtr<IAudioRenderClient> keep_render;
      UINT32 keep_capacity{};
      struct KeepStop {
        IAudioClient *p{};
        ~KeepStop() {
          if (p)
            p->Stop();
        }
      } keep_stop;
      if (loopback_) {
        hr(device->Activate(__uuidof(IAudioClient), CLSCTX_INPROC_SERVER,
                            nullptr, &keep_client),
           "Cannot activate loopback clock keepalive");
        hr(keep_client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                   AUDCLNT_STREAMFLAGS_NOPERSIST, 2000000, 0,
                                   raw, nullptr),
           "Cannot prepare loopback clock keepalive");
        hr(keep_client->GetBufferSize(&keep_capacity),
           "Keepalive capacity unavailable");
        hr(keep_client->GetService(IID_PPV_ARGS(&keep_render)),
           "Keepalive render service unavailable");
        BYTE *silent{};
        hr(keep_render->GetBuffer(keep_capacity, &silent),
           "Keepalive prefill failed");
        hr(keep_render->ReleaseBuffer(keep_capacity,
                                      AUDCLNT_BUFFERFLAGS_SILENT),
           "Keepalive prefill release failed");
        keepalive_frames_ += keep_capacity;
      }
      wave_.open(plan_.at("path"));
      publish(format);
      {
        std::unique_lock lock(mutex_);
        ready_ = true;
        changed_.notify_all();
        changed_.wait(lock, [&] { return start_ || stop_; });
        if (stop_) {
          done_ = true;
          --live_sources;
          return;
        }
      }
      if (keep_client) {
        hr(keep_client->Start(), "Loopback clock keepalive did not start");
        keep_stop.p = keep_client.Get();
      }
      hr(client->Start(), "Mandatory audio source did not start");
      {
        std::lock_guard lock(mutex_);
        started_ = loopback_;
        changed_.notify_all();
      }
      struct StopClient {
        IAudioClient *p;
        ~StopClient() {
          if (p)
            p->Stop();
        }
      } stop_client{client.Get()};
      DWORD task_index{};
      HANDLE task = AvSetMmThreadCharacteristicsW(L"Audio", &task_index);
      struct TaskOwner {
        HANDLE task;
        ~TaskOwner() {
          if (task)
            AvRevertMmThreadCharacteristics(task);
        }
      } task_owner{task};
      if (task)
        AvSetMmThreadPriority(task, AVRT_PRIORITY_HIGH);
      PacketProcessor processing(*packet_queue_, size_t(format.rate / 5) * format.align,
          [&](const std::vector<uint8_t> &copy, const PacketQueue::Packet &input) {
            const auto began = qpc_hns();
#ifdef NSS_AUDIO_QUALIFICATION
            if (qualification_stall_ && !qualification_stall_done_ && packets_ >= 50) {
              qualification_stall_done_ = true;
              std::this_thread::sleep_for(std::chrono::milliseconds(250));
            }
#endif
            queued_max_hns_ = (std::max)(queued_max_hns_, began - input.received_hns);
            keepalive_frames_ = input.keepalive_frames;
            packet(copy, input.frames, input.flags, input.position, input.qpc, format,
                   input.observed_idle_hns);
            processing_max_hns_ = (std::max)(processing_max_hns_, qpc_hns() - began);
            publish(format, false);
            std::lock_guard lock(mutex_);
            if (!started_) { started_ = true; changed_.notify_all(); }
          });
      auto keepalive_submitted = keepalive_frames_;
      auto last_packet_wall = qpc_hns();
      auto last_service = last_packet_wall;
      int64_t empty_since{}, last_empty{}, received_qpc{};
      uint32_t received_frames{};
      int64_t stop_seen{};
      for (;;) {
        packet_queue_->check();
        const auto service_at = qpc_hns();
        const auto service_gap = service_at - last_service;
        if (service_gap > capture_service_gap_max_hns_.load())
          capture_service_gap_max_hns_ = service_gap;
        last_service = service_at;
        if (keep_client) {
          UINT32 padding{};
          hr(keep_client->GetCurrentPadding(&padding),
             "Loopback clock keepalive invalidated");
          if (padding > keep_capacity)
            throw Error(NSS_E_INTERNAL, NSS_STAGE_AUDIO, 0,
                        "Invalid keepalive padding");
          const auto available = keep_capacity - padding;
          if (available) {
            BYTE *silent{};
            hr(keep_render->GetBuffer(available, &silent),
               "Keepalive buffer unavailable");
            hr(keep_render->ReleaseBuffer(available,
                                          AUDCLNT_BUFFERFLAGS_SILENT),
               "Keepalive buffer release failed");
            keepalive_submitted += available;
          }
        }
        WaitForSingleObject(event, 5);
        UINT32 available{};
        hr(capture->GetNextPacketSize(&available),
           "Audio endpoint invalidated or capture failed");
        while (available) {
          BYTE *bytes{};
          UINT32 frames{};
          DWORD flags{};
          UINT64 position{}, qpc{};
          hr(capture->GetBuffer(&bytes, &frames, &flags, &position, &qpc),
             "WASAPI GetBuffer failed");
          if (!frames) break;
          const auto received = qpc_hns();
          // Copy into preallocated owned storage, preserving the endpoint's
          // positions/flags. Both WASAPI calls stay on this owner thread.
          try {
            packet_queue_->push(bytes, {frames, flags, position, qpc, keepalive_submitted,
                received, empty_since && received - last_empty <= 300000
                              ? last_empty - empty_since : 0,
                bool(flags & AUDCLNT_BUFFERFLAGS_SILENT)});
          } catch (...) { capture->ReleaseBuffer(frames); throw; }
          hr(capture->ReleaseBuffer(frames), "WASAPI ReleaseBuffer failed");
          last_packet_wall = received;
          if (!(flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) &&
              qpc <= uint64_t((std::numeric_limits<int64_t>::max)())) {
            received_qpc = static_cast<int64_t>(qpc);
            received_frames = frames;
          }
          empty_since = last_empty = 0;
          hr(capture->GetNextPacketSize(&available),
             "Audio endpoint invalidated");
        }
        const auto empty_at = qpc_hns();
        // Only completed, consecutive empty polls establish idle. A scheduling
        // stall or processing backlog is never evidence of source silence.
        if (!empty_since || empty_at - last_empty > 300000)
          empty_since = empty_at;
        last_empty = empty_at;
        if (!loopback_ && qpc_hns() - last_packet_wall > hns_per_second)
          throw Error(NSS_E_TIMEOUT, NSS_STAGE_AUDIO, 0,
                      "Mandatory microphone stopped delivering packets");
        bool stopping{};
        {
          std::lock_guard lock(mutex_);
          stopping = stop_;
        }
        if (stopping) {
          const auto now = qpc_hns();
          if (!stop_seen)
            stop_seen = now;
          const auto cuts = clock_->history();
          const auto cut = cuts.empty() ? now : cuts.back().end;
          // Retain packets still in the endpoint at Stop; trim against the
          // already frozen cut.
          if ((cut >= 0 && received_qpc &&
               received_qpc + sample_time(received_frames, rate_) >= cut) ||
              now - stop_seen >= 500000)
            break;
        }
      }
      hr(client->Stop(), "Mandatory WASAPI Stop failed");
      stop_client.p = nullptr;
      if (keep_client) {
        hr(keep_client->Stop(), "Loopback clock keepalive Stop failed");
        keep_stop.p = nullptr;
      }
      processing.drain();
      keepalive_frames_ = keepalive_submitted;
      flush_segment(base_ + data_.size(), true);
      publish(format);
    } catch (const Error &e) {
      std::lock_guard lock(mutex_);
      report_["packetTimestamps"] = packet_timestamps_;
      report_["recentPacketTimestamps"] = recent_timestamps_;
      report_["lostSamples"] = lost_samples_;
      report_["deviceGapSamples"] = gap_samples_;
      report_["timestampErrors"] = timestamp_errors_;
      report_["packetHandoff"] = handoff_report();
      report_["firstError"] = {{"code", e.code},
                               {"stage", e.stage},
                               {"hresult", e.native},
                               {"message", e.what()}};
      fault_ = {e.code, e.stage, e.native, e.what()};
    } catch (const std::exception &e) {
      std::lock_guard lock(mutex_);
      report_["packetTimestamps"] = packet_timestamps_;
      report_["recentPacketTimestamps"] = recent_timestamps_;
      report_["packetHandoff"] = handoff_report();
      report_["firstError"] = {{"code", NSS_E_INTERNAL},
                               {"stage", NSS_STAGE_AUDIO},
                               {"hresult", 0},
                               {"message", e.what()}};
      fault_ = {NSS_E_INTERNAL, NSS_STAGE_AUDIO, 0, e.what()};
    }
    {
      std::lock_guard lock(mutex_);
      done_ = true;
      ready_ = true;
      changed_.notify_all();
    }
    --live_sources;
  }

public:
#ifdef NSS_AUDIO_QUALIFICATION
  static Json live_handoff(const std::string &filename) {
    const auto endpoints = devices(NSS_SOURCE_LOOPBACK).at("devices");
    if (endpoints.empty()) throw std::runtime_error("Live handoff requires a render endpoint");
    const auto found = std::find_if(endpoints.begin(), endpoints.end(),
        [](const Json &value) { return value.at("default").get<bool>(); });
    const auto &endpoint = found == endpoints.end() ? endpoints.front() : *found;
    Stream stream({{"path", filename}, {"kind", "loopback"}, {"endpointId", endpoint.at("id")}});
    stream.qualification_stall_ = true;
    stream.prepare();
    SessionClock clock;
    const auto begin = qpc_hns();
    clock.start(begin);
    stream.start(clock);
    while (qpc_hns() - begin < 2 * hns_per_second) {
      stream.check();
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    const auto end = qpc_hns();
    clock.stop(end);
    stream.finish(clock.active_time(end));
    const auto report = stream.snapshot();
    if (!stream.qualification_stall_done_ || report.at("lostSamples") != 0 ||
        report.at("packetHandoff").at("queuedFrames") != 0 ||
        report.at("packetHandoff").at("packetProcessingMaxMs").get<double>() < 250 ||
        report.at("packetHandoff").at("peakQueuedFrames").get<uint64_t>() < stream.rate_ / 5)
      throw std::runtime_error("Real audio handoff did not preserve packets through the owned 250ms stall");
    return report;
  }
  static Json fixture(const std::string &filename, uint32_t rate,
                      uint32_t channels, bool pause, double drift = 0,
                      int fault = 0) {
    Stream s({{"path", filename},
              {"kind", "microphone"},
              {"endpointId", "qualification-only"}});
    WAVEFORMATEX native{};
    native.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    native.nChannels = static_cast<WORD>(channels);
    native.nSamplesPerSec = rate;
    native.wBitsPerSample = 32;
    native.nBlockAlign = static_cast<WORD>(channels * 4);
    const Format format(&native);
    s.rate_ = rate;
    s.mix_format_ = format.json();
    SessionClock clock;
    const int64_t epoch = 100000000, wall = 60000000;
    clock.start(epoch);
    if (pause) {
      clock.pause(epoch + 12345678);
      clock.resume(epoch + 15679011);
    }
    clock.stop(epoch + wall);
    s.clock_ = &clock;
    s.start_ = true;
    s.wave_.open(filename);
    const uint32_t quantum = rate / 100;
    for (uint64_t pos = 0; pos < uint64_t(rate) * 6; pos += quantum) {
      uint64_t qpc = static_cast<uint64_t>(
          epoch + 2500000 +
          static_cast<int64_t>(double(pos) * hns_per_second /
                               (rate * (1 + drift * 1e-6))));
      uint64_t position = pos;
      DWORD flags{};
      if (fault == 1 && pos == 0)
        flags = AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR;
      if (fault == 2 && pos == quantum * 10)
        position += quantum;
      if (fault == 3 && pos == quantum * 10)
        qpc += hns_per_second;
      if (fault == 4 && pos == quantum * 10)
        flags = AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR;
      if (fault == 5 && pos >= quantum * 100 && pos < quantum * 200)
        flags = AUDCLNT_BUFFERFLAGS_SILENT;
      std::vector<uint8_t> copy(size_t(quantum) * channels * 4);
      if (fault == 6 && pos == quantum * 100)
        s.wave_.invalidate_for_test();
      if (fault == 7 && pos == 0)
        qpc = (std::numeric_limits<uint64_t>::max)();
      for (uint32_t i = 0; i < quantum; ++i) {
        const auto at = qpc + static_cast<uint64_t>(sample_time(i, rate));
        const double hz = pause && at >= uint64_t(epoch + 12345678) &&
                                  at < uint64_t(epoch + 15679011)
                              ? 4000
                              : 1000;
        const float value = static_cast<float>(
            .25 * std::sin(2 * std::numbers::pi * hz * double(pos + i) / rate));
        for (uint32_t c = 0; c < channels; ++c)
          std::memcpy(copy.data() + (size_t(i) * channels + c) * 4, &value, 4);
      }
      s.packet(copy, quantum, flags, position, qpc, format);
    }
    s.flush_segment(s.base_ + s.data_.size(), true);
    s.publish(format);
    s.finish(clock.active_time(epoch + wall));
    auto result = s.snapshot();
    result["qualificationOnly"] = true;
    return result;
  }
#endif
  explicit Stream(Json plan) : plan_(std::move(plan)) {}
  ~Stream() { stop(); }
  void prepare() {
    thread_ = std::thread([this] { run(); });
    std::unique_lock lock(mutex_);
    if (!changed_.wait_for(lock, std::chrono::seconds(3),
                           [&] { return ready_; }))
      throw Error(NSS_E_TIMEOUT, NSS_STAGE_AUDIO, 0, "Audio Prepare timed out");
    if (fault_)
      throw Error(fault_.code, fault_.stage, fault_.hresult,
                  fault_.message.c_str());
  }
  void start(SessionClock &clock) {
    std::unique_lock lock(mutex_);
    clock_ = &clock;
    start_ = true;
    changed_.notify_all();
    if (!changed_.wait_for(lock, std::chrono::seconds(3),
                           [&] { return started_ || done_; }))
      throw Error(NSS_E_TIMEOUT, NSS_STAGE_AUDIO, 0, "Audio Start timed out");
    if (fault_)
      throw Error(fault_.code, fault_.stage, fault_.hresult,
                  fault_.message.c_str());
  }
  void stop() {
    {
      std::lock_guard lock(mutex_);
      stop_ = true;
      changed_.notify_all();
    }
    if (thread_.joinable())
      thread_.join();
  }
  void check() {
    std::lock_guard lock(mutex_);
    if (fault_)
      throw Error(fault_.code, fault_.stage, fault_.hresult,
                  fault_.message.c_str());
  }
  void finish(int64_t hns) {
    stop();
    check();
    if (!start_)
      throw Error(NSS_E_STATE, NSS_STAGE_AUDIO, 0,
                  "Cannot finalize audio without Start");
    const auto target =
        scale_floor(static_cast<uint64_t>(hns), 48000, hns_per_second);
    terminal_pad_ = target > wave_.count() ? target - wave_.count() : 0;
    idle_samples_ += terminal_pad_;
    wave_.finish(target);
    std::lock_guard lock(mutex_);
    report_["outputSamples"] = target;
    report_["terminalAlignmentSilenceSamples"] = terminal_pad_;
    report_["initialOrLoopbackIdleSilenceSamples"] = idle_samples_;
    report_["finalized"] = true;
    report_["durationHns"] = hns;
  }
  Json snapshot() const {
    std::lock_guard lock(mutex_);
    return report_;
  }
};
} // namespace
Json devices(uint32_t kind) {
  if (kind != NSS_SOURCE_MICROPHONE && kind != NSS_SOURCE_LOOPBACK)
    throw std::invalid_argument("Invalid audio source kind");
  return std::async(
             std::launch::async,
             [kind] {
               Apartment apartment;
               auto e = enumerator();
               ComPtr<IMMDeviceCollection> collection;
               const auto flow =
                   kind == NSS_SOURCE_LOOPBACK ? eRender : eCapture;
               hr(e->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &collection),
                  "Audio enumeration failed");
               ComPtr<IMMDevice> default_device;
               LPWSTR default_id{};
               if (SUCCEEDED(e->GetDefaultAudioEndpoint(flow, eConsole,
                                                        &default_device)))
                 default_device->GetId(&default_id);
               const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)>
                   default_owner(default_id, CoTaskMemFree);
               UINT count{};
               hr(collection->GetCount(&count), "Audio endpoint count failed");
               Json list = Json::array();
               for (UINT i = 0; i < count; ++i) {
                 ComPtr<IMMDevice> device;
                 hr(collection->Item(i, &device), "Audio endpoint unavailable");
                 LPWSTR id{};
                 hr(device->GetId(&id), "Endpoint ID unavailable");
                 const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)>
                     id_owner(id, CoTaskMemFree);
                 ComPtr<IPropertyStore> store;
                 hr(device->OpenPropertyStore(STGM_READ, &store),
                    "Endpoint properties unavailable");
                 PROPVARIANT name{};
                 PropVariantInit(&name);
                 hr(store->GetValue(PKEY_Device_FriendlyName, &name),
                    "Endpoint name unavailable");
                 const auto friendly =
                     name.vt == VT_LPWSTR ? utf8(name.pwszVal) : utf8(id);
                 PropVariantClear(&name);
                 ComPtr<IAudioClient> client;
                 hr(device->Activate(__uuidof(IAudioClient),
                                     CLSCTX_INPROC_SERVER, nullptr, &client),
                    "Endpoint format activation failed");
                 WAVEFORMATEX *raw{};
                 hr(client->GetMixFormat(&raw), "Endpoint format query failed");
                 const std::unique_ptr<WAVEFORMATEX, decltype(&CoTaskMemFree)>
                     owner(raw, CoTaskMemFree);
                 list.push_back(
                     {{"id", utf8(id)},
                      {"name", friendly},
                      {"default",
                       default_id && std::wcscmp(id, default_id) == 0},
                      {"kind", kind},
                      {"mixFormat", Format(raw).json()}});
               }
               return Json{{"version", 1},
                           {"kind", kind},
                           {"devices", list},
                           {"acceleratedDeviceCreated", false}};
             })
      .get();
}
Json validate(const Json &request, const std::string &video_path) {
  if (!request.is_array() || request.empty() || request.size() > 2)
    throw std::invalid_argument("Audio requires one or two mandatory sources");
  Json result = request;
  std::vector<std::wstring> paths;
  std::vector<std::string> kinds;
  for (auto &item : result) {
    if (!item.is_object() || item.size() != 3 || !item.contains("kind") ||
        !item.contains("endpointId") || !item.contains("path"))
      throw std::invalid_argument(
          "Audio source requires kind, endpointId and path");
    const auto kind = item.at("kind").get<std::string>();
    if (kind != "microphone" && kind != "loopback")
      throw std::invalid_argument("Unsupported audio source kind");
    if (std::find(kinds.begin(), kinds.end(), kind) != kinds.end())
      throw std::invalid_argument("Duplicate audio source");
    kinds.push_back(kind);
    (void)wide(item.at("endpointId"));
    const auto path = wide(item.at("path"));
    const std::filesystem::path p(path);
    if (!p.is_absolute() || p.extension() != L".wav" ||
        std::filesystem::exists(p) ||
        !std::filesystem::is_directory(p.parent_path()) ||
        std::any_of(paths.begin(), paths.end(), [&](const auto &existing) {
          return _wcsicmp(existing.c_str(), path.c_str()) == 0;
        }))
      throw std::invalid_argument(
          "WAV requires a distinct new absolute .wav path");
    paths.push_back(path);
  }
  if (std::filesystem::exists(wide(video_path + ".session.manifest.json")))
    throw std::invalid_argument("Session manifest already exists");
  return result;
}
struct Session::Impl {
  std::vector<std::unique_ptr<Stream>> streams;
  explicit Impl(const Json &p) {
    for (const auto &s : p)
      streams.push_back(std::make_unique<Stream>(s));
  }
};
Session::Session(Json plan) : impl_(std::make_unique<Impl>(plan)) {}
Session::~Session() = default;
void Session::prepare() {
  for (auto &s : impl_->streams)
    s->prepare();
}
void Session::start(SessionClock &clock) {
  for (auto &s : impl_->streams)
    s->start(clock);
}
void Session::check() {
  for (auto &s : impl_->streams)
    s->check();
}
void Session::stop() {
  for (auto &s : impl_->streams)
    s->stop();
}
void Session::finalize(int64_t hns) {
  for (auto &s : impl_->streams)
    s->finish(hns);
}
Json Session::snapshot() const {
  Json a = Json::array();
  for (const auto &s : impl_->streams)
    a.push_back(s->snapshot());
  return a;
}
#ifdef NSS_AUDIO_QUALIFICATION
Json qualify(const std::string &scope, const std::string &directory) {
  std::filesystem::create_directories(wide(directory));
  Json cases = Json::array();
  uint64_t serial{};
  auto filename = [&] {
    return directory + "/audio-" + std::to_string(GetCurrentProcessId()) + "-" +
           std::to_string(GetTickCount64()) + "-" + std::to_string(++serial) +
           ".wav";
  };
  auto check = [](bool b, const char *text) {
    if (!b)
      throw std::runtime_error(text);
  };
  if(scope == "handoff-live") {
    cases.push_back(Stream::live_handoff(filename()));
  } else if(scope == "pcm-store") {
    const auto path=filename();
    Wave wave;wave.open(path);
    std::vector<int16_t> expected;
    const std::array<float,12> edges{0.f,-0.f,1.f,-1.f,1.5f,-1.5f,
      .5f/32768,-.5f/32768,std::nextafter(.5f/32768,0.f),
      std::nextafter(-.5f/32768,0.f),32767.f/32768,-32767.f/32768};
    for(const size_t length:{size_t{0},size_t{1},size_t{47},size_t{96},size_t{97},size_t{48001}}) {
      for(size_t i=0;i<length;++i) {
        const std::array<float,2> sample{edges[i%edges.size()],edges[(i*7+3)%edges.size()]};
        wave.append(sample);
        const auto tail=(std::min)(length,size_t{96});
        const float begin_gain=static_cast<float>((std::min)(i+1,size_t{96}))/96.f;
        const float end_gain=i>=length-tail ? static_cast<float>(length-1-i)/static_cast<float>(tail) : 1.f;
        // Independent closed-form reference for both ramps; no FIFO/ring is
        // used by the oracle. Both multiplications retain the original float
        // precision and PCM half-away-from-zero quantization.
        for(float value:sample) {
          value*=begin_gain;
          expected.push_back(static_cast<int16_t>(std::lround(
            std::clamp(value*end_gain,-1.f,32767.f/32768)*32768.f)));
        }
      }
      wave.end_segment();
    }
    wave.finish(expected.size()/2);
    // Finalization can retract a late clock cut and applies the existing
    // integer-domain terminal fade to the last96 stored frames as well.
    const auto terminal=(std::min)(expected.size()/2,size_t{96});
    for(size_t i=0;i<terminal;++i)
      for(size_t channel=0;channel<2;++channel) {
        auto &sample=expected[(expected.size()/2-terminal+i)*2+channel];
        sample=static_cast<int16_t>(sample*double(terminal-1-i)/double(terminal));
      }
    std::ifstream input(std::filesystem::path(wide(path)),std::ios::binary);
    const std::vector<char> bytes((std::istreambuf_iterator<char>(input)),std::istreambuf_iterator<char>());
    check(bytes.size()==80+expected.size()*sizeof(int16_t),"PCM writer byte count changed");
    check(std::memcmp(bytes.data()+80,expected.data(),expected.size()*sizeof(int16_t))==0,
      "PCM writer changed a ramp, clipped value, signed zero or rounding boundary");
    cases.push_back({{"path",path},{"pcmFramesCompared",expected.size()/2},
      {"bitExactClosedFormReference",true},{"tailStorage", "fixed96frame ring"}});
  } else if (scope == "resample" || scope == "pause") {
    for (const auto rate : {44100u, 48000u})
      for (const auto channels : {1u, 2u}) {
        auto result =
            Stream::fixture(filename(), rate, channels, scope == "pause");
        check(result.at("outputSamples").get<uint64_t>() ==
                  (scope == "pause" ? 272000u : 288000u),
              "Aligned sample count differs from common cut");
        check(result.at("finalized") == true, "WAV did not finalize");
        cases.push_back(std::move(result));
      }
  } else if (scope == "clock") {
    for (double ppm : {-150., 150.}) {
      auto r = Stream::fixture(filename(), 48000, 2, false, ppm);
      check(std::abs(r.at("observedDriftPpm").get<double>() - ppm) < 1,
            "Device/QPC drift conversion mismatch");
      check(std::abs(r.at("driftCorrectionPpm").get<double>()) <= 200,
            "Unbounded correction");
      cases.push_back(r);
    }
    for (int fault : {1, 2, 3, 6, 7}) {
      bool failed{};
      try {
        Stream::fixture(filename(), 48000, 2, false, 0, fault);
      } catch (const Error &e) {
        failed = e.stage == (fault == 6 ? NSS_STAGE_STORAGE : NSS_STAGE_AUDIO);
      }
      check(failed, "Invalid clock/lost sample/WAV write was accepted");
      cases.push_back({{"expectedFault", fault}, {"passed", true}});
    }
    bool exceeded{};
    try {
      Stream::fixture(filename(), 48000, 2, false, 300);
    } catch (const Error &e) {
      exceeded = e.stage == NSS_STAGE_AUDIO;
    }
    check(exceeded, "Out-of-budget drift was accepted");
    cases.push_back(Stream::fixture(filename(), 48000, 2, false, 0, 4));
    cases.push_back(Stream::fixture(filename(), 48000, 2, false, 0, 5));
  } else if (scope == "rf64") {
    const auto samples = uint64_t(48000) * 3600 * 8, bytes = samples * 4;
    const auto sizes = Wave::sizes(samples);
    check(bytes > 0xffffffff && sizes[0] == bytes + 72 && sizes[1] == bytes &&
              sizes[2] == samples,
          "RF64 64-bit sizes wrapped");
    cases.push_back(
        {{"samples", samples},
         {"dataBytes", bytes},
         {"riffBytes", sizes[0]},
         {"scope", "64-bit size arithmetic, not an eight-hour recording"}});
  } else
    throw std::invalid_argument("Unknown audio qualification scope");
  return {{"status", "PASS"}, {"scope", scope}, {"cases", cases}};
}
#endif
} // namespace nss::audio
