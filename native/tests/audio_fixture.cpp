#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "capture_fixture.hpp"
#include <audioclient.h>
#include <avrt.h>
#include <cmath>
#include <fstream>
#include <iostream>
#include <mmdeviceapi.h>
#include <numbers>
#include <wrl/client.h>

using fixture::check;
using Microsoft::WRL::ComPtr;
using nss::capture::Json;
static void hr(HRESULT r, const char *s) { check(SUCCEEDED(r), s); }
int wmain(int argc, wchar_t **argv) {
  std::cout << std::unitbuf;
  try {
    check(argc == 5 || argc == 6,
          "Usage: audio-fixture STOP REFERENCE_BGRA REPORT ENDPOINT_ID [idle]");
    const bool idle = argc == 6;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    hr(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "Fixture COM failed");
    struct Apartment {
      ~Apartment() { CoUninitialize(); }
    } apartment;
    const auto display = nss::capture::displays().front();
    fixture::Window window(display.bounds);
    window.pattern(0);
    {
      const auto image = window.reference();
      std::ofstream f(std::filesystem::path(argv[2]), std::ios::binary);
      f.write(reinterpret_cast<const char *>(image.data()),
              static_cast<std::streamsize>(image.size()));
      check(bool(f), "Fixture reference write failed");
    }
    check(SendMessageW(window.handle(), WM_APP + 7, 0, 0) == 1,
          "Moving calibration marker requires a 1920px wide and at least 1024px high source");
    UpdateWindow(window.handle());
    ComPtr<IMMDeviceEnumerator> enumerator;
    hr(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                        CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&enumerator)),
       "Render enumeration failed");
    ComPtr<IMMDevice> device;
    hr(enumerator->GetDevice(argv[4], &device),
       "Fixture render endpoint unavailable");
    ComPtr<IAudioClient> client;
    hr(device->Activate(__uuidof(IAudioClient), CLSCTX_INPROC_SERVER, nullptr,
                        &client),
       "Fixture audio activation failed");
    WAVEFORMATEX *format{};
    hr(client->GetMixFormat(&format), "Fixture mix format unavailable");
    const std::unique_ptr<WAVEFORMATEX, decltype(&CoTaskMemFree)> format_owner(
        format, CoTaskMemFree);
    check(format->wBitsPerSample == 32 && format->nChannels == 2,
          "Fixture requires stereo float32 endpoint");
    const auto rate = format->nSamplesPerSec;
    hr(client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                          AUDCLNT_STREAMFLAGS_NOPERSIST |
                              AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                          2000000, 0, format, nullptr),
       "Fixture render initialization failed");
    UINT32 capacity{};
    hr(client->GetBufferSize(&capacity), "Fixture render capacity unavailable");
    HANDLE audio_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    check(audio_event != nullptr, "Fixture event creation failed");
    struct EventOwner {
      HANDLE h;
      ~EventOwner() { CloseHandle(h); }
    } event_owner{audio_event};
    hr(client->SetEventHandle(audio_event), "Fixture event binding failed");
    ComPtr<IAudioRenderClient> render;
    hr(client->GetService(IID_PPV_ARGS(&render)),
       "Fixture render service unavailable");
    ComPtr<IAudioClock> clock;
    hr(client->GetService(IID_PPV_ARGS(&clock)),
       "Fixture render clock unavailable");
    UINT64 frequency{};
    hr(clock->GetFrequency(&frequency), "Fixture clock frequency unavailable");
    BYTE *initial{};
    hr(render->GetBuffer(capacity, &initial), "Fixture prefill failed");
    hr(render->ReleaseBuffer(capacity, AUDCLNT_BUFFERFLAGS_SILENT),
       "Fixture prefill release failed");
    hr(client->Start(), "Fixture renderer failed to start");
    struct Stop {
      IAudioClient *p;
      ~Stop() { p->Stop(); }
    } stop{client.Get()};
    Json pulses = Json::array(), painted = Json::array(),
         idle_intervals = Json::array();
    uint64_t submitted{capacity}, underruns{};
    const auto epoch = nss::qpc_hns(),
               deadline = epoch + 20 * 60 * nss::hns_per_second;
    bool idle_started{}, idle_done{}, rendering{true};
    auto last_start = epoch;
    DWORD task_index{};
    HANDLE task = AvSetMmThreadCharacteristicsW(L"Audio", &task_index);
    struct TaskOwner {
      HANDLE h;
      ~TaskOwner() {
        if (h)
          AvRevertMmThreadCharacteristics(h);
      }
    } task_owner{task};
    if (task)
      AvSetMmThreadPriority(task, AVRT_PRIORITY_HIGH);
    std::mutex pulse_mutex;
    std::exception_ptr painter_error;
    std::jthread painter([&](std::stop_token token) {
      try {
        const auto timer = CreateWaitableTimerExW(nullptr, nullptr,
            CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_MODIFY_STATE | SYNCHRONIZE);
        check(timer != nullptr, "Fixture high-resolution painter timer unavailable");
        struct CloseTimer {
          HANDLE value;
          ~CloseTimer() { CloseHandle(value); }
        } close_timer{timer};
        size_t visual_next{};
        uint64_t last_number{};
        bool on{};
        while (!token.stop_requested()) {
          const auto now = nss::qpc_hns();
          if (now - epoch >= nss::hns_per_second) {
            const auto number =
                static_cast<uint64_t>(now - epoch - nss::hns_per_second) * 60 /
                    nss::hns_per_second +
                1;
            if (number != last_number) {
              SendMessageW(window.handle(), WM_APP + 4, 0,
                           static_cast<LPARAM>(number));
              UpdateWindow(window.handle());
              painted.push_back(
                  {{"number", number}, {"qpcHns", nss::qpc_hns()}});
              last_number = number;
            }
          }
          int64_t target{};
          {
            std::lock_guard lock(pulse_mutex);
            if (visual_next < pulses.size())
              target = pulses[visual_next].at("audioQpcHns").get<int64_t>();
          }
          if (target) {
            if (!on && now >= target) {
              const auto before = nss::qpc_hns();
              SendMessageW(window.handle(), WM_APP + 5, 1, 0);
              UpdateWindow(window.handle());
              {
                std::lock_guard lock(pulse_mutex);
                pulses[visual_next]["visualBeginQpcHns"] = before;
                pulses[visual_next]["visualEndQpcHns"] = nss::qpc_hns();
              }
              on = true;
            }
            if (on && now >= target + 800000) {
              SendMessageW(window.handle(), WM_APP + 5, 0, 0);
              UpdateWindow(window.handle());
              on = false;
              ++visual_next;
            }
          }
          // Sleep(1) can follow a coarse scheduler tick and bunch paints near
          // adjacent refreshes. Use an owned timer; change no global period.
          LARGE_INTEGER due{};
          due.QuadPart = -10000;
          check(SetWaitableTimerEx(timer, &due, 0, nullptr, nullptr, nullptr, 0) != FALSE,
                "Fixture painter timer could not be armed");
          check(WaitForSingleObject(timer, 1000) == WAIT_OBJECT_0,
                "Fixture painter timer did not wake");
        }
      } catch (...) {
        painter_error = std::current_exception();
      }
    });
    std::cout << Json{{"ready", true},
                      {"processId", GetCurrentProcessId()},
                      {"display", nss::capture::display_json(display)},
                      {"identityPattern", fixture::marker::name},
                      {"renderRate", rate},
                      {"toneHz", 1000},
                      {"toneAmplitude", .08},
                      {"pulseMs", 80},
                      {"periodMs", 2000}}
                     .dump()
              << '\n';
    while (!std::filesystem::exists(argv[1]) && nss::qpc_hns() < deadline) {
      const auto elapsed = nss::qpc_hns() - epoch;
      if (idle && !idle_started && elapsed >= 92000000) {
        hr(client->Stop(), "Fixture idle stop failed");
        rendering = false;
        idle_started = true;
        idle_intervals.push_back({{"beginQpcHns", nss::qpc_hns()}});
      }
      if (idle_started && !idle_done && elapsed >= 442000000) {
        hr(client->Start(), "Fixture idle resume failed");
        rendering = true;
        last_start = nss::qpc_hns();
        idle_done = true;
        idle_intervals.back()["endQpcHns"] = nss::qpc_hns();
      }
      UINT32 padding{};
      hr(client->GetCurrentPadding(&padding), "Fixture padding failed");
      if (rendering && padding == 0 &&
          nss::qpc_hns() - last_start > nss::hns_per_second)
        ++underruns;
      const auto available = rendering ? capacity - padding : 0;
      if (available) {
        BYTE *raw{};
        hr(render->GetBuffer(available, &raw), "Fixture render buffer failed");
        auto *samples = reinterpret_cast<float *>(raw);
        for (UINT32 i = 0; i < available; ++i) {
          const auto index = submitted + i;
          const auto period = index / (uint64_t(rate) * 2);
          const auto phase = index % (uint64_t(rate) * 2);
          const bool pulse = period > 0 && phase < rate * 80 / 1000;
          const float sample =
              pulse ? static_cast<float>(.08 *
                                         std::sin(2 * std::numbers::pi * 1000 *
                                                  double(phase) / rate))
                    : 0.f;
          samples[i * 2] = samples[i * 2 + 1] = sample;
          if (pulse && phase == 0) {
            UINT64 position{}, qpc{};
            hr(clock->GetPosition(&position, &qpc),
               "Fixture correlated audio clock failed");
            const double current_sample =
                double(position) * rate / double(frequency);
            const auto target =
                static_cast<int64_t>(qpc) +
                static_cast<int64_t>((double(index) - current_sample) *
                                     nss::hns_per_second / rate);
            std::lock_guard lock(pulse_mutex);
            pulses.push_back({{"index", period},
                              {"renderSample", index},
                              {"audioQpcHns", target},
                              {"audioClockPosition", position},
                              {"audioClockQpcHns", qpc},
                              {"audioClockFrequency", frequency}});
          }
        }
        hr(render->ReleaseBuffer(available, 0),
           "Fixture render release failed");
        submitted += available;
      }
      WaitForSingleObject(audio_event, 5);
    }
    painter.request_stop();
    painter.join();
    if (painter_error)
      std::rethrow_exception(painter_error);
    check(std::filesystem::exists(argv[1]), "Fixture stop deadline reached");
    const Json result{
        {"status", "PASS"},
        {"source", "owned WASAPI 1kHz PCM and binary/flash scene"},
        {"epochQpcHns", epoch},
        {"pulses", pulses},
        {"updates", painted},
        {"identityPattern", fixture::marker::name},
                      {"renderRate", rate},
        {"renderSamples", submitted},
        {"renderUnderruns", underruns},
        {"renderBufferFrames", capacity},
        {"renderMmcssRegistered", task != nullptr},
        {"renderIdleIntervals", idle_intervals}};
    std::ofstream out(std::filesystem::path(argv[3]), std::ios::binary);
    out << result.dump(2);
    out.close();
    check(bool(out), "Fixture report close failed");
    std::cout << Json{{"status", "PASS"},
                      {"pulses", pulses.size()},
                      {"renderSamples", submitted}}
                     .dump()
              << '\n';
    return 0;
  } catch (const std::exception &e) {
    std::cout << Json{{"status", "FAIL"}, {"message", e.what()}}.dump() << '\n';
    return 1;
  }
}
