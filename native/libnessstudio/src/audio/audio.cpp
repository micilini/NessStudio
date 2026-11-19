#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "audio.hpp"
#include "sinc_kernel.hpp"
#include "packet_queue.hpp"
#include "clock_rate.hpp"
#include "ness_buffered_io.hpp"
#include "capture/capture.hpp"
#include <windows.h>
// clang-format off
#include <audioclient.h>
#include <avrt.h>
#include <initguid.h>
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
  bool finalized_{};
  float output_gain_{1};
  std::unique_ptr<nessmuxer::io::BufferedWrites> writes_;
  uint64_t file_position_{};
#ifdef NSS_AUDIO_QUALIFICATION
  uint32_t qualification_open_stall_ms_{};
  uint32_t qualification_write_stall_ms_{};
#endif
  void write_at(uint64_t offset, std::span<const uint8_t> bytes) {
    LARGE_INTEGER at{}; at.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(file_, at, nullptr, FILE_BEGIN))
      throw Error(NSS_E_IO, NSS_STAGE_STORAGE, HRESULT_FROM_WIN32(GetLastError()), "WAV seek failed");
#ifdef NSS_AUDIO_QUALIFICATION
    if (const auto delay = std::exchange(qualification_write_stall_ms_, 0u))
      std::this_thread::sleep_for(std::chrono::milliseconds(delay));
#endif
    DWORD done{};
    const auto size = static_cast<DWORD>(bytes.size());
    const bool written = WriteFile(file_, bytes.data(), size, &done, nullptr) != FALSE;
    const auto error = written ? (done == size ? ERROR_SUCCESS : ERROR_WRITE_FAULT) : GetLastError();
    if (error != ERROR_SUCCESS)
      throw Error(NSS_E_IO, NSS_STAGE_STORAGE,
                  HRESULT_FROM_WIN32(error), "WAV write failed");
  }
  void io(const void *data, DWORD size) {
    try { writes_->write(file_position_, {static_cast<const uint8_t *>(data), size}); }
    catch (const nessmuxer::io::BufferFull &) {
      throw Error(NSS_E_BACKPRESSURE, NSS_STAGE_STORAGE, 0,
          "Recording storage backlog exceeded its 8 MiB audio output buffer");
    }
    file_position_ += size;
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
  void set_open_stall_for_test(uint32_t milliseconds) { qualification_open_stall_ms_ = milliseconds; }
  void set_write_stall_for_test(uint32_t milliseconds) { qualification_write_stall_ms_ = milliseconds; }
  void invalidate_for_test() {
    writes_->drain();
    CloseHandle(std::exchange(file_, INVALID_HANDLE_VALUE));
  }
#endif
  ~Wave() {
    if (writes_) { try { writes_->finish(); } catch (...) {} }
    if (file_ != INVALID_HANDLE_VALUE)
      CloseHandle(file_);
  }
  void open(const std::string &path, float gain = 1) {
    if (!std::isfinite(gain) || gain < 0 || gain > 1)
      throw std::invalid_argument("Recording audio gain must be between 0 and 1");
    output_gain_ = gain;
#ifdef NSS_AUDIO_QUALIFICATION
    if (qualification_open_stall_ms_)
      std::this_thread::sleep_for(std::chrono::milliseconds(qualification_open_stall_ms_));
#endif
    file_ = CreateFileW(wide(path).c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                        nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file_ == INVALID_HANDLE_VALUE)
      throw Error(NSS_E_IO, NSS_STAGE_STORAGE,
                  HRESULT_FROM_WIN32(GetLastError()),
                  "Cannot create exclusive WAV");
    writes_ = std::make_unique<nessmuxer::io::BufferedWrites>(8 * 1024 * 1024,
        [this](uint64_t offset, std::span<const uint8_t> bytes) { write_at(offset, bytes); });
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
  double maximum_write_ms() const { return writes_ ? writes_->stats().maximum_write_ms : 0; }
  Json storage_report() const {
    const auto stats = writes_ ? writes_->stats() : nessmuxer::io::BufferedWrites::Stats{};
    return {{"capacityBytes", stats.capacity_bytes}, {"pendingBytes", stats.pending_bytes},
            {"peakPendingBytes", stats.peak_pending_bytes}, {"submittedBytes", stats.submitted_bytes},
            {"completedBytes", stats.completed_bytes}, {"maximumWriteMs", stats.maximum_write_ms},
            {"workerThreads", 1}, {"timelineChanged", false}};
  }
  void check() const { if (writes_) writes_->check(); }
  void append(std::array<float, 2> frame) {
    const float gain =
        static_cast<float>((std::min)(++ramp_, uint64_t{96})) / 96.f;
    for (auto &v : frame) {
      v *= gain;
      v *= output_gain_;
    }
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
    writes_->drain();
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
      file_position_ = static_cast<uint64_t>(p.QuadPart);
      io(last.data(), static_cast<DWORD>(n * 4));
      writes_->drain();
    }
    const auto lengths = sizes(samples);
    p.QuadPart = 20;
    hr(SetFilePointerEx(file_, p, nullptr, FILE_BEGIN) ? S_OK : E_FAIL,
       "RF64 seek failed");
    file_position_ = 20;
    io(lengths.data(), 24);
    writes_->finish();
    if (!FlushFileBuffers(file_))
      hr(HRESULT_FROM_WIN32(GetLastError()), "WAV flush failed");
    const auto f = std::exchange(file_, INVALID_HANDLE_VALUE);
    if (!CloseHandle(f))
      hr(HRESULT_FROM_WIN32(GetLastError()), "WAV close failed");
    finalized_ = true;
  }
  bool finalized() const { return finalized_; }
};
// Shared capture can deliver converted PCM while packet positions still use
// the endpoint's hardware sample rate. Keep that clock separate from PCM indices.
class PacketPosition {
  uint32_t client_rate_{}, endpoint_rate_{}, position_rate_{}, previous_frames_{};
  uint64_t anchor_raw_{}, anchor_pcm_{}, previous_raw_{}, next_pcm_{}, previous_qpc_{};
  uint64_t normalized_positions_{}, maximum_normalized_deviation_{};
  bool initialized_{}, selected_{}, initial_discontinuity_{}, previous_valid_{};
  bool normalized_{};
public:
  void configure(uint32_t client, uint32_t endpoint) {
    client_rate_ = position_rate_ = client;
    endpoint_rate_ = endpoint ? endpoint : client;
    selected_ = endpoint_rate_ == client_rate_;
  }
  uint32_t rate() const { return position_rate_; }
  bool selected() const { return selected_; }
  bool normalized() const { return normalized_; }
  Json diagnostics() const {
    return {{"normalizedPackets", normalized_positions_},
            {"maximumNormalizedPcmFrames", maximum_normalized_deviation_},
            {"cumulativeAnchorPreserved", true}, {"rawPositionsPreserved", true}};
  }
  uint64_t map(uint64_t raw, uint32_t frames, uint64_t qpc,
               bool discontinuity, bool valid) {
    normalized_ = false;
    if (!initialized_) {
      initialized_ = true;
      anchor_raw_ = previous_raw_ = raw;
      previous_frames_ = frames;
      next_pcm_ = frames;
      previous_qpc_ = qpc;
      previous_valid_ = valid;
      initial_discontinuity_ = discontinuity;
      return 0;
    }
    if (raw < previous_raw_)
      throw Error(NSS_E_IO, NSS_STAGE_AUDIO, 0, "Audio device position moved backwards");
    const auto delta = raw - previous_raw_;
    if (!selected_) {
      const auto converted = scale_floor(delta, client_rate_, endpoint_rate_);
      const auto tolerance = (client_rate_ + endpoint_rate_ - 1) / endpoint_rate_;
      const bool regular = converted + tolerance >= previous_frames_ &&
                           converted <= uint64_t(previous_frames_) + tolerance;
      // A converter may expose a short initial packet after its startup cut.
      // Accept at most 1ms here, once, only with the initial discontinuity flag.
      const bool initial_cut = initial_discontinuity_ && converted >= previous_frames_ &&
                               converted - previous_frames_ <= client_rate_ / 1000;
      // Initial clock-domain selection needs the same bounded jitter tolerance
      // as placement. A noisy second QPC must not misclassify endpoint ticks as
      // PCM frames when the packet sample counts prove the conversion ratio.
      const bool correlated = valid && previous_valid_ &&
          std::abs(double(static_cast<int64_t>(qpc) - static_cast<int64_t>(previous_qpc_)) -
                   double(delta) * hns_per_second / endpoint_rate_) <= 700000;
      if (delta != previous_frames_ && (regular || initial_cut) && correlated) {
        position_rate_ = endpoint_rate_;
        if (initial_cut) { anchor_raw_ = raw; anchor_pcm_ = next_pcm_; }
      }
      selected_ = true;
    }
    const auto relative = scale_floor(raw - anchor_raw_, client_rate_, position_rate_);
    if (relative > (std::numeric_limits<uint64_t>::max)() - anchor_pcm_)
      throw Error(NSS_E_IO, NSS_STAGE_AUDIO, 0, "Audio sample position overflow");
    auto pcm = anchor_pcm_ + relative;
    {
      // Test against the cumulative anchor so rounding cannot conceal a small
      // loss on every packet. One hardware sample is the position resolution.
      const auto tolerance = (client_rate_ + position_rate_ - 1) / position_rate_;
      const auto deviation = pcm >= next_pcm_ ? pcm - next_pcm_ : next_pcm_ - pcm;
      const bool correlated = valid && previous_valid_ &&
          std::abs(double(static_cast<int64_t>(qpc) - static_cast<int64_t>(previous_qpc_)) -
                   double(previous_frames_) * hns_per_second / client_rate_) <= 700000;
      if (deviation <= tolerance && (position_rate_ != client_rate_ || correlated)) {
        if (deviation) { normalized_ = true; ++normalized_positions_; maximum_normalized_deviation_ = (std::max)(maximum_normalized_deviation_, deviation); }
        pcm = next_pcm_;
      }
    }
    if (pcm > (std::numeric_limits<uint64_t>::max)() - frames)
      throw Error(NSS_E_IO, NSS_STAGE_AUDIO, 0, "Audio sample position overflow");
    next_pcm_ = pcm + frames;
    previous_raw_ = raw;
    previous_frames_ = frames;
    previous_qpc_ = qpc;
    previous_valid_ = valid;
    return pcm;
  }
};
uint32_t endpoint_sample_rate(IMMDevice *device, uint32_t fallback) {
  ComPtr<IPropertyStore> properties;
  if (FAILED(device->OpenPropertyStore(STGM_READ, &properties))) return fallback;
  PROPVARIANT value{};
  uint32_t rate = fallback;
  if (SUCCEEDED(properties->GetValue(PKEY_AudioEngine_DeviceFormat, &value)) &&
      value.vt == VT_BLOB && value.blob.pBlobData && value.blob.cbSize >= sizeof(WAVEFORMATEX)) {
    WAVEFORMATEX format{};
    std::memcpy(&format, value.blob.pBlobData, sizeof(format));
    if (format.nSamplesPerSec >= 8000 && format.nSamplesPerSec <= 192000)
      rate = format.nSamplesPerSec;
  }
  PropVariantClear(&value);
  return rate;
}
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
      timestamp_errors_{}, idle_samples_{}, continuous_timing_discontinuities_{};
  uint64_t recovered_gap_packets_{}, recovered_gap_samples_{}, recovered_gap_silence_{};
  int64_t last_qpc_{}, last_valid_qpc_{};
  int64_t maximum_timestamp_residual_hns_{}, last_clock_observation_qpc_{};
  uint64_t smoothed_timestamp_packets_{}, rejected_clock_observations_{};
  double timestamp_fraction_hns_{};
  ClockRate clock_rate_;
  PacketPosition packet_position_;
  uint64_t last_valid_position_{}, previous_frames_{}, idle_intervals_{},
      lost_samples_{}, terminal_pad_{}, keepalive_frames_{}, exact_silence_outputs_{},
      pcm_zero_outputs_{}, filtered_outputs_{};
  double phase_{}, ppm_{}, observed_ppm_{}, maximum_observed_absolute_ppm_{};
  uint64_t limited_correction_packets_{};
  uint64_t segment_{~uint64_t{}};
  bool segment_open_{}, loopback_{};
  uint32_t rate_{};
  std::optional<SincKernel> kernel_;
  int64_t last_report_at_{};
  uint64_t report_publications_{};
  Json clock_report() const {
    return {{"method", "centered rolling device-position/QPC regression"},
            {"maximumWindowSeconds", 30}, {"observationLimit", 640},
            {"momentRebuildEveryObservations", 256},
            {"observations", clock_rate_.observations()}, {"spanSeconds", clock_rate_.span_seconds()},
            {"rawTimestampsChanged", false}, {"correctionBudgetPpm", 200},
            {"limitedCorrectionPackets", limited_correction_packets_},
            {"maximumObservedAbsolutePpm", maximum_observed_absolute_ppm_},
            {"excessEstimatePolicy", "Limit correction and retain diagnostics; sample continuity and bounded clock divergence determine capture validity"}};
  }
  Json timestamp_report() const {
    return {{"method", "Continuous device samples determine placement; raw QPC observations remain diagnostic"},
            {"smoothingToleranceMs", 70}, {"clockObservationOutlierThresholdMs", 2},
            {"smoothedPackets", smoothed_timestamp_packets_},
            {"rejectedClockObservations", rejected_clock_observations_},
            {"maximumRawResidualMs", double(maximum_timestamp_residual_hns_) / 10000},
            {"rawPacketTimestampsPreserved", true}};
  }
  std::atomic<int64_t> file_preparation_hns_{}, endpoint_preparation_started_{}, endpoint_preparation_hns_{};
  Json preparation_report() const {
    return {{"filePreparationMs", double(file_preparation_hns_.load()) / 10000},
            {"endpointPreparationMs", double(endpoint_preparation_hns_.load()) / 10000},
            {"endpointDeadlineMs", 3000},
            {"deadlineScope", "Audio device initialization; output file I/O completes before this wait"}};
  }
  Json handoff_report() const {
    const auto stats = packet_queue_ ? packet_queue_->stats() : PacketQueue::Stats{};
    return {{"captureBufferFrames", capture_buffer_frames_},
            {"queuedFrames", stats.queued_frames}, {"peakQueuedFrames", stats.peak_frames},
            {"queuedPackets", stats.queued_packets}, {"peakQueuedPackets", stats.peak_packets},
            {"pushedPackets", stats.pushed}, {"processedOrInProgressPackets", stats.popped},
            {"allocatedQueueBytes", stats.bytes}, {"pendingDurationLimitMs", packet_queue_
                ? packet_queue_->pending_duration_limit_ms() : PacketQueue::recording_pending_ms},
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
               {"gain", plan_.value("gain", 1.f)},
               {"channelMapping", format.channels == 1
                                      ? "mono duplicated to L/R"
                                      : "L/R preserved"},
               {"deviceMixFormat", mix_format_},
               {"packetPositionRate", packet_position_.rate()},
               {"packetPositionNormalization", packet_position_.diagnostics()},
               {"wasapiFormatConversion", false},
               {"reportPublishedQpcHns", now},
               {"regularReportIntervalMs", 1000},
               {"reportPublicationCount", report_publications_},
               {"packetHandoff", handoff_report()},
               {"storageWrites", wave_.storage_report()},
               {"preparation", preparation_report()},
               {"packetTimestamps", packet_timestamps_},
               {"recentPacketTimestamps", recent_timestamps_},
               {"packets", packets_},
               {"inputSamples", captured_},
               {"outputSamples", wave_.count()},
               {"silentFlagSamples", silent_},
               {"pausedInputSamplesRemoved", paused_samples_},
               {"deviceGapSamples", gap_samples_},
               {"discontinuities", discontinuities_},
               {"discontinuityFlagsWithContinuousTiming", continuous_timing_discontinuities_},
               {"timestampErrors", timestamp_errors_},
               {"timestampPlacement", timestamp_report()},
               {"initialOrLoopbackIdleSilenceSamples", idle_samples_},
               {"driftCorrectionPpm", ppm_},
               {"observedDriftPpm", observed_ppm_},
               {"clockRateEstimator", clock_report()},
               {"loopbackNoPacketIdleIntervals", idle_intervals_},
               {"lostSamples", lost_samples_},
               {"recoveredAudioGaps", {{"packets", recovered_gap_packets_},
                   {"missingInputFrames", recovered_gap_samples_}, {"insertedSilentOutputFrames", recovered_gap_silence_},
                   {"maximumGapMs", 200}, {"clockCorrelationToleranceMs", 2},
                   {"policy", "Retain delivered PCM; fill bounded, driver-flagged forward gaps with silence"}}},
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
    const auto raw_position = position;
    const auto raw_qpc = qpc;
    position = packet_position_.map(position, frames, qpc,
        (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) != 0,
        (flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) == 0);
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
    }
    const bool position_gap = next_device_ && position != next_device_;
    const bool observed_loopback_idle = loopback_ && observed_idle_hns >= 300000;
    bool reliable_clock_observation = !(flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) && !packet_position_.normalized();
    if (last_qpc_ && !position_gap && !observed_loopback_idle) {
      // Packet QPC values can jitter even without TIMESTAMP_ERROR. Device
      // positions prove whether PCM is missing. Keep contiguous samples on a
      // sample-count timeline instead of opening a false hole at every jitter.
      // Accumulate only the fractional delta, avoiding precision loss from
      // repeatedly adding small floating-point values to a large QPC epoch.
      const double delta = double(previous_frames_) * hns_per_second /
          (rate_ * (1.0 + ppm_ * 1e-6)) + timestamp_fraction_hns_;
      const auto whole_delta = static_cast<int64_t>(std::llround(delta));
      if (last_qpc_ > (std::numeric_limits<int64_t>::max)() - whole_delta)
        throw Error(NSS_E_IO, NSS_STAGE_AUDIO, 0, "Audio placement timestamp overflow");
      const int64_t expected = last_qpc_ + whole_delta;
      const int64_t residual = static_cast<int64_t>(qpc) - expected;
      const auto magnitude = std::abs(residual);
      maximum_timestamp_residual_hns_ = (std::max)(maximum_timestamp_residual_hns_, magnitude);
      if (magnitude <= 700000) {
        if (residual) ++smoothed_timestamp_packets_;
        qpc = static_cast<uint64_t>(expected);
        timestamp_fraction_hns_ = delta - whole_delta;
        // Never train a rate estimate on a scheduling/timestamp spike or on
        // an extrapolated value. The original packet timestamp stays logged.
        if (magnitude > 20000) reliable_clock_observation = false;
      } else {
        throw Error(NSS_E_IO, NSS_STAGE_AUDIO, 0,
                    "Audio clock correlation exceeds the bounded 70 ms smoothing tolerance");
      }
    } else {
      timestamp_fraction_hns_ = 0;
    }
    if (last_qpc_ && static_cast<int64_t>(qpc) < last_qpc_)
      throw Error(NSS_E_IO, NSS_STAGE_AUDIO, 0,
                  "Audio timestamp moved backwards");
    const bool qpc_gap =
        last_qpc_ && static_cast<int64_t>(qpc) - last_qpc_ -
                             sample_time(previous_frames_, rate_) >
                         200000;
    bool recovered_gap = false;
    if (position_gap || qpc_gap) {
      if (position < next_device_)
        throw Error(NSS_E_IO, NSS_STAGE_AUDIO, 0,
                    "Audio device position moved backwards");
      const auto missing = position - next_device_;
      gap_samples_ += missing;
      // Idle loopback is established by repeated empty polls, never inferred
      // from a dropped packet alone.
      const bool observed_idle =
          loopback_ && observed_idle_hns >= 300000;
      // A driver-reported discontinuity can lose a short packet without
      // invalidating the video and all remaining audio. Require agreement
      // between positions and the valid QPC, bound the hole, and preserve it
      // as explicit silence rather than compressing time or dropping PCM.
      const auto correlated_qpc = missing <= rate_ / 5
          ? last_qpc_ + sample_time(previous_frames_ + missing, rate_) : last_qpc_;
      recovered_gap = !observed_idle && missing > 0 && missing <= rate_ / 5 &&
          (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) &&
          !(flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) &&
          std::abs(static_cast<int64_t>(qpc) - correlated_qpc) <= 20000;
      if (!observed_idle && !recovered_gap) {
        lost_samples_ += missing;
        const auto detail =
            "Mandatory audio timestamp/sample gap: position=" +
            std::to_string(position) +
            " expected=" + std::to_string(next_device_) + " qpcDeltaHns=" +
            std::to_string(static_cast<int64_t>(qpc) - last_qpc_) +
            " previousFrames=" + std::to_string(previous_frames_) +
            " flags=" + std::to_string(flags);
        throw Error(NSS_E_IO, NSS_STAGE_AUDIO, 0, detail.c_str());
      }
      if (recovered_gap) {
        ++recovered_gap_packets_;
        recovered_gap_samples_ += missing;
        lost_samples_ += missing;
        qpc = static_cast<uint64_t>(correlated_qpc);
        reliable_clock_observation = false;
      } else ++idle_intervals_;
      flush_segment(next_device_, true);
      clock_rate_.reset();
      last_clock_observation_qpc_ = 0;
    }
    if ((flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) && packets_ &&
        !(loopback_ && observed_idle_hns >= 300000)) {
      // WASAPI can flag a driver glitch even when the supplied device position
      // and QPC remain continuous. Preserve that diagnostic, and retain the
      // original PCM/timing without manufacturing silence or resetting phase.
      // Actual position/QPC gaps still fail above. An uncertain timestamp
      // cannot establish continuity and must not take this recovery path.
      if (flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR)
        throw Error(NSS_E_IO, NSS_STAGE_AUDIO, 0,
                    "Audio discontinuity has an uncertain timestamp");
      if (!recovered_gap) ++continuous_timing_discontinuities_;
    }
    if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY)
      ++discontinuities_;
    if (flags & AUDCLNT_BUFFERFLAGS_SILENT)
      silent_ += frames;
    if (!(flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR)) {
      last_valid_qpc_ = static_cast<int64_t>(qpc);
      last_valid_position_ = position;
    }
    // Extrapolated timestamps may preserve a bounded damaged packet, but must
    // not train the estimator from its own previous correction.
    if (reliable_clock_observation && static_cast<int64_t>(raw_qpc) >= last_clock_observation_qpc_ && packet_position_.selected()) {
      clock_rate_.observe(raw_position, static_cast<int64_t>(raw_qpc), packet_position_.rate());
      last_clock_observation_qpc_ = static_cast<int64_t>(raw_qpc);
    } else if (!(flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR)) {
      ++rejected_clock_observations_;
    }
    if (const auto measured=clock_rate_.ppm()) {
      observed_ppm_ = *measured;
      maximum_observed_absolute_ppm_ = (std::max)(maximum_observed_absolute_ppm_, std::abs(observed_ppm_));
      // A short-window rate estimate can react strongly to a small driver QPC
      // offset without any missing PCM. Bound the resampler correction, rather
      // than turning that estimate alone into a recording failure. Device gaps
      // and actual placement/QPC divergence are independently checked above.
      if (std::abs(observed_ppm_) > 200) ++limited_correction_packets_;
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
          if (recovered_gap) recovered_gap_silence_ += target - wave_.count();
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
      // Capture PCM in the mix format. Packet positions may retain the physical
      // endpoint clock when Windows applies conversion before delivering PCM.
      const DWORD streamflags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                                (loopback_ ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0);
      const Format format(raw);
      rate_ = format.rate;
      packet_position_.configure(rate_, endpoint_sample_rate(device.Get(), rate_));
      hr(client->Initialize(AUDCLNT_SHAREMODE_SHARED, streamflags, 2000000, 0,
                            raw, nullptr),
         "WASAPI shared initialization failed");
      hr(client->GetBufferSize(&capture_buffer_frames_), "Capture capacity unavailable");
      packet_queue_ = std::make_unique<PacketQueue>(format.rate, format.align,
          PacketQueue::recording_pending_ms);
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
      endpoint_preparation_hns_ = qpc_hns() - endpoint_preparation_started_.load();
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
              std::this_thread::sleep_for(std::chrono::milliseconds(750));
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
      report_["timestampPlacement"] = timestamp_report();
      report_["clockRateEstimator"] = clock_report();
      report_["driftCorrectionPpm"] = ppm_;
      report_["observedDriftPpm"] = observed_ppm_;
      report_["inputSamples"] = captured_;
      report_["outputSamples"] = wave_.count();
      report_["packets"] = packets_;
      report_["discontinuities"] = discontinuities_;
      report_["discontinuityFlagsWithContinuousTiming"] = continuous_timing_discontinuities_;
      report_["packetHandoff"] = handoff_report();
      report_["storageWrites"] = wave_.storage_report();
      report_["preparation"] = preparation_report();
      report_["kind"] = plan_.at("kind");
      report_["endpointId"] = plan_.at("endpointId");
      report_["firstError"] = {{"code", e.code},
                               {"stage", e.stage},
                               {"hresult", e.native},
                               {"message", e.what()}};
      fault_ = {e.code, e.stage, e.native, e.what()};
    } catch (const std::exception &e) {
      std::lock_guard lock(mutex_);
      report_["packetTimestamps"] = packet_timestamps_;
      report_["recentPacketTimestamps"] = recent_timestamps_;
      report_["timestampPlacement"] = timestamp_report();
      report_["clockRateEstimator"] = clock_report();
      report_["driftCorrectionPpm"] = ppm_;
      report_["observedDriftPpm"] = observed_ppm_;
      report_["packetHandoff"] = handoff_report();
      report_["storageWrites"] = wave_.storage_report();
      report_["preparation"] = preparation_report();
      report_["kind"] = plan_.at("kind");
      report_["endpointId"] = plan_.at("endpointId");
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
  static Json live_storage_stall(const std::string &filename) {
    const auto available = devices(NSS_SOURCE_LOOPBACK).at("devices");
    if (available.empty()) throw std::runtime_error("No loopback endpoint for storage qualification");
    auto endpoint = available.front();
    for (const auto &candidate : available) if (candidate.value("default", false)) endpoint = candidate;
    Stream stream({{"path", filename}, {"kind", "loopback"}, {"endpointId", endpoint.at("id")}});
    stream.wave_.set_write_stall_for_test(12000);
    stream.prepare();
    SessionClock clock;
    const auto begin = qpc_hns(); clock.start(begin); stream.start(clock);
    while (qpc_hns() - begin < 3 * hns_per_second) {
      stream.check(); std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    const auto end = qpc_hns(); clock.stop(end); stream.finish(clock.active_time(end));
    auto result = stream.snapshot();
    result["storageWrites"] = stream.wave_.storage_report();
    const auto &storage = result.at("storageWrites");
    if (storage.at("maximumWriteMs").get<double>() < 12000 ||
        storage.at("pendingBytes") != 0 || storage.at("submittedBytes") != storage.at("completedBytes") ||
        result.at("lostSamples") != 0 || result.at("deviceGapSamples") != 0 || result.at("finalized") != true)
      throw std::runtime_error("Audio was lost or finalization overtook the delayed storage writer");
    result["injectedStorageStallMs"] = 12000;
    return result;
  }
  static Json live_prepare_storage(const std::string &filename) {
    const auto available = devices(NSS_SOURCE_LOOPBACK).at("devices");
    if (available.empty()) throw std::runtime_error("No loopback endpoint for preparation qualification");
    auto endpoint = available.front();
    for (const auto &candidate : available) if (candidate.value("default", false)) endpoint = candidate;
    Stream stream({{"path", filename}, {"kind", "loopback"}, {"endpointId", endpoint.at("id")}});
    stream.wave_.set_open_stall_for_test(4500);
    stream.prepare();
    SessionClock clock;
    const auto begin = qpc_hns();
    clock.start(begin); stream.start(clock);
    while (qpc_hns() - begin < 2 * hns_per_second) {
      stream.check(); std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    const auto end = qpc_hns(); clock.stop(end); stream.finish(clock.active_time(end));
    auto result = stream.snapshot();
    if (result.at("preparation").at("filePreparationMs").get<double>() < 4500 ||
        result.at("preparation").at("endpointPreparationMs").get<double>() >= 3000 ||
        result.at("lostSamples") != 0 || result.at("finalized") != true)
      throw std::runtime_error("Slow output-file preparation consumed the audio device initialization deadline");
    result["injectedFilePreparationStallMs"] = 4500;
    return result;
  }
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
        report.at("packetHandoff").at("packetProcessingMaxMs").get<double>() < 750 ||
        report.at("packetHandoff").at("peakQueuedFrames").get<uint64_t>() < stream.rate_ / 2 ||
        report.at("packetHandoff").at("pendingDurationLimitMs") != PacketQueue::recording_pending_ms)
      throw std::runtime_error("Real audio handoff did not preserve packets through the owned 750ms stall");
    return report;
  }
  static Json fixture(const std::string &filename, uint32_t rate,
                      uint32_t channels, bool pause, double drift = 0,
                      int fault = 0, uint32_t position_rate = 0,
                      uint32_t endpoint_rate = 0, bool partial_first = false) {
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
    if (!position_rate) position_rate = rate;
    s.packet_position_.configure(rate, endpoint_rate ? endpoint_rate : position_rate);
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
      // Clock metadata noise must not change the actual source waveform.
      const auto physical_qpc = qpc;
      uint64_t position = scale_floor(pos, position_rate, rate);
      DWORD flags{};
      uint32_t frames = partial_first && pos == 0 ? quantum - 17 : quantum;
      if (partial_first && pos == 0) flags = AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY;
      if (fault == 1 && pos == 0)
        flags = AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR;
      if (fault == 2 && pos == quantum * 10)
        position += position_rate / 100;
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
      if (fault == 8 && pos == quantum * 10)
        position -= position_rate / 50;
      if (fault == 9 && pos == quantum * 10)
        flags = AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY;
      if (fault == 10 && pos >= quantum * 10)
        position += pos / quantum - 9;
      if (fault == 11 && pos == quantum * 10)
        flags = AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY | AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR;
      if (fault == 12 && pos == quantum * 10) {
        flags = AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY;
        position += position_rate / 100;
      }
      const auto packet_index = pos / quantum;
      const bool near_cut = packet_index == 10 || packet_index == 98 || packet_index == 131;
      if (fault == 13 && near_cut) qpc += 200759; // Physical HyperX failure: 20.0759 ms.
      if (fault == 14 && near_cut) qpc += 690000;
      if (fault == 15 && near_cut) qpc -= 50000;
      if (fault == 16 && packet_index % 3 != 0)
        qpc = packet_index % 3 == 1 ? qpc + 200759 : qpc - 50000;
      if (fault == 17 && packet_index >= 10) qpc += 200759;
      if (fault == 18 && packet_index == 10) qpc += 700001;
      if (fault == 19 && packet_index == 10) qpc -= 1000000;
      if (fault == 20 && near_cut) qpc += 700000;
      if (fault == 21 && packet_index >= 200) qpc += 16428;
      if (fault == 22 && packet_index >= 100)
        qpc = packet_index / 100 % 2 ? qpc + 8000 : qpc - 8000;
      const auto missing = uint64_t(position_rate / 1000);
      const bool after_gap = packet_index >= 200;
      if (fault == 29 && after_gap) {
        position += missing;
        qpc += packet_index == 200 ? 8085 : 10000;
        if (packet_index == 200) flags = AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY;
        if (pos + quantum >= uint64_t(rate) * 6) frames -= rate / 1000;
      }
      if (fault >= 30 && fault <= 33 && packet_index == 200) {
        const auto gap = fault == 30 ? position_rate / 4 : missing;
        position = fault == 33 ? position - gap : position + gap;
        qpc += fault == 30 ? hns_per_second / 4 : 10000;
        flags = fault == 31 ? 0 : AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY;
        if (fault == 32) flags |= AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR;
      }
      if (fault == 23 && packet_index) {
        // Reported Intel Smart Sound startup: position=481, expected=480,
        // QPC delta=100157 hns, discontinuity flag, with 480 delivered frames.
        ++position;
        qpc += 157;
        if (packet_index == 1) flags = AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY;
      }
      if (fault == 24 && packet_index % 2) ++position;
      if (fault == 25 && packet_index) position += packet_index;
      if (fault == 26 && packet_index == 1) {
        ++position;
        flags = AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR | AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY;
      }
      if (fault == 27 && packet_index == 1) { ++position; qpc += hns_per_second; }
      if (fault == 28 && packet_index) --position;
      for (uint32_t i = 0; i < quantum; ++i) {
        const auto at = physical_qpc + static_cast<uint64_t>(sample_time(i, rate));
        const double hz = pause && at >= uint64_t(epoch + 12345678) &&
                                  at < uint64_t(epoch + 15679011)
                              ? 4000
                              : 1000;
        const float value = static_cast<float>(
            .25 * std::sin(2 * std::numbers::pi * hz * double(pos + i +
                (fault == 29 && after_gap ? rate / 1000 : 0)) / rate));
        for (uint32_t c = 0; c < channels; ++c)
          std::memcpy(copy.data() + (size_t(i) * channels + c) * 4, &value, 4);
      }
      s.packet(copy, frames, flags, position, qpc, format);
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
    // Open/write the owned output before starting the endpoint deadline. A
    // delayed disk write is not an audio-device initialization failure. This
    // runs on the native preparation worker, never the application UI thread.
    const auto file_started = qpc_hns();
    wave_.open(plan_.at("path"), plan_.value("gain", 1.f));
    file_preparation_hns_ = qpc_hns() - file_started;
    endpoint_preparation_started_ = qpc_hns();
    thread_ = std::thread([this] { run(); });
    std::unique_lock lock(mutex_);
    if (!changed_.wait_for(lock, std::chrono::seconds(3),
                           [&] { return ready_; }))
      throw Error(NSS_E_TIMEOUT, NSS_STAGE_AUDIO, 0, "Audio device initialization timed out (3000 ms)");
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
    wave_.check();
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
    report_["storageWrites"] = wave_.storage_report();
    report_["packetHandoff"] = handoff_report();
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
    if (!item.is_object() || item.size() != 3 + size_t(item.contains("gain")) || !item.contains("kind") ||
        !item.contains("endpointId") || !item.contains("path"))
      throw std::invalid_argument(
          "Audio source requires kind, endpointId and path");
    if (item.contains("gain") && (!item.at("gain").is_number() ||
        !std::isfinite(item.at("gain").get<double>()) || item.at("gain").get<double>() < 0 ||
        item.at("gain").get<double>() > 1))
      throw std::invalid_argument("Recording audio gain must be between 0 and 1");
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
  } else if(scope == "storage-stall-live") {
    cases.push_back(Stream::live_storage_stall(filename()));
  } else if(scope == "prepare-storage-live") {
    cases.push_back(Stream::live_prepare_storage(filename()));
  } else if(scope == "pcm-store") {
    for (const float recording_gain : {0.f, .25f, .8f, 1.f}) {
    const auto path=filename();
    Wave wave;wave.open(path, recording_gain);
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
          value*=recording_gain;
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
    cases.push_back({{"path",path},{"gain",recording_gain},{"pcmFramesCompared",expected.size()/2},
      {"bitExactClosedFormReference",true},{"tailStorage", "fixed96frame ring"}});
    }
    for (const float invalid_gain : {-.1f, 1.1f, std::numeric_limits<float>::infinity(),
                                    std::numeric_limits<float>::quiet_NaN()}) {
      const auto path = filename();
      bool rejected{};
      try { Wave wave; wave.open(path, invalid_gain); }
      catch (const std::invalid_argument &) { rejected = true; }
      check(rejected && !std::filesystem::exists(wide(path)), "Invalid audio gain opened an output file");
    }
    cases.push_back({{"invalidGainsRejectedBeforeFileOpen", true}, {"unityGainPreservesExistingPcm", true}});
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
  } else if (scope == "position-domain") {
    for (const auto rate : {48000u, 16000u, 44100u, 96000u}) {
      for (const bool paused : {false, true}) {
        const auto reference_path = filename();
        const auto reference = Stream::fixture(reference_path, rate, 2, paused);
        auto bytes = [](const std::string &path) {
          std::ifstream input(std::filesystem::path(wide(path)), std::ios::binary);
          return std::vector<char>((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        };
        const auto reference_bytes = bytes(reference_path);
        for (const int fault : {23, 24, 28}) {
          const auto path = filename();
          auto quantized = Stream::fixture(path, rate, 2, paused, 0, fault);
          check(quantized.at("lostSamples") == 0 && quantized.at("deviceGapSamples") == 0 &&
                quantized.at("finalized") == true && quantized.at("inputSamples") == reference.at("inputSamples") &&
                quantized.at("outputSamples") == reference.at("outputSamples") && bytes(path) == reference_bytes,
                "One-frame device position quantization changed or discarded the delivered PCM");
          quantized["qualificationFault"] = fault;
          quantized["bitExactWithoutPositionQuantization"] = true;
          cases.push_back(std::move(quantized));
        }
      }
    }
    for (const int fault : {25, 26, 27}) {
      bool failed{};
      try { Stream::fixture(filename(), 48000, 2, false, 0, fault); }
      catch (const Error &error) { failed = error.stage == NSS_STAGE_AUDIO; }
      check(failed, "Position normalization concealed cumulative loss or an unreliable clock");
      cases.push_back({{"expectedFault", fault}, {"passed", true}});
    }
    for (const auto rate : {16000u, 48000u}) {
      for (const bool paused : {false, true}) {
        const auto path = filename();
        auto repaired = Stream::fixture(path, rate, 2, paused, 0, 29);
        const auto &recovery = repaired.at("recoveredAudioGaps");
        // Pause cuts and rate conversion quantize boundaries to input frames.
        const uint64_t rounding = (48000u + rate - 1) / rate;
        check(repaired.at("lostSamples") == rate / 1000 && repaired.at("deviceGapSamples") == rate / 1000 &&
              recovery.at("packets") == 1 && recovery.at("missingInputFrames") == rate / 1000 &&
              recovery.at("insertedSilentOutputFrames").get<uint64_t>() >= 48 - rounding &&
              recovery.at("insertedSilentOutputFrames").get<uint64_t>() <= 48 + rounding && repaired.at("finalized") == true &&
              repaired.at("outputSamples") == (paused ? 272000u : 288000u),
              ("A correlated one-ms driver discontinuity did not preserve the common recording cut: rate=" +
               std::to_string(rate) + " paused=" + std::to_string(paused) + " lost=" +
               repaired.at("lostSamples").dump() + " deviceGap=" + repaired.at("deviceGapSamples").dump() +
               " recovery=" + recovery.dump() + " output=" + repaired.at("outputSamples").dump()).c_str());
        if (!paused) {
          std::ifstream file(std::filesystem::path(wide(path)), std::ios::binary);
          file.seekg(80 + (12000 + 96000) * 4);
          std::array<int16_t, 96> silence{};
          file.read(reinterpret_cast<char *>(silence.data()), sizeof(silence));
          check(file.gcount() == sizeof(silence) &&
                std::all_of(silence.begin(), silence.end(), [](int16_t sample) { return sample == 0; }),
                "Recovered audio gap was not filled with exactly timed silent PCM");
        }
        repaired["oneMillisecondDriverGapPreservedAsSilence"] = true;
        cases.push_back(std::move(repaired));
      }
    }
    for (const int fault : {30, 31, 32, 33}) {
      bool failed{};
      try { Stream::fixture(filename(), 48000, 2, false, 0, fault); }
      catch (const Error &error) { failed = error.stage == NSS_STAGE_AUDIO; }
      check(failed, "Discontinuity recovery accepted a large, unflagged, uncertain or backwards gap");
      cases.push_back({{"expectedFault", fault}, {"passed", true}});
    }
    for (const auto rates : {std::array<uint32_t, 3>{48000, 16000, 16000},
                            {16000, 48000, 48000}, {44100, 48000, 48000},
                            {48000, 44100, 44100}, {48000, 48000, 16000}}) {
      for (const double drift : {-150., 0., 150.}) {
        auto result = Stream::fixture(filename(), rates[0], 2, false, drift, 0, rates[1], rates[2]);
        check(result.at("packetPositionRate") == rates[1], "Packet clock domain was misidentified");
        check(result.at("lostSamples") == 0 && result.at("deviceGapSamples") == 0,
              "Converted packets were mistaken for lost PCM");
        check(result.at("outputSamples") == 288000 && result.at("finalized") == true,
              "Converted packet stream did not finalize at the common cut");
        check(std::abs(result.at("observedDriftPpm").get<double>() - drift) < 1,
              "Converted packet clock changed drift measurement");
        cases.push_back(std::move(result));
      }
    }
    auto partial = Stream::fixture(filename(), 48000, 2, false, 0, 0, 16000, 16000, true);
    check(partial.at("inputSamples") == 288000 - 17 && partial.at("lostSamples") == 0 &&
          partial.at("deviceGapSamples") == 0 && partial.at("packetPositionRate") == 16000,
          "Initial converter cut discarded or invented PCM samples");
    cases.push_back(std::move(partial));
    for (const auto position_rate : {16000u, 48000u}) {
      const auto reference_path = filename(), flagged_path = filename();
      const auto reference = Stream::fixture(reference_path, 48000, 2, false, 0, 0, position_rate);
      auto flagged = Stream::fixture(flagged_path, 48000, 2, false, 0, 9, position_rate);
      auto bytes = [](const std::string &path) {
        std::ifstream input(std::filesystem::path(wide(path)), std::ios::binary);
        return std::vector<char>((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
      };
      check(flagged.at("discontinuityFlagsWithContinuousTiming") == 1 &&
            flagged.at("discontinuities") == 1 && flagged.at("lostSamples") == 0 &&
            flagged.at("deviceGapSamples") == 0 && flagged.at("timestampErrors") == 0,
            "A continuous-timing driver flag lost its diagnostic or changed the timeline");
      check(flagged.at("outputSamples") == reference.at("outputSamples") &&
            bytes(reference_path) == bytes(flagged_path),
            "A continuous-timing driver flag changed the recorded PCM/RF64 bytes");
      flagged["bitExactWithoutDiscontinuityFlag"] = true;
      cases.push_back(std::move(flagged));
    }
    for (const auto position_rate : {16000u, 48000u}) {
      for (const bool paused : {false, true}) {
        const auto reference_path = filename();
        const auto reference = Stream::fixture(reference_path, 48000, 2, paused, 0, 0, position_rate);
        auto bytes = [](const std::string &path) {
          std::ifstream input(std::filesystem::path(wide(path)), std::ios::binary);
          return std::vector<char>((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        };
        const auto reference_bytes = bytes(reference_path);
        for (const int fault : {13, 14, 15, 16, 17, 20}) {
          const auto jittered_path = filename();
          auto jittered = Stream::fixture(jittered_path, 48000, 2, paused, 0, fault, position_rate);
          const auto &placement = jittered.at("timestampPlacement");
          check(jittered.at("lostSamples") == 0 && jittered.at("deviceGapSamples") == 0 &&
                jittered.at("timestampErrors") == 0 && jittered.at("finalized") == true,
                "Bounded QPC jitter interrupted continuous PCM samples");
          check(placement.at("smoothedPackets").get<uint64_t>() > 0 &&
                placement.at("rejectedClockObservations").get<uint64_t>() > 0 &&
                placement.at("maximumRawResidualMs").get<double>() <= 70.00001 &&
                placement.at("rawPacketTimestampsPreserved") == true,
                "QPC jitter was hidden or trained the audio clock estimator");
          check(jittered.at("packetTimestamps").at(10).at("qpcHns") !=
                    reference.at("packetTimestamps").at(10).at("qpcHns"),
                "Original jittered packet QPC was overwritten in diagnostics");
          check(jittered.at("outputSamples") == reference.at("outputSamples") &&
                jittered.at("pausedInputSamplesRemoved") == reference.at("pausedInputSamplesRemoved") &&
                bytes(jittered_path) == reference_bytes,
                "Bounded QPC jitter changed PCM/RF64 bytes or a pause boundary");
          jittered["qualificationFault"] = fault;
          jittered["pauseBoundariesVerified"] = paused;
          jittered["bitExactWithoutTimestampJitter"] = true;
          cases.push_back(std::move(jittered));
        }
      }
    }
    for (int fault : {1, 2, 3, 7, 8, 10, 11, 12, 18, 19}) {
      bool failed{};
      try { Stream::fixture(filename(), 48000, 2, false, 0, fault, 16000); }
      catch (const Error &e) { failed = e.stage == NSS_STAGE_AUDIO; }
      check(failed, "Converted clock concealed a real audio fault");
      cases.push_back({{"expectedFault", fault}, {"passed", true}});
    }
    cases.push_back(Stream::fixture(filename(), 48000, 2, false, 0, 4, 16000));
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
    // A real sustained clock-rate difference is observable and correction is
    // bounded; it must not discard continuous source samples merely because
    // a ppm estimate crosses the resampler's limit.
    auto limited = Stream::fixture(filename(), 48000, 2, false, 300);
    check(std::abs(limited.at("observedDriftPpm").get<double>() - 300) < 1 &&
          std::abs(limited.at("driftCorrectionPpm").get<double>()) <= 200 &&
          limited.at("clockRateEstimator").at("limitedCorrectionPackets").get<uint64_t>() > 0 &&
          limited.at("lostSamples") == 0 && limited.at("deviceGapSamples") == 0 &&
          limited.at("outputSamples") == 288000 && limited.at("finalized") == true,
          "Out-of-budget estimate lost continuous audio or exceeded the correction bound");
    cases.push_back(std::move(limited));
    for (const int fault : {21, 22}) {
      auto noisy = Stream::fixture(filename(), 48000, 2, false, 0, fault);
      check(noisy.at("clockRateEstimator").at("limitedCorrectionPackets").get<uint64_t>() > 0 &&
            noisy.at("clockRateEstimator").at("maximumObservedAbsolutePpm").get<double>() > 200 &&
            std::abs(noisy.at("driftCorrectionPpm").get<double>()) <= 200 &&
            noisy.at("timestampPlacement").at("maximumRawResidualMs").get<double>() < 3 &&
            noisy.at("lostSamples") == 0 && noisy.at("deviceGapSamples") == 0 &&
            noisy.at("outputSamples") == 288000 && noisy.at("finalized") == true,
            "Small QPC offsets became fatal clock drift or lost PCM");
      noisy["qualificationFault"] = fault;
      cases.push_back(std::move(noisy));
    }
    bool exceeded{};
    try {
      Stream::fixture(filename(), 48000, 2, false, 20000);
    } catch (const Error &e) {
      exceeded = e.stage == NSS_STAGE_AUDIO;
    }
    check(exceeded, "Actual clock divergence beyond the smoothing tolerance was accepted");
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
