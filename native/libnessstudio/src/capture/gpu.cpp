#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "camera/camera.hpp"
#include "capture.hpp"
#include "readback.hpp"
#include "nv12_shader.hpp"
#include "nv12_compute.hpp"
#include "gpu_timing.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <d3d11_4.h>
#include <deque>
#include <dxgi1_6.h>
#include <functional>
#include <limits>
#include <mutex>
#include <roapi.h>
#include <thread>
#include <type_traits>
#include <utility>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <windows.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <wrl/client.h>

namespace nss::capture {
#ifdef NSS_CAPTURE_QUALIFICATION
static std::function<void()> qualification_arrival;
static bool qualification_completion_event_failure{};
static bool qualification_fence_failure{};
static bool qualification_shader_view_failure{};
static bool qualification_compute_view_failure{};
static bool qualification_output5_unavailable{};
void set_qualification_arrival(std::function<void()> hook) {
  qualification_arrival = std::move(hook);
}
void set_qualification_completion_event_failure(bool fail) {
  qualification_completion_event_failure = fail;
}
void set_qualification_fence_failure(bool fail) {
  qualification_fence_failure = fail;
}
void set_qualification_shader_view_failure(bool fail) {
  qualification_shader_view_failure = fail;
}
void set_qualification_compute_view_failure(bool fail) {
  qualification_compute_view_failure = fail;
}
void set_qualification_output5_unavailable(bool unavailable) {
  qualification_output5_unavailable = unavailable;
}
#endif
namespace {
using Microsoft::WRL::ComPtr;
using namespace winrt::Windows::Graphics::Capture;
using winrt::Windows::Graphics::SizeInt32;
using winrt::Windows::Graphics::DirectX::DirectXPixelFormat;
using WinDevice =
    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice;
using Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess;
uint64_t luid(LUID value) {
  return (uint64_t(static_cast<uint32_t>(value.HighPart)) << 32) |
         value.LowPart;
}
void retain_capture_code() {
  static const bool retained = [] {
    const auto image = LoadLibraryExW(L"GraphicsCapture.dll", nullptr,
                                      LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!image)
      checked(HRESULT_FROM_WIN32(GetLastError()), NSS_STAGE_CAPTURE,
              "WGC system component unavailable");
    HMODULE pinned{};
    const auto result = GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
        reinterpret_cast<LPCWSTR>(image), &pinned);
    const auto error = GetLastError();
    FreeLibrary(image);
    if (!result)
      checked(HRESULT_FROM_WIN32(error), NSS_STAGE_CAPTURE,
              "Cannot retain WGC implementation code");
    // WGC can retain a delegate after its public Close has returned. Its
    // native delegate code must also survive unloading by a synchronous host.
    static constexpr wchar_t anchor[] = L"NessStudio.Capture.CodeLifetime";
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN |
                                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                            anchor, &pinned))
      checked(HRESULT_FROM_WIN32(GetLastError()), NSS_STAGE_CAPTURE,
              "Cannot retain capture runtime code");
    return true;
  }();
  (void)retained;
}
struct Apartment {
  HRESULT initialized{E_FAIL};
  explicit Apartment(bool capture = true) {
    // Internal WGC work can outlive a short-lived MTA teardown. Retain code,
    // never capture sessions, devices, frames, textures or worker ownership.
    if (capture)
      retain_capture_code();
    initialized = RoInitialize(RO_INIT_MULTITHREADED);
    checked(initialized, NSS_STAGE_CAPTURE, "WinRT MTA initialization failed");
  }
  ~Apartment() {
    if (SUCCEEDED(initialized)) {
      winrt::clear_factory_cache();
      RoUninitialize();
    }
  }
};
struct Counters {
  std::atomic<uint64_t> bytes{}, peak_bytes{}, copies{}, copy_bytes{},
      converted{}, delivered{}, released{}, pressure{}, queries{},
      generations{}, resize_rejected{}, padding_clears{}, full_coverage_copies{},
      deferred_sources{}, uncopied_releases{}, submitted_source_returns{}, shader_converted{}, packed_converted{},
      served_window_releases{};
};
struct Device {
  // Declared first so the handle outlives every device/context/fence reference
  // in this owner. Surface leases retain this owner too. Normal close drains
  // submitted work before releasing it, including abandoned conversions.
  winrt::handle completion_event;
  ComPtr<IDXGIAdapter1> adapter;
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  ComPtr<ID3D11VideoDevice> video;
  ComPtr<ID3D11VideoContext1> video_context;
  ComPtr<ID3D11DeviceContext4> fence_context;
  ComPtr<ID3D11Fence> completion_fence;
  uint64_t next_completion{}, observed_completion{}, fence_reads{}, fence_cache_hits{}, query_reads{};
  uint64_t pending_completion{}, fence_signals{}, coalesced_completions{};
  uint64_t notification_value{}, last_notification_value{}, notification_registrations{}, notification_failures{};
  bool notification_attempted{}, notification_disabled{};
  HRESULT notification_result{E_NOINTERFACE};
  HRESULT fence_capability{E_NOINTERFACE};
  WinDevice winrt_device{nullptr};
  Json info;
  DWORD owner = GetCurrentThreadId();
  uint64_t adapter_id{};
  explicit Device(uint64_t selected, bool prefer_query_completion = false) : adapter_id(selected) {
    ComPtr<IDXGIFactory4> factory;
    checked(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), NSS_STAGE_CAPTURE,
            "DXGI factory failed");
    // Resolve the already enumerated monitor identity directly. Enumerating
    // every adapter here needlessly initializes unrelated hybrid drivers.
    const LUID requested{static_cast<DWORD>(selected),
                         static_cast<LONG>(selected >> 32)};
    checked(factory->EnumAdapterByLuid(requested, IID_PPV_ARGS(&adapter)),
            NSS_STAGE_CAPTURE, "Monitor adapter LUID cannot be resolved");
    DXGI_ADAPTER_DESC1 selected_desc{};
    checked(adapter->GetDesc1(&selected_desc), NSS_STAGE_CAPTURE,
            "Adapter descriptor failed");
    if (luid(selected_desc.AdapterLuid) != selected)
      throw Error(NSS_E_UNAVAILABLE, NSS_STAGE_CAPTURE, 0,
                  "Resolved adapter LUID does not match monitor");
    if (selected_desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
      throw Error(NSS_E_UNAVAILABLE, NSS_STAGE_CAPTURE, 0,
                  "Strict GPU capture cannot use a software adapter");
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1,
                                        D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL level{};
    checked(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                              D3D11_CREATE_DEVICE_BGRA_SUPPORT |
                                  D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
                              levels, 2, D3D11_SDK_VERSION, &device, &level,
                              &context),
            NSS_STAGE_CAPTURE,
            "Hardware D3D11 device creation failed; no software fallback was "
            "attempted");
    checked(device.As(&video), NSS_STAGE_PROCESS,
            "D3D11 video device unavailable");
    checked(context.As(&video_context), NSS_STAGE_PROCESS,
            "Explicit video color-space controls unavailable");
    ComPtr<ID3D11Device5> fence_device;
    if (SUCCEEDED(device.As(&fence_device)) &&
        SUCCEEDED(context.As(&fence_context))) {
      fence_capability =
#ifdef NSS_CAPTURE_QUALIFICATION
          qualification_fence_failure ? E_NOINTERFACE :
#endif
          fence_device->CreateFence(
          0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&completion_fence));
      if (FAILED(fence_capability)) {
        // Optional synchronization capability. The original event-query path
        // remains available; a removed device is still an immediate error.
        checked(device->GetDeviceRemovedReason(), NSS_STAGE_CAPTURE,
                "D3D11 device removed while probing timeline fences");
        completion_fence.Reset();
        fence_context.Reset();
      }
    }
    const bool timeline_supported = bool(completion_fence);
    if (prefer_query_completion) {
      completion_fence.Reset();
      fence_context.Reset();
    }
    ComPtr<IDXGIDevice> dxgi;
    checked(device.As(&dxgi), NSS_STAGE_CAPTURE, "DXGI device unavailable");
    winrt::com_ptr<IInspectable> inspect;
    checked(CreateDirect3D11DeviceFromDXGIDevice(dxgi.Get(), inspect.put()),
            NSS_STAGE_CAPTURE, "WinRT D3D device bridge failed");
    winrt_device = inspect.as<WinDevice>();
    DXGI_ADAPTER_DESC1 desc{};
    checked(adapter->GetDesc1(&desc), NSS_STAGE_CAPTURE,
            "Adapter descriptor failed");
    LARGE_INTEGER driver{};
    checked(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &driver),
            NSS_STAGE_CAPTURE, "Driver version query failed");
    const std::wstring wide(desc.Description);
    const auto length = WideCharToMultiByte(CP_UTF8, 0, wide.data(),
                                            static_cast<int>(wide.size()),
                                            nullptr, 0, nullptr, nullptr);
    std::string name(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                        name.data(), length, nullptr, nullptr);
    info = {{"adapterLuid", selected},
            {"name", name},
            {"vendorId", desc.VendorId},
            {"deviceId", desc.DeviceId},
            {"featureLevel", static_cast<uint32_t>(level)},
            {"driverVersion", uint64_t(driver.QuadPart)},
            {"hardware", true},
            {"deviceCreations", 1},
            {"immediateContextOwnerThread", owner},
            {"timelineFenceSupported", timeline_supported},
            {"timelineFenceSelected", bool(completion_fence)},
            {"queryCompletionPreferred", prefer_query_completion},
            {"timelineFenceCapabilityResult", fence_capability},
            {"formatIn", "BGRA8"},
            {"formatOut", "NV12"}};
  }
  void thread() const {
    if (GetCurrentThreadId() != owner)
      throw Error(NSS_E_STATE, NSS_STAGE_PROCESS, 0,
                  "Immediate context operations require their owner thread");
  }
  void healthy() const {
    thread();
    checked(device->GetDeviceRemovedReason(), NSS_STAGE_CAPTURE,
            "D3D11 device removed");
  }
  void submit_completion() {
    thread();
    if (!pending_completion) return;
    checked(fence_context->Signal(completion_fence.Get(), pending_completion),
            NSS_STAGE_PROCESS, "GPU batch completion timeline signal failed");
    ++fence_signals;
    pending_completion = 0;
  }
  uint64_t mark_completion(ID3D11Query *fallback, bool batch = false) {
    thread();
    if (!completion_fence) {
      context->End(fallback);
      return 0;
    }
    if (batch && pending_completion) {
      ++coalesced_completions;
      return pending_completion;
    }
    // A reserved value cannot complete until its actual Signal is submitted.
    // Commands on this owner/context may share a marker up to that boundary.
    // Immediate consumers also drain an earlier reservation before advancing
    // the timeline, preserving strictly increasing GPU signals.
    if (!batch) submit_completion();
    if (next_completion == (std::numeric_limits<uint64_t>::max)() - 1)
      throw Error(NSS_E_INTERNAL, NSS_STAGE_PROCESS, 0,
                  "GPU completion timeline exhausted");
    const auto value = ++next_completion;
    if (batch) {
      pending_completion = value;
      return value;
    }
    checked(fence_context->Signal(completion_fence.Get(), value),
            NSS_STAGE_PROCESS, "GPU completion timeline signal failed");
    ++fence_signals;
    return value;
  }
  uint64_t completed_fence_value() {
    thread();
    // This fence has one owner and is signaled only with increasing values.
    // Once all submitted work has completed, another driver query cannot
    // reveal newer work until mark_completion advances next_completion.
    // Device-loss probes remain independent of this completion evidence.
    if (observed_completion == next_completion) {
      ++fence_cache_hits;
      return observed_completion;
    }
    ++fence_reads;
    const auto value = completion_fence->GetCompletedValue();
    if (value == (std::numeric_limits<uint64_t>::max)()) {
      healthy();
      throw Error(NSS_E_UNAVAILABLE, NSS_STAGE_PROCESS, 0,
                  "GPU completion timeline became unavailable");
    }
    observed_completion = value;
    return observed_completion;
  }
  bool completed(ID3D11Query *fallback, uint64_t value) {
    thread();
    if (completion_fence) {
      if (value <= observed_completion) {
        ++fence_cache_hits;
        return true;
      }
      return completed_fence_value() >= value;
    }
    ++query_reads;
    BOOL ready{};
    const auto result = context->GetData(fallback, &ready, sizeof(ready),
                                         D3D11_ASYNC_GETDATA_DONOTFLUSH);
    checked(result, NSS_STAGE_PROCESS, "GPU completion query failed");
    return result == S_OK && ready;
  }
  HANDLE completion_notification(uint64_t value) {
    thread();
    if (!completion_fence || notification_disabled)
      return nullptr;
    if (!notification_attempted) {
      if (!value) return nullptr;
      notification_attempted = true;
      completion_event.attach(CreateEventW(nullptr, TRUE, FALSE, nullptr));
      notification_result = completion_event ? S_OK : HRESULT_FROM_WIN32(GetLastError());
      if (!completion_event) {
        notification_disabled = true;
        ++notification_failures;
        return nullptr;
      }
    }
    if (notification_value) {
      // A completed fence value alone does not prove that its notification
      // was delivered. Keep exactly one registration until its event signals.
      // Manual reset lets the consumer wait without consuming that evidence.
      const auto signaled = WaitForSingleObject(completion_event.get(), 0);
      if (signaled == WAIT_OBJECT_0) {
        checked(ResetEvent(completion_event.get()) ? S_OK : HRESULT_FROM_WIN32(GetLastError()),
                NSS_STAGE_PROCESS, "GPU completion notification reset failed");
        notification_value = 0;
      } else if (signaled != WAIT_TIMEOUT) {
        checked(HRESULT_FROM_WIN32(GetLastError()), NSS_STAGE_PROCESS,
                "GPU completion notification query failed");
      }
    }
    if (!value)
      return nullptr;
    if (!notification_value) {
      // Completed queue entries can remain until poll() consumes them. Never
      // repeatedly register an immediately signaled notification for one value.
      if (value <= last_notification_value)
        return nullptr;
#ifdef NSS_CAPTURE_QUALIFICATION
      notification_result = qualification_completion_event_failure ? E_OUTOFMEMORY :
          completion_fence->SetEventOnCompletion(value, completion_event.get());
#else
      notification_result = completion_fence->SetEventOnCompletion(value, completion_event.get());
#endif
      if (FAILED(notification_result)) {
        // Optional wakeup optimization. Preserve the bounded polling path if
        // registration resources are unavailable; device removal still fails.
        healthy();
        notification_disabled = true;
        ++notification_failures;
        return nullptr;
      }
      notification_value = value;
      last_notification_value = value;
      ++notification_registrations;
    }
    return completion_event.get();
  }
};
struct CaptureState {
  struct Gap {
    int64_t previous{}, acquisition{}, callback{};
  };
  std::array<Gap, 128> gaps{};
  uint64_t gap_count{};
  int64_t last_acquisition{}, maximum_gap{};
  HANDLE ready{};
  CaptureState() {
    // Allocate after all C++ members have constructed, so an allocation
    // failure in the frame queue cannot strand an already-created handle.
    ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!ready)
      throw Error(NSS_E_UNAVAILABLE, NSS_STAGE_CAPTURE,
                  HRESULT_FROM_WIN32(GetLastError()),
                  "WGC readiness event creation failed");
  }
  ~CaptureState() { CloseHandle(ready); }
  std::mutex mutex;
  std::condition_variable idle;
  bool stopping{}, recreating{}, closed{}, closed_by_event{}, closed_by_os{};
  uint32_t callbacks{};
  std::deque<Direct3D11CaptureFrame> frames;
  uint64_t received{}, dropped{}, dequeued{}, stop_discard{}, resize_discard{},
      callback_peak{};
  int32_t observed_width{}, observed_height{};
  std::optional<Error> failure;
  uint32_t callbacks_at_stop{};
#ifdef NSS_CAPTURE_QUALIFICATION
  std::function<void()> before_arrival;
#endif
};
class Wgc {
  // Retained acquisitions and an outstanding owned copy must not exhaust
  // the producer's pool while the owner finishes conversion/encoder work.
  static constexpr int32_t frame_pool_size = 5;
  std::shared_ptr<CaptureState> state_ = std::make_shared<CaptureState>();
  IDirect3D11CaptureFramePoolStatics2 pool_factory_{nullptr};
  GraphicsCaptureItem item_{nullptr};
  Direct3D11CaptureFramePool pool_{nullptr};
  GraphicsCaptureSession session_{nullptr};
  winrt::Windows::Foundation::TypedEventHandler<
      Direct3D11CaptureFramePool, winrt::Windows::Foundation::IInspectable>
      handler_{nullptr};
  winrt::event_token arrived_{}, closed_{};
  bool subscribed_{}, closed_subscribed_{}, cursor_{};
  HWND window_{};
  HMONITOR monitor_{};
  int64_t lifetime_probe_at_{};

public:
  SizeInt32 size{};
  Wgc(const std::shared_ptr<Device> &device, const Plan &plan, HWND window)
      : cursor_(plan.cursor), window_(window),
        monitor_(window ? nullptr
                        : static_cast<HMONITOR>(plan.display.monitor)) {
    // Own activation factories inside the initialized apartment. Projected
    // static calls cache factories beyond a short-lived MTA and can retain
    // stale interfaces after RoUninitialize followed by a later capture.
    const auto support =
        winrt::get_activation_factory<GraphicsCaptureSession,
                                      IGraphicsCaptureSessionStatics>();
    if (!support.IsSupported())
      throw Error(NSS_E_UNAVAILABLE, NSS_STAGE_CAPTURE, 0,
                  "Windows Graphics Capture is not supported");
    pool_factory_ =
        winrt::get_activation_factory<Direct3D11CaptureFramePool,
                                      IDirect3D11CaptureFramePoolStatics2>();
    const auto factory =
        winrt::get_activation_factory<GraphicsCaptureItem,
                                      IGraphicsCaptureItemInterop>();
    const auto hr =
        window
            ? factory->CreateForWindow(window,
                                       winrt::guid_of<GraphicsCaptureItem>(),
                                       winrt::put_abi(item_))
            : factory->CreateForMonitor(
                  static_cast<HMONITOR>(plan.display.monitor),
                  winrt::guid_of<GraphicsCaptureItem>(), winrt::put_abi(item_));
    checked(hr, NSS_STAGE_CAPTURE, "WGC item creation failed");
    size = item_.Size();
    if (size.Width <= 0 || size.Height <= 0)
      throw Error(NSS_E_UNAVAILABLE, NSS_STAGE_CAPTURE, 0,
                  "WGC source has no valid content size");
    try {
      pool_ = pool_factory_.CreateFreeThreaded(
          device->winrt_device, DirectXPixelFormat::B8G8R8A8UIntNormalized, frame_pool_size,
          size);
      std::weak_ptr<CaptureState> weak = state_;
#ifdef NSS_CAPTURE_QUALIFICATION
      state_->before_arrival = qualification_arrival;
#endif
      handler_ = [weak](const Direct3D11CaptureFramePool &sender,
                        const auto &) noexcept {
        const auto state = weak.lock();
        if (!state)
          return;
        {
          std::lock_guard lock(state->mutex);
          if (state->stopping || state->recreating)
            return;
          ++state->callbacks;
          state->callback_peak =
              (std::max)(state->callback_peak, uint64_t(state->callbacks));
        }
        struct finish {
          std::shared_ptr<CaptureState> value;
          ~finish() {
            std::lock_guard lock(value->mutex);
            --value->callbacks;
            value->idle.notify_all();
            SetEvent(value->ready);
          }
        } done{state};
        try {
#ifdef NSS_CAPTURE_QUALIFICATION
          if (state->before_arrival)
            state->before_arrival();
#endif
          auto frame = sender.TryGetNextFrame();
          if (!frame)
            return;
          std::lock_guard lock(state->mutex);
          if (state->stopping || state->recreating) {
            frame.Close();
            return;
          }
          ++state->received;
          const auto acquisition = frame.SystemRelativeTime().count();
          if (state->last_acquisition && acquisition > state->last_acquisition) {
            const auto interval = acquisition - state->last_acquisition;
            state->maximum_gap = (std::max)(state->maximum_gap, interval);
            if (interval > 300000) {
              state->gaps[state->gap_count % state->gaps.size()] =
                  {state->last_acquisition, acquisition, qpc_hns()};
              ++state->gap_count;
            }
          }
          state->last_acquisition = acquisition;
          const auto content = frame.ContentSize();
          state->observed_width = content.Width;
          state->observed_height = content.Height;
          while (state->frames.size() >= 4) {
            state->frames.front().Close();
            state->frames.pop_front();
            ++state->dropped;
          }
          state->frames.push_back(std::move(frame));
        } catch (const winrt::hresult_error &e) {
          std::lock_guard lock(state->mutex);
          if (!state->stopping)
            state->failure.emplace(NSS_E_UNAVAILABLE, NSS_STAGE_CAPTURE,
                                   e.code(), "WGC frame acquisition failed");
        } catch (...) {
          std::lock_guard lock(state->mutex);
          if (!state->stopping)
            state->failure.emplace(NSS_E_INTERNAL, NSS_STAGE_CAPTURE, 0,
                                   "WGC callback failed");
        }
      };
      arrived_ = pool_.FrameArrived(handler_);
      subscribed_ = true;
      closed_ = item_.Closed([weak](const auto &, const auto &) noexcept {
        if (const auto state = weak.lock()) {
          std::lock_guard lock(state->mutex);
          if (!state->stopping) {
            state->closed = true;
            state->closed_by_event = true;
            SetEvent(state->ready);
          }
        }
      });
      closed_subscribed_ = true;
      session_ = pool_.CreateCaptureSession(item_);
      session_.IsCursorCaptureEnabled(plan.cursor);
      session_.StartCapture();
    } catch (...) {
      close();
      throw;
    }
  }
  ~Wgc() {
    try {
      close();
    } catch (...) {
    }
  }
  void *wake_event() const noexcept { return state_->ready; }
  Direct3D11CaptureFrame pop() {
    // Desktop interop sources also have an independently queryable Win32
    // lifetime. Do not depend on delivery of an item event to an apartment.
    const auto now = qpc_hns();
    if (now - lifetime_probe_at_ >= hns_per_second / 50) {
      MONITORINFO monitor{};
      monitor.cbSize = sizeof(monitor);
      if ((window_ && !IsWindow(window_)) ||
          (monitor_ && !GetMonitorInfoW(monitor_, &monitor))) {
        std::lock_guard lock(state_->mutex);
        state_->closed = true;
        state_->closed_by_os = true;
      }
      lifetime_probe_at_ = now;
    }
    std::lock_guard lock(state_->mutex);
    if (state_->failure)
      throw *state_->failure;
    if (state_->closed)
      throw Error(NSS_E_UNAVAILABLE, NSS_STAGE_CAPTURE, RO_E_CLOSED,
                  "WGC source was closed");
    if (state_->frames.empty())
      return nullptr;
    // Preserve chronological candidates until the CFR owner can compare their
    // timestamps with its window. A newer acquisition may already belong to
    // the next window; replacing the older one here could force a repeat.
    // The callback retains at most four acquisitions in the existing
    // five-frame WGC pool, including while the owner publishes its journal.
    auto frame = std::move(state_->frames.front());
    state_->frames.pop_front();
    ++state_->dequeued;
    return frame;
  }
  void recreate(const std::shared_ptr<Device> &device, SizeInt32 next) {
    {
      std::unique_lock lock(state_->mutex);
      state_->recreating = true;
      if (!state_->idle.wait_for(lock, std::chrono::seconds(2),
                                 [&] { return state_->callbacks == 0; }))
        throw Error(NSS_E_TIMEOUT, NSS_STAGE_CAPTURE, 0,
                    "WGC callbacks did not quiesce for resize");
      state_->resize_discard += state_->frames.size();
      for (auto &frame : state_->frames)
        frame.Close();
      state_->frames.clear();
    }
    // The triggering resized frame may exceed the old pool's texture. A new
    // capture session requests initial content even when the source is now
    // static; Recreate alone is not a guarantee of a subsequent dirty frame.
    if (subscribed_) {
      pool_.FrameArrived(arrived_);
      subscribed_ = false;
    }
    session_.Close();
    session_ = nullptr;
    pool_.Close();
    pool_ = nullptr;
    pool_ = pool_factory_.CreateFreeThreaded(
        device->winrt_device, DirectXPixelFormat::B8G8R8A8UIntNormalized, frame_pool_size,
        next);
    size = next;
    arrived_ = pool_.FrameArrived(handler_);
    subscribed_ = true;
    session_ = pool_.CreateCaptureSession(item_);
    session_.IsCursorCaptureEnabled(cursor_);
    {
      std::lock_guard lock(state_->mutex);
      state_->recreating = false;
    }
    session_.StartCapture();
  }
  Json metrics() const {
    std::lock_guard lock(state_->mutex);
    Json gaps = Json::array();
    const auto retained = (std::min)(state_->gap_count, uint64_t(state_->gaps.size()));
    for (uint64_t i = state_->gap_count - retained; i < state_->gap_count; ++i) {
      const auto &gap = state_->gaps[i % state_->gaps.size()];
      gaps.push_back({{"previousAcquisitionQpcHns", gap.previous},
                      {"acquisitionQpcHns", gap.acquisition},
                      {"callbackQpcHns", gap.callback}});
    }
    return {{"captureFrames", state_->received},
            {"captureFramePoolSize", frame_pool_size},
            {"acquisitionGapDiagnosticThresholdHns", 300000},
            {"acquisitionGapsOverThreshold", state_->gap_count},
            {"maximumAcquisitionGapHns", state_->maximum_gap},
            {"recentAcquisitionGaps", std::move(gaps)},
            {"observedWidth", state_->observed_width},
            {"observedHeight", state_->observed_height},
            {"poolWidth", size.Width},
            {"poolHeight", size.Height},
            {"acquisitionDecimation", state_->dropped},
            {"dequeuePolicy", "oldest retained acquisition; timestamp-aware CFR coalescing"},
            {"lifetimeFallbackProbeIntervalMs", 20},
            {"callbackPeak", state_->callback_peak},
            {"callbacksActive", state_->callbacks},
            {"callbacksActiveAtStop", state_->callbacks_at_stop},
            {"queueLimit", 4},
            {"queued", state_->frames.size()},
            {"dequeued", state_->dequeued},
            {"discardedAtStop", state_->stop_discard},
            {"discardedAtResize", state_->resize_discard},
            {"callbackD3DContextCalls", 0},
            {"callbackGpuWaits", 0},
            {"closed", state_->closed},
            {"closedByItemEvent", state_->closed_by_event},
            {"closedByWin32Lifetime", state_->closed_by_os}};
  }
  void stop() {
    {
      std::lock_guard lock(state_->mutex);
      if (state_->stopping)
        return;
      state_->stopping = true;
      state_->callbacks_at_stop = state_->callbacks;
    }
    if (subscribed_) {
      pool_.FrameArrived(arrived_);
      subscribed_ = false;
    }
    if (closed_subscribed_) {
      item_.Closed(closed_);
      closed_subscribed_ = false;
    }
    if (session_) {
      session_.Close();
      session_ = nullptr;
    }
    {
      std::unique_lock lock(state_->mutex);
      if (!state_->idle.wait_for(lock, std::chrono::seconds(2),
                                 [&] { return state_->callbacks == 0; }))
        throw Error(NSS_E_TIMEOUT, NSS_STAGE_CAPTURE, 0,
                    "WGC callbacks did not quiesce on close");
      state_->stop_discard += state_->frames.size();
      for (auto &frame : state_->frames)
        frame.Close();
      state_->frames.clear();
    }
  }
  void close() {
    stop();
    if (pool_) {
      pool_.Close();
      pool_ = nullptr;
    }
    item_ = nullptr;
    pool_factory_ = nullptr;
  }
};
// Optional owner-thread diagnostics. Fixed storage avoids allocating on a slow
// driver return; disabled traces neither read QPC nor retain call samples.
class DriverTimings {
public:
  enum class Operation : size_t { Acquire, Copy, Mark, Signal, Flush, Release, Reap,
                                  DeviceHealth, MonitorHealth, InspectTexture, Count };
private:
  struct Total { uint64_t calls{}; int64_t total{}, maximum{}, maximum_begin{}; };
  struct Slow { Operation operation{}; int64_t begin{}, end{}; };
  bool enabled_{};
  std::array<Total, static_cast<size_t>(Operation::Count)> totals_{};
  std::array<Slow, 128> slow_{};
  size_t retained_{};
  uint64_t suppressed_{};
  void observe(Operation operation, int64_t begin, int64_t end) noexcept {
    auto &total = totals_[static_cast<size_t>(operation)];
    ++total.calls;
    total.total += end - begin;
    if (end - begin > total.maximum) { total.maximum = end - begin; total.maximum_begin = begin; }
    if (end - begin < 2 * 10000) return;
    if (retained_ < slow_.size()) slow_[retained_++] = {operation, begin, end};
    else {
      ++suppressed_;
      const auto smallest = std::min_element(slow_.begin(), slow_.end(), [](const auto &a, const auto &b) {
        return a.end - a.begin < b.end - b.begin;
      });
      if (end - begin > smallest->end - smallest->begin) *smallest = {operation, begin, end};
    }
  }
public:
  explicit DriverTimings(bool enabled) : enabled_(enabled) {}
  template<class F> auto call(Operation operation, F &&action) {
    if (!enabled_) return std::forward<F>(action)();
    const auto begin = qpc_hns();
    if constexpr (std::is_void_v<std::invoke_result_t<F>>) {
      std::forward<F>(action)();
      observe(operation, begin, qpc_hns());
    } else {
      auto result = std::forward<F>(action)();
      observe(operation, begin, qpc_hns());
      return result;
    }
  }
  Json report() const {
    static constexpr std::array names{"AcquireNextFrame", "CopySubresourceRegion", "ReserveOrSignalCompletion", "SubmitBatchSignal",
                                      "Flush", "ReleaseFrame", "ReapCompleted", "DeviceHealth",
                                      "MonitorHealth", "InspectAcquiredTexture"};
    Json calls = Json::object(), slow = Json::array();
    for (size_t i = 0; i < totals_.size(); ++i) {
      const auto &total = totals_[i];
      calls[names[i]] = {{"calls", total.calls}, {"totalMs", double(total.total) / 10000},
                        {"maximumMs", double(total.maximum) / 10000},
                        {"maximumBeginQpcHns", total.maximum_begin}};
    }
    for (size_t i = 0; i < retained_; ++i) {
      const auto &sample = slow_[i];
      slow.push_back({{"operation", names[static_cast<size_t>(sample.operation)]},
                     {"beginQpcHns", sample.begin}, {"endQpcHns", sample.end}});
    }
    return {{"enabled", enabled_}, {"minimumSlowCallMs", 2}, {"calls", calls},
            {"slowCalls", slow}, {"suppressedSlowCalls", suppressed_},
            {"retention", "longest 128 calls"},
            {"interpretation", "CPU wall spans, including scheduling; not GPU execution durations"}};
  }
};
using DriverOperation = DriverTimings::Operation;

// The explicit duplication route owns at most one acquired desktop resource.
// Deferred recording returns it after the owned copy completes, or before a
// new acquisition if that is requested first. Consumers use the owned pool.
class Duplication {
  std::shared_ptr<Device> device_;
  std::shared_ptr<Counters> stats_;
  DriverTimings &timings_;
  ComPtr<IDXGIOutputDuplication> output_;
  HRESULT output5_probe_{E_NOINTERFACE};
  bool modern_factory_{};
  bool original_factory_requested_{};
  bool acquired_{};
  bool copied_{};
  uint64_t copied_id_{};
  uint64_t copied_returns_before_acquire_{};
  uint64_t copied_returns_after_completion_{};
  uint64_t poll_requests_{}, poll_throttled_{};
  uint64_t polls_{}, frames_{}, pointer_only_{}, timeouts_{}, releases_{};
  uint64_t accumulated_{}, accumulated_peak_{}, protected_frames_{};
  int64_t last_timestamp_{};
  int64_t next_poll_hns_{};
  uint64_t frequency_{};
  uint32_t width_{}, height_{};
  struct AcquisitionObservation {
    int64_t begin{}, end{}, present_counter{};
    HRESULT result{};
    UINT accumulated{};
  };
  bool trace_acquisitions_{};
  std::array<AcquisitionObservation, 128> acquisitions_{};
  uint64_t observed_acquisitions_{};
public:
  struct Image { ComPtr<ID3D11Texture2D> texture; int64_t at{}; };
  Duplication(std::shared_ptr<Device> device, std::shared_ptr<Counters> stats,
              const Plan &plan, DriverTimings &timings)
      : device_(std::move(device)), stats_(std::move(stats)), timings_(timings),
        trace_acquisitions_(plan.trace) {
    if (plan.cursor)
      throw Error(NSS_E_UNAVAILABLE, NSS_STAGE_PLAN, 0,
                  "Experimental Desktop Duplication cursor composition is unavailable");
    ComPtr<IDXGIDevice> dxgi;
    checked(device_->device.As(&dxgi), NSS_STAGE_CAPTURE, "Duplication DXGI device unavailable");
    ComPtr<IDXGIAdapter> adapter;
    checked(dxgi->GetAdapter(&adapter), NSS_STAGE_CAPTURE, "Duplication adapter unavailable");
    ComPtr<IDXGIOutput1> selected;
    for (UINT index = 0; ; ++index) {
      ComPtr<IDXGIOutput> output;
      const auto result = adapter->EnumOutputs(index, &output);
      if (result == DXGI_ERROR_NOT_FOUND) break;
      checked(result, NSS_STAGE_CAPTURE, "Duplication output enumeration failed");
      DXGI_OUTPUT_DESC description{};
      checked(output->GetDesc(&description), NSS_STAGE_CAPTURE, "Duplication output description failed");
      if (!description.AttachedToDesktop || description.Monitor != plan.display.monitor) continue;
      if (description.Rotation != DXGI_MODE_ROTATION_IDENTITY)
        throw Error(NSS_E_UNAVAILABLE, NSS_STAGE_CAPTURE, 0,
                    "Experimental Desktop Duplication requires an unrotated monitor");
      checked(output.As(&selected), NSS_STAGE_CAPTURE, "Desktop Duplication interface unavailable");
      break;
    }
    if (!selected)
      throw Error(NSS_E_UNAVAILABLE, NSS_STAGE_CAPTURE, 0,
                  "Selected monitor is not on the duplication device");
    ComPtr<IDXGIOutput5> modern;
#ifdef NSS_CAPTURE_QUALIFICATION
    output5_probe_ = qualification_output5_unavailable ? E_NOINTERFACE : selected.As(&modern);
#else
    output5_probe_ = selected.As(&modern);
#endif
    original_factory_requested_ = plan.prefer_legacy_duplication;
    if (SUCCEEDED(output5_probe_) && !original_factory_requested_) {
      // Negotiate only the existing SDR input contract. Accepting additional
      // scan-out formats requires matching color handling and qualification.
      constexpr DXGI_FORMAT formats[]{DXGI_FORMAT_B8G8R8A8_UNORM};
      checked(modern->DuplicateOutput1(device_->device.Get(), 0, 1, formats, &output_),
              NSS_STAGE_CAPTURE, "Desktop Duplication1 could not acquire the selected output");
      modern_factory_ = true;
    } else {
      // The original factory is either an explicit SDR recording policy or a
      // missing-interface fallback. Unexpected probe and creation failures
      // remain errors; no access/device failure is hidden by another factory.
      if (FAILED(output5_probe_) && output5_probe_ != E_NOINTERFACE)
        checked(output5_probe_, NSS_STAGE_CAPTURE, "Desktop Duplication1 interface probe failed");
      checked(selected->DuplicateOutput(device_->device.Get(), &output_), NSS_STAGE_CAPTURE,
              "Desktop Duplication could not acquire the selected output");
    }
    DXGI_OUTDUPL_DESC description{};
    output_->GetDesc(&description);
    width_ = description.ModeDesc.Width; height_ = description.ModeDesc.Height;
    if (description.ModeDesc.Format != DXGI_FORMAT_B8G8R8A8_UNORM ||
        width_ != static_cast<uint32_t>(plan.display.bounds.width) ||
        height_ != static_cast<uint32_t>(plan.display.bounds.height))
      throw Error(NSS_E_UNAVAILABLE, NSS_STAGE_CAPTURE, 0,
                  "Duplicated desktop differs from the planned SDR monitor");
    LARGE_INTEGER frequency{};
    if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0)
      throw Error(NSS_E_UNAVAILABLE, NSS_STAGE_CAPTURE, 0, "Duplication QPC frequency unavailable");
    frequency_ = static_cast<uint64_t>(frequency.QuadPart);
  }
  ~Duplication() { try { release(); } catch (...) {} }
  void copied(uint64_t id) noexcept {
    copied_ = true;
    copied_id_ = id;
    ++stats_->deferred_sources;
  }
  void release_completed(uint64_t id) {
    if (!copied_ || copied_id_ != id) return;
    release();
    ++copied_returns_after_completion_;
  }
  void release() {
    if (!acquired_) return;
    const auto result = timings_.call(DriverOperation::Release, [&] { return output_->ReleaseFrame(); });
    const auto copied = copied_;
    acquired_ = false;
    copied_ = false;
    copied_id_ = 0;
    if (copied) --stats_->deferred_sources;
    checked(result, NSS_STAGE_CAPTURE, "Duplicated desktop release failed");
    ++releases_;
    if (copied) ++stats_->submitted_source_returns;
  }
  std::optional<Image> acquire() {
    device_->thread();
    ++poll_requests_;
    const auto now = qpc_hns();
    // GPU conversion/readback events can wake the owner several times before
    // the next desktop update. Bound repeated driver queries independently
    // of those completion consumers; source timestamps remain untouched.
    if (now < next_poll_hns_) { ++poll_throttled_; return {}; }
    next_poll_hns_ = now + 2 * 10000;
    // A new acquisition can arrive before copy completion. All reads of the
    // old desktop were already issued on the immediate context, so returning
    // it here is valid; otherwise completion retirement returns it earlier.
    const auto copied = copied_;
    release();
    if (copied) ++copied_returns_before_acquire_;
    ++polls_;
    DXGI_OUTDUPL_FRAME_INFO info{};
    ComPtr<IDXGIResource> resource;
    const auto acquire_begin = trace_acquisitions_ ? qpc_hns() : 0;
    const auto result = timings_.call(DriverOperation::Acquire,
        [&] { return output_->AcquireNextFrame(0, &info, &resource); });
    if (trace_acquisitions_) {
      acquisitions_[observed_acquisitions_ % acquisitions_.size()] =
          {acquire_begin, qpc_hns(), info.LastPresentTime.QuadPart, result, info.AccumulatedFrames};
      ++observed_acquisitions_;
    }
    if (result == DXGI_ERROR_WAIT_TIMEOUT) { ++timeouts_; return {}; }
    checked(result, NSS_STAGE_CAPTURE, "Duplicated desktop acquisition failed or access was lost");
    acquired_ = true;
    if (!info.LastPresentTime.QuadPart) { ++pointer_only_; release(); return {}; }
    if (info.LastPresentTime.QuadPart < 0)
      throw Error(NSS_E_UNAVAILABLE, NSS_STAGE_CAPTURE, 0, "Duplicated desktop timestamp is invalid");
    const auto timestamp = scale_floor(static_cast<uint64_t>(info.LastPresentTime.QuadPart),
                                       hns_per_second, frequency_);
    if (timestamp > static_cast<uint64_t>((std::numeric_limits<int64_t>::max)()) ||
        static_cast<int64_t>(timestamp) <= last_timestamp_)
      throw Error(NSS_E_UNAVAILABLE, NSS_STAGE_CAPTURE, 0,
                  "Duplicated desktop timestamps are not strictly increasing");
    last_timestamp_ = static_cast<int64_t>(timestamp);
    accumulated_ += info.AccumulatedFrames;
    accumulated_peak_ = (std::max)(accumulated_peak_, uint64_t(info.AccumulatedFrames));
    protected_frames_ += info.ProtectedContentMaskedOut != FALSE;
    ComPtr<ID3D11Texture2D> texture;
    D3D11_TEXTURE2D_DESC description{};
    timings_.call(DriverOperation::InspectTexture, [&] {
      checked(resource.As(&texture), NSS_STAGE_CAPTURE, "Duplicated desktop has no D3D11 texture");
      texture->GetDesc(&description);
    });
    if (description.Width != width_ || description.Height != height_ ||
        description.Format != DXGI_FORMAT_B8G8R8A8_UNORM)
      throw Error(NSS_E_UNAVAILABLE, NSS_STAGE_CAPTURE, 0, "Duplicated desktop geometry changed");
    ++frames_;
    return Image{std::move(texture), last_timestamp_};
  }
  void close() { release(); output_.Reset(); }
  Json acquisition_history() const {
    if (!trace_acquisitions_) return Json::object();
    Json calls = Json::array();
    const auto retained = (std::min)(observed_acquisitions_, uint64_t(acquisitions_.size()));
    for (auto i = observed_acquisitions_ - retained; i < observed_acquisitions_; ++i) {
      const auto &call = acquisitions_[i % acquisitions_.size()];
      calls.push_back({{"beginQpcHns", call.begin}, {"endQpcHns", call.end},
          {"result", static_cast<int64_t>(call.result)},
          {"lastPresentCounter", call.present_counter}, {"accumulatedFrames", call.accumulated}});
    }
    return {{"capacity", acquisitions_.size()}, {"callsObserved", observed_acquisitions_},
            {"qpcFrequency", frequency_}, {"calls", calls},
            {"scope", "Actual nonblocking AcquireNextFrame calls; source timestamps remain unmodified"}};
  }
  Json metrics() const {
    return {{"backend", "dxgi-desktop-duplication-experimental"},
            {"duplicationFactory", modern_factory_ ? "DuplicateOutput1" : "DuplicateOutput"},
            {"originalDuplicationFactoryRequested", original_factory_requested_},
            {"output5ProbeResult", static_cast<int64_t>(output5_probe_)},
            {"sourceTimestamp", "unmodified LastPresentTime QPC scaled to hns"},
            {"sourceApiExperimental", true}, {"cursorCompositionQualified", false},
            {"acquisitionPollRequests", poll_requests_}, {"acquisitionPollsThrottled", poll_throttled_},
            {"minimumAcquisitionPollMs", 2},
            {"acquisitionPolls", polls_}, {"acquisitionTimeouts", timeouts_},
            {"received", frames_}, {"pointerOnlyUpdates", pointer_only_},
            {"returnedAcquisitions", releases_}, {"heldDesktopAcquisition", acquired_},
            {"copiedDesktopReturnsBeforeAcquire", copied_returns_before_acquire_},
            {"copiedDesktopReturnsAfterCompletion", copied_returns_after_completion_},
            {"accumulatedDesktopFrames", accumulated_}, {"maximumAccumulatedFrames", accumulated_peak_},
            {"protectedDesktopFrames", protected_frames_}, {"idlePollCapMs", 4}};
  }
};
enum class SlotStage : uint32_t { Free, Writing, Ready, InUse, Released };
struct VideoObjects {
  ComPtr<ID3D11VideoProcessorEnumerator> enumerator;
  ComPtr<ID3D11VideoProcessor> processor;
  std::unique_ptr<Nv12Shader> shader;
  std::unique_ptr<Nv12Compute> compute;
};
struct Slot {
  std::shared_ptr<Device> device;
  std::shared_ptr<Counters> stats;
  uint64_t bytes{}, generation{};
  std::shared_ptr<VideoObjects> video;
  std::atomic<SlotStage> stage{SlotStage::Free};
  bool nv12_ready{}, conversion_queued{};
  ComPtr<ID3D11Texture2D> bgra, nv12;
  ComPtr<ID3D11RenderTargetView> clear;
  ComPtr<ID3D11VideoProcessorInputView> input;
  ComPtr<ID3D11VideoProcessorOutputView> output;
  Nv12Shader::Views shader_views;
  Nv12Compute::Views compute_views;
  ComPtr<ID3D11Query> done;
  uint64_t completion_value{};
  // Only the direct recording path defers this borrowed WGC acquisition.
  // Selection copies it into the existing owned BGRA/NV12 pair. The owner
  // returns it after enqueueing the copy, or without a copy when discarded.
  // The owned textures remain leased through GPU completion and consumption.
  Direct3D11CaptureFrame acquisition{nullptr};
  Rect acquisition_crop{};
  void return_acquisition() {
    if (!acquisition) return;
    acquisition.Close();
    acquisition = nullptr;
    --stats->deferred_sources;
  }
  ~Slot() {
    if (acquisition) {
      try { return_acquisition(); }
      catch (...) { --stats->deferred_sources; }
    }
    stats->bytes -= bytes;
    live_capture_bytes -= bytes;
  }
};
struct GpuSurface final : Surface {
  std::shared_ptr<Slot> slot;
  explicit GpuSurface(std::shared_ptr<Slot> value) : slot(std::move(value)) {
    slot->stage = SlotStage::InUse;
  }
  ~GpuSurface() override {
    slot->stage = SlotStage::Released;
    ++slot->stats->released;
  }
  void *gpu_texture() const noexcept override {
    return slot->nv12_ready ? slot->nv12.Get() : nullptr;
  }
  void *gpu_device() const noexcept override {
    return slot->device->device.Get();
  }
  uint64_t adapter_luid() const noexcept override {
    return slot->device->adapter_id;
  }
};
struct Generation {
  uint64_t id{};
  std::shared_ptr<VideoObjects> video = std::make_shared<VideoObjects>();
  std::vector<std::shared_ptr<Slot>> slots;
};
struct Pending {
  std::shared_ptr<Slot> slot;
  Direct3D11CaptureFrame source{nullptr};
  uint64_t id{};
  int64_t at{};
  bool delivered_before_completion{};
  uint64_t copy_completion{};
};
class GpuPipeline final : public Pipeline {
  Apartment apartment_;
  Plan plan_;
  DriverTimings driver_timings_{plan_.trace && plan_.desktop_duplication};
  std::shared_ptr<Device> device_;
  GpuTiming copy_timing_{plan_.trace}, conversion_timing_{plan_.trace};
  std::unique_ptr<Wgc> source_;
  std::unique_ptr<Duplication> duplication_;
  std::unique_ptr<camera::Source> camera_;
  uint64_t camera_upload_bytes_{};
  std::shared_ptr<Counters> stats_ = std::make_shared<Counters>();
  std::unique_ptr<Generation> generation_;
  std::deque<Pending> pending_;
  std::deque<std::shared_ptr<Slot>> conversions_;
  std::vector<std::weak_ptr<Slot>> old_;
  Json trace_ = Json::array(), final_source_ = Json::object();
  std::optional<SizeInt32> resize_;
  uint64_t trace_suppressed_{}, next_id_{};
  int64_t health_at_{}, device_probe_at_{};
  bool closed_{}, window_{};
  bool flush_pending_{};
  HANDLE completion_wait_{};
  uint64_t batch_flushes_{};
  bool shader_selected_{};
  bool compute_selected_{};
  bool bgra_shader_only_{};
  HRESULT shader_view_result_{E_NOTIMPL};
  HRESULT compute_texture_result_{E_NOTIMPL}, compute_view_result_{E_NOTIMPL};
  void healthy() {
    device_->thread();
    const auto now = qpc_hns();
    if (now - device_probe_at_ >= hns_per_second / 50) {
      device_->healthy();
      device_probe_at_ = now;
    }
  }
  void trace(const char *op) {
    if (!plan_.trace)
      return;
    if (trace_.size() < 128)
      trace_.push_back({{"operation", op},
                        {"qpcHns", qpc_hns()},
                        {"thread", GetCurrentThreadId()}});
    else
      ++trace_suppressed_;
  }
  void generation() {
    const auto sw = static_cast<UINT>((plan_.crop.width + 1) & ~1),
               sh = static_cast<UINT>((plan_.crop.height + 1) & ~1);
    const auto bytes =
        uint64_t(sw) * sh * 4 + uint64_t(plan_.width) * plan_.height * 3 / 2;
    // Retained old leases remain charged even after their source/pool is
    // replaced.
    old_.erase(std::remove_if(old_.begin(), old_.end(),
                              [](const auto &v) { return v.expired(); }),
               old_.end());
    if (stats_->bytes + bytes * plan_.slots > plan_.max_bytes)
      throw Error(NSS_E_BACKPRESSURE, NSS_STAGE_PROCESS, 0,
                  "Resize would exceed retained texture pool budget");
    auto next = std::make_unique<Generation>();
    next->id = ++stats_->generations;
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
    content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    content.InputFrameRate = {plan_.fps_num, plan_.fps_den};
    content.InputWidth = sw;
    content.InputHeight = sh;
    content.OutputFrameRate = content.InputFrameRate;
    content.OutputWidth = plan_.width;
    content.OutputHeight = plan_.height;
    content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
    checked(device_->video->CreateVideoProcessorEnumerator(
                &content, &next->video->enumerator),
            NSS_STAGE_PROCESS, "Video processor enumeration failed");
    UINT input{}, output{};
    checked(next->video->enumerator->CheckVideoProcessorFormat(
                DXGI_FORMAT_B8G8R8A8_UNORM, &input),
            NSS_STAGE_PROCESS, "BGRA video format query failed");
    checked(next->video->enumerator->CheckVideoProcessorFormat(DXGI_FORMAT_NV12,
                                                               &output),
            NSS_STAGE_PROCESS, "NV12 video format query failed");
    if (!(input & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT) ||
        !(output & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT))
      throw Error(NSS_E_UNAVAILABLE, NSS_STAGE_PROCESS, 0,
                  "BGRA to NV12 is unsupported on selected adapter");
    ComPtr<ID3D11VideoProcessorEnumerator1> enumeration;
    checked(next->video->enumerator.As(&enumeration), NSS_STAGE_PROCESS,
            "Video color conversion enumeration unavailable");
    BOOL supported{};
    checked(enumeration->CheckVideoProcessorFormatConversion(
                DXGI_FORMAT_B8G8R8A8_UNORM,
                DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709, DXGI_FORMAT_NV12,
                DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709, &supported),
            NSS_STAGE_PROCESS, "BT709 conversion query failed");
    if (!supported)
      throw Error(NSS_E_UNAVAILABLE, NSS_STAGE_PROCESS, 0,
                  "BT709 limited conversion is unsupported");
    checked(device_->video->CreateVideoProcessor(next->video->enumerator.Get(),
                                                 0, &next->video->processor),
            NSS_STAGE_PROCESS, "Video processor creation failed");
    auto *context = device_->video_context.Get();
    auto *processor = next->video->processor.Get();
    context->VideoProcessorSetStreamFrameFormat(
        processor, 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    context->VideoProcessorSetStreamAutoProcessingMode(processor, 0, FALSE);
    context->VideoProcessorSetStreamAlpha(processor, 0, FALSE, 1.0f);
    context->VideoProcessorSetStreamColorSpace1(
        processor, 0, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
    context->VideoProcessorSetOutputColorSpace1(
        processor, DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709);
    const RECT source{0, 0, plan_.crop.width, plan_.crop.height},
        dest{plan_.viewport.x, plan_.viewport.y,
             plan_.viewport.x + plan_.viewport.width,
             plan_.viewport.y + plan_.viewport.height},
        target{0, 0, static_cast<LONG>(plan_.width),
               static_cast<LONG>(plan_.height)};
    context->VideoProcessorSetStreamSourceRect(processor, 0, TRUE, &source);
    context->VideoProcessorSetStreamDestRect(processor, 0, TRUE, &dest);
    context->VideoProcessorSetOutputTargetRect(processor, TRUE, &target);
    D3D11_VIDEO_COLOR black{};
    black.YCbCr.Y = 16.0f / 255;
    black.YCbCr.Cb = black.YCbCr.Cr = 128.0f / 255;
    black.YCbCr.A = 1;
    context->VideoProcessorSetOutputBackgroundColor(processor, TRUE, &black);
    const bool allow_shader = (source_ || duplication_) && plan_.defer_conversion &&
        int64_t(plan_.crop.width) <= int64_t(plan_.viewport.width) * 2 &&
        int64_t(plan_.crop.height) <= int64_t(plan_.viewport.height) * 2;
    // A fully covered shader input is never rendered into. Odd crops still
    // need an RTV to clear their padding. Video-processor views are created
    // only after shader selection, since SRV-only inputs cannot be VP inputs.
    const bool shader_only_input = allow_shader &&
        !((plan_.crop.width | plan_.crop.height) & 1);
    bool compute_textures = allow_shader;
    compute_texture_result_ = compute_view_result_ = E_NOTIMPL;
    const auto unsupported = [](HRESULT result) {
      return result == E_INVALIDARG || result == E_NOINTERFACE || result == DXGI_ERROR_UNSUPPORTED;
    };
    for (uint32_t i = 0; i < plan_.slots; ++i) {
      auto slot = std::make_shared<Slot>();
      slot->device = device_;
      slot->stats = stats_;
      slot->generation = next->id;
      slot->video = next->video;
      D3D11_TEXTURE2D_DESC desc{};
      desc.Width = sw;
      desc.Height = sh;
      desc.MipLevels = desc.ArraySize = 1;
      desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
      desc.SampleDesc.Count = 1;
      desc.Usage = D3D11_USAGE_DEFAULT;
      desc.BindFlags = D3D11_BIND_SHADER_RESOURCE |
          (shader_only_input ? 0 : D3D11_BIND_RENDER_TARGET);
      checked(device_->device->CreateTexture2D(&desc, nullptr, &slot->bgra),
              NSS_STAGE_PROCESS, "Owned BGRA texture allocation failed");
      if (!shader_only_input)
        checked(device_->device->CreateRenderTargetView(slot->bgra.Get(), nullptr,
                                                        &slot->clear),
                NSS_STAGE_PROCESS, "BGRA padding view failed");
      desc.Width = plan_.width;
      desc.Height = plan_.height;
      desc.Format = DXGI_FORMAT_NV12;
      desc.BindFlags = D3D11_BIND_RENDER_TARGET |
          (compute_textures ? D3D11_BIND_UNORDERED_ACCESS : 0);
      auto texture_result = device_->device->CreateTexture2D(&desc, nullptr, &slot->nv12);
      if (compute_textures) compute_texture_result_ = texture_result;
      if (FAILED(texture_result) && compute_textures) {
        device_->healthy();
        if (!unsupported(texture_result))
          checked(texture_result, NSS_STAGE_PROCESS, "NV12 compute texture allocation failed");
        compute_textures = false;
        slot->nv12.Reset();
        desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        texture_result = device_->device->CreateTexture2D(&desc, nullptr, &slot->nv12);
      }
      checked(texture_result, NSS_STAGE_PROCESS, "NV12 output texture allocation failed");
      if (!device_->completion_fence) {
        D3D11_QUERY_DESC query{D3D11_QUERY_EVENT, 0};
        checked(device_->device->CreateQuery(&query, &slot->done),
                NSS_STAGE_PROCESS, "GPU completion query allocation failed");
      }
      slot->bytes = bytes;
      stats_->bytes += bytes;
      stats_->peak_bytes =
          (std::max)(stats_->peak_bytes.load(), stats_->bytes.load());
      live_capture_bytes += bytes;
      next->slots.push_back(std::move(slot));
    }
    shader_selected_ = false;
    compute_selected_ = false;
    shader_view_result_ = E_NOTIMPL;
    // The selected-screen path owns its immediate context and BGRA copy. Keep
    // camera, scene taps and larger reduction ratios on the existing video
    // processor. Probe actual NV12 plane views before choosing the shader.
    if (allow_shader && compute_textures) {
      compute_view_result_ = S_OK;
      for (auto &slot : next->slots) {
#ifdef NSS_CAPTURE_QUALIFICATION
        compute_view_result_ = (qualification_shader_view_failure || qualification_compute_view_failure) ? E_NOINTERFACE :
            Nv12Compute::views(device_->device.Get(), slot->bgra.Get(), slot->nv12.Get(), slot->compute_views);
#else
        compute_view_result_ = Nv12Compute::views(device_->device.Get(), slot->bgra.Get(),
                                                 slot->nv12.Get(), slot->compute_views);
#endif
        if (FAILED(compute_view_result_)) break;
      }
      if (SUCCEEDED(compute_view_result_)) {
        next->video->compute = std::make_unique<Nv12Compute>(device_->device.Get(),
            static_cast<uint32_t>(plan_.crop.width), static_cast<uint32_t>(plan_.crop.height),
            sw, sh, plan_.width, plan_.height, plan_.viewport);
        compute_selected_ = shader_selected_ = true;
        shader_view_result_ = compute_view_result_;
      } else {
        device_->healthy();
        if (!unsupported(compute_view_result_))
          checked(compute_view_result_, NSS_STAGE_PROCESS, "NV12 compute plane view allocation failed");
        for (auto &slot : next->slots) slot->compute_views = {};
      }
    }
    if (allow_shader && !compute_selected_) {
      shader_view_result_ = S_OK;
      for (auto &slot : next->slots) {
#ifdef NSS_CAPTURE_QUALIFICATION
        shader_view_result_ = qualification_shader_view_failure ? E_NOINTERFACE :
            Nv12Shader::views(device_->device.Get(), slot->bgra.Get(), slot->nv12.Get(), slot->shader_views);
#else
        shader_view_result_ = Nv12Shader::views(device_->device.Get(), slot->bgra.Get(),
                                              slot->nv12.Get(), slot->shader_views);
#endif
        if (FAILED(shader_view_result_)) break;
      }
      if (SUCCEEDED(shader_view_result_)) {
        next->video->shader = std::make_unique<Nv12Shader>(device_->device.Get(),
            static_cast<uint32_t>(plan_.crop.width), static_cast<uint32_t>(plan_.crop.height),
            sw, sh, plan_.width, plan_.height, plan_.viewport);
        shader_selected_ = true;
      } else {
        device_->healthy();
        if (shader_view_result_ != E_INVALIDARG && shader_view_result_ != E_NOINTERFACE &&
            shader_view_result_ != DXGI_ERROR_UNSUPPORTED)
          checked(shader_view_result_, NSS_STAGE_PROCESS, "NV12 shader view allocation failed");
        for (auto &slot : next->slots) slot->shader_views = {};
      }
    }
    if (!shader_selected_) {
      for (auto &slot : next->slots) {
        if (shader_only_input) {
          // Selection failed before any capture commands used this pool.
          // Drop partial shader views before replacing the incompatible BGRA
          // allocation; retained older generations are never modified.
          slot->compute_views = {};
          slot->shader_views = {};
          D3D11_TEXTURE2D_DESC desc{};
          slot->bgra->GetDesc(&desc);
          desc.BindFlags |= D3D11_BIND_RENDER_TARGET;
          slot->bgra.Reset();
          checked(device_->device->CreateTexture2D(&desc, nullptr, &slot->bgra),
                  NSS_STAGE_PROCESS, "Video fallback BGRA allocation failed");
          checked(device_->device->CreateRenderTargetView(slot->bgra.Get(), nullptr,
                                                          &slot->clear),
                  NSS_STAGE_PROCESS, "Video fallback BGRA view failed");
        }
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC input_desc{};
        input_desc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        checked(device_->video->CreateVideoProcessorInputView(
                    slot->bgra.Get(), next->video->enumerator.Get(), &input_desc,
                    &slot->input),
                NSS_STAGE_PROCESS, "Video processor input view failed");
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC output_desc{};
        output_desc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        checked(device_->video->CreateVideoProcessorOutputView(
                    slot->nv12.Get(), next->video->enumerator.Get(), &output_desc,
                    &slot->output),
                NSS_STAGE_PROCESS, "Video processor output view failed");
      }
    }
    bgra_shader_only_ = shader_only_input && shader_selected_;
    if (generation_)
      for (auto &slot : generation_->slots)
        old_.push_back(slot);
    generation_ = std::move(next);
    trace("generation-created");
  }
  void reap_conversions() {
    while (!conversions_.empty()) {
      const auto &slot = conversions_.front();
      ++stats_->queries;
      if (!device_->completed(slot->done.Get(), slot->completion_value))
        break;
      slot->return_acquisition();
      slot->nv12_ready = true;
      slot->conversion_queued = false;
      conversions_.pop_front();
    }
  }
  void submit_completion() {
    if (device_->pending_completion)
      driver_timings_.call(DriverOperation::Signal, [&] { device_->submit_completion(); });
  }
  void discard_unused_acquisitions(bool all = false) {
    if (!generation_) return;
    for (const auto &slot : generation_->slots) {
      if (slot->acquisition && !slot->conversion_queued &&
          (all || slot.use_count() == 1)) {
        slot->return_acquisition();
        ++stats_->uncopied_releases;
      }
    }
  }
  void queue_conversion(const std::shared_ptr<Slot> &slot) {
    if (slot->nv12_ready || slot->conversion_queued) return;
    ComPtr<ID3D11Texture2D> texture;
    if (slot->acquisition) {
      const auto size = slot->acquisition.ContentSize();
      auto access = slot->acquisition.Surface().as<IDirect3DDxgiInterfaceAccess>();
      checked(access->GetInterface(IID_PPV_ARGS(&texture)), NSS_STAGE_CAPTURE,
              "Selected WGC texture interface unavailable");
      ComPtr<ID3D11Device> origin;
      texture->GetDevice(&origin);
      D3D11_TEXTURE2D_DESC desc{};
      texture->GetDesc(&desc);
      if (origin.Get() != device_->device.Get() ||
          desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM ||
          desc.Width < static_cast<UINT>(size.Width) ||
          desc.Height < static_cast<UINT>(size.Height))
        throw Error(NSS_E_UNAVAILABLE, NSS_STAGE_PROCESS, 0,
                    "Selected WGC resource differs from its acquisition contract");
      (void)local_crop({0, 0, size.Width, size.Height}, slot->acquisition_crop, false);
    }
    // Retain before issuing commands, including when the selection is later
    // abandoned. Copy and conversion share one ordered GPU completion fence.
    conversions_.push_back(slot);
    slot->conversion_queued = true;
    if (texture) {
      const auto crop = slot->acquisition_crop;
      if ((crop.width & 1) || (crop.height & 1)) {
        const float black[] = {0, 0, 0, 1};
        device_->context->ClearRenderTargetView(slot->clear.Get(), black);
        ++stats_->padding_clears;
      } else {
        ++stats_->full_coverage_copies;
      }
      const D3D11_BOX box{static_cast<UINT>(crop.x), static_cast<UINT>(crop.y), 0,
                         static_cast<UINT>(crop.x + crop.width),
                         static_cast<UINT>(crop.y + crop.height), 1};
      device_->context->CopySubresourceRegion(slot->bgra.Get(), 0, 0, 0, 0,
                                              texture.Get(), 0, &box);
      ++stats_->copies;
      stats_->copy_bytes += uint64_t(crop.width) * crop.height * 4;
      trace("selected-acquisition;GPU-copy-crop;owned-padding");
      // The immediate-context copy is the last use of the borrowed surface.
      // Return the WGC frame now; later conversion reads only our owned BGRA
      // texture. Keep the owned slot and its completion fence until NV12 is
      // ready, so returning this source never exposes unfinished output.
      texture.Reset();
      slot->return_acquisition();
      ++stats_->submitted_source_returns;
    }
    const auto timing = conversion_timing_.begin(device_->device.Get(), device_->context.Get());
    HRESULT result = S_OK;
    if (slot->video->compute) {
      slot->video->compute->dispatch(device_->context.Get(), slot->compute_views);
      ++stats_->shader_converted;
    } else if (slot->video->shader) {
      slot->video->shader->draw(device_->context.Get(), slot->shader_views);
      ++stats_->shader_converted;
    } else {
      D3D11_VIDEO_PROCESSOR_STREAM stream{};
      stream.Enable = TRUE;
      stream.pInputSurface = slot->input.Get();
      result = device_->video_context->VideoProcessorBlt(
          slot->video->processor.Get(), slot->output.Get(), 0, 1, &stream);
    }
    conversion_timing_.end(device_->context.Get(), timing);
    slot->completion_value = device_->mark_completion(slot->done.Get(),
        (source_ || duplication_) && plan_.defer_conversion);
    flush_pending_ = true;
    checked(result, NSS_STAGE_PROCESS, "Selected BGRA crop/scale to NV12 failed");
    ++stats_->converted;
  }
  std::optional<Frame> completed() {
    reap_conversions();
    while (!pending_.empty() && pending_.front().delivered_before_completion) {
      const auto &copy = pending_.front();
      ++stats_->queries;
      if (!device_->completed(copy.slot->done.Get(), copy.copy_completion)) return {};
      if (duplication_) duplication_->release_completed(copy.id);
      // A selected conversion may already share this slot. Only release the
      // copy's retention; never reset its published pixels or conversion state.
      pending_.pop_front();
    }
    if (pending_.empty())
      return {};
    auto &job = pending_.front();
    ++stats_->queries;
    if (!device_->completed(job.slot->done.Get(), job.slot->completion_value))
      return {};
    if (job.source)
      job.source.Close();
    job.source = nullptr;
    job.slot->stage = SlotStage::Ready;
    job.slot->nv12_ready = !plan_.defer_conversion;
    trace("owned-copy-completed;WGC-frame-returned");
    Frame frame{std::make_shared<GpuSurface>(job.slot),
                job.id,
                job.slot->generation,
                job.at,
                job.at,
                job.at,
                plan_.width,
                plan_.height,
                0,
                camera_ != nullptr};
    if (plan_.defer_conversion) {
      frame.format = Frame::Format::Bgra;
      frame.width = static_cast<uint32_t>((plan_.crop.width + 1) & ~1);
      frame.height = static_cast<uint32_t>((plan_.crop.height + 1) & ~1);
    }
    pending_.pop_front();
    ++stats_->delivered;
    return frame;
  }

  std::optional<Frame> poll_duplication(int64_t minimum_source_time) {
    auto ready = driver_timings_.call(DriverOperation::Reap, [&] { return completed(); });
    auto image = duplication_->acquire();
    if (!image) return ready;
    if (plan_.defer_conversion && image->at <= minimum_source_time) {
      image->texture.Reset();
      duplication_->release();
      ++stats_->uncopied_releases;
      ++stats_->served_window_releases;
      return ready;
    }
    const auto found = std::find_if(generation_->slots.begin(), generation_->slots.end(),
        [](const auto &slot) { return slot.use_count() == 1 && slot->stage != SlotStage::Writing; });
    if (found == generation_->slots.end()) {
      image->texture.Reset();
      duplication_->release();
      ++stats_->pressure;
      return ready;
    }
    const auto &slot = *found;
    slot->stage = SlotStage::Writing;
    slot->nv12_ready = false;
    slot->conversion_queued = false;
    if ((plan_.crop.width & 1) || (plan_.crop.height & 1)) {
      const float black[] = {0, 0, 0, 1};
      device_->context->ClearRenderTargetView(slot->clear.Get(), black);
      ++stats_->padding_clears;
    } else ++stats_->full_coverage_copies;
    const D3D11_BOX box{static_cast<UINT>(plan_.crop.x), static_cast<UINT>(plan_.crop.y), 0,
        static_cast<UINT>(plan_.crop.x + plan_.crop.width),
        static_cast<UINT>(plan_.crop.y + plan_.crop.height), 1};
    driver_timings_.call(DriverOperation::Copy, [&] {
      const auto timing = copy_timing_.begin(device_->device.Get(), device_->context.Get());
      device_->context->CopySubresourceRegion(slot->bgra.Get(), 0, 0, 0, 0, image->texture.Get(), 0, &box);
      copy_timing_.end(device_->context.Get(), timing);
    });
    ++stats_->copies;
    stats_->copy_bytes += uint64_t(plan_.crop.width) * plan_.crop.height * 4;
    duplication_->copied(next_id_ + 1);
    slot->completion_value = driver_timings_.call(DriverOperation::Mark,
        [&] { return device_->mark_completion(slot->done.Get(), plan_.defer_conversion); });
    pending_.push_back({slot, nullptr, ++next_id_, image->at,
                        plan_.defer_conversion, slot->completion_value});
    if (!plan_.defer_conversion) queue_conversion(slot);
    // Later conversion reads only our owned BGRA slot, retained through its
    // real completion fence. Deferred capture returns the single borrowed
    // desktop at copy retirement, or before an earlier acquisition; eager
    // poll-only consumers return it now. The owner submits before waiting.
    flush_pending_ = true;
    image->texture.Reset();
    if (!plan_.defer_conversion) {
      duplication_->release();
      // Eager consumers also support a poll-only loop without flush_work().
      flush_work();
      return ready;
    }
    const auto &copy = pending_.back();
    Frame acquired{std::make_shared<GpuSurface>(slot), copy.id, slot->generation,
        copy.at, copy.at, copy.at, static_cast<uint32_t>((plan_.crop.width + 1) & ~1),
        static_cast<uint32_t>((plan_.crop.height + 1) & ~1), 0, false};
    acquired.format = Frame::Format::Bgra;
    ++stats_->delivered;
    return acquired;
  }
  std::optional<Frame> poll_camera() {
    auto ready = completed();
    auto frame = camera_->poll();
    if (!frame)
      return ready;
    auto found = std::find_if(generation_->slots.begin(),
                              generation_->slots.end(), [](const auto &slot) {
                                return slot.use_count() == 1 &&
                                       slot->stage != SlotStage::Writing;
                              });
    if (found == generation_->slots.end()) {
      ++stats_->pressure;
      return ready;
    }
    const auto &slot = *found;
    slot->stage = SlotStage::Writing;
    const auto pixels = frame->surface->cpu_pixels();
    device_->context->UpdateSubresource(slot->bgra.Get(), 0, nullptr,
                                        pixels.data(), frame->stride, 0);
    camera_upload_bytes_ += uint64_t(frame->stride) * frame->height;
    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.pInputSurface = slot->input.Get();
    checked(device_->video_context->VideoProcessorBlt(
                generation_->video->processor.Get(), slot->output.Get(), 0, 1,
                &stream),
            NSS_STAGE_PROCESS, "Camera RAM upload/scale to GPU NV12 failed");
    ++stats_->converted;
    slot->completion_value = device_->mark_completion(slot->done.Get());
    pending_.push_back({slot, nullptr, frame->id, frame->capture_hns});
    device_->context->Flush();
    return ready;
  }

public:
  GpuPipeline(Plan value, const camera::Plan &camera)
      : apartment_(false), plan_(std::move(value)),
        device_(std::make_shared<Device>(plan_.display.adapter_luid)),
        camera_(std::make_unique<camera::Source>(camera)) {
    generation();
  }
  GpuPipeline(Plan value, const camera::Plan& camera, std::shared_ptr<Device> device)
      : apartment_(false), plan_(std::move(value)), device_(std::move(device)),
        camera_(std::make_unique<camera::Source>(camera)) {
    if (device_->adapter_id != plan_.display.adapter_luid)
      throw Error(NSS_E_UNAVAILABLE,NSS_STAGE_PROCESS,0,"Shared camera device must match the scene adapter");
    generation();
  }
  GpuPipeline(Plan value, HWND window)
      : apartment_(!value.desktop_duplication), plan_(std::move(value)),
        device_(std::make_shared<Device>(plan_.display.adapter_luid, plan_.prefer_query_completion)),
        window_(window != nullptr) {
    try {
      if (plan_.desktop_duplication) {
        if (window)
          throw Error(NSS_E_ARGUMENT, NSS_STAGE_CAPTURE, 0,
                      "Desktop Duplication is an explicit monitor-only source");
        duplication_ = std::make_unique<Duplication>(device_, stats_, plan_, driver_timings_);
        generation();
        trace("DXGI-DuplicateOutput;owner-thread;bounded-pool");
        return;
      }
      source_ = std::make_unique<Wgc>(device_, plan_, window);
      if (window_ && plan_.full_source) {
        plan_.crop = {0, 0, source_->size.Width, source_->size.Height};
        plan_.viewport = fit_viewport(static_cast<uint32_t>(plan_.crop.width),
                                      static_cast<uint32_t>(plan_.crop.height),
                                      plan_.width, plan_.height);
      }
      generation();
      trace("WGC-CreateFreeThreaded;no-Dispatcher");
    } catch (const winrt::hresult_error &e) {
      throw Error(NSS_E_UNAVAILABLE, NSS_STAGE_CAPTURE, e.code(),
                  "WGC setup failed");
    }
  }
  ~GpuPipeline() override {
    try {
      close();
    } catch (...) {
    }
  }
  WaitHint wait_hint() const noexcept override {
    const auto awaiting_delivery = std::any_of(pending_.begin(), pending_.end(),
        [](const Pending &job) { return !job.delivered_before_completion; });
    if(camera_ && !closed_)
      return {camera_->wait_event(),awaiting_delivery || !conversions_.empty(),nullptr};
    if (duplication_ && !closed_)
      return {nullptr, awaiting_delivery || !conversions_.empty(), completion_wait_, 4};
    return source_ && !closed_
               ? WaitHint{source_->wake_event(), awaiting_delivery || !conversions_.empty(), completion_wait_}
               : WaitHint{};
  }
  void flush_work() override {
    if (closed_)
      return;
    device_->thread();
    if (flush_pending_) {
      submit_completion();
      driver_timings_.call(DriverOperation::Flush, [&] { device_->context->Flush(); });
      flush_pending_ = false;
      ++batch_flushes_;
    }
    // Only the direct screen recording owns this notification consumer.
    // Camera/shared scene devices keep their existing synchronization policy.
    if ((source_ || duplication_) && plan_.defer_conversion && device_->completion_fence) {
      const auto completed_value = device_->completed_fence_value();
      uint64_t target{}, ready{};
      const auto consider = [&](uint64_t value) {
        if (value > completed_value)
          target = target ? (std::min)(target, value) : value;
        else
          ready = ready ? (std::min)(ready, value) : value;
      };
      for (const auto& job : pending_)
        if (!job.delivered_before_completion) consider(job.slot->completion_value);
      for (const auto& slot : conversions_) consider(slot->completion_value);
      completion_wait_ = device_->completion_notification(target ? target : ready);
    }
  }
  void retire_completed_work() override {
    if (closed_) return;
    device_->thread();
    // DD already delivered these owned copies. Reap their actual completion
    // now so a served acquisition window does not keep the desktop held
    // through idle waiting, encoder output or recovery-checkpoint writes.
    if (duplication_ && plan_.defer_conversion) (void)completed();
    else reap_conversions();
    discard_unused_acquisitions();
  }
  bool prepare_frame(Frame &frame) override {
    if (!plan_.defer_conversion)
      return true;
    const auto *surface = dynamic_cast<const GpuSurface *>(frame.surface.get());
    if (!surface || surface->slot->device != device_)
      throw Error(NSS_E_ARGUMENT, NSS_STAGE_PROCESS, 0,
                  "Selected conversion requires a frame owned by this GPU pipeline");
    const auto &slot = surface->slot;
    if (!slot->nv12_ready) {
      healthy();
      if (closed_)
        throw Error(NSS_E_STATE, NSS_STAGE_PROCESS, 0,
                    "Cannot start conversion after GPU capture closes");
      reap_conversions();
      queue_conversion(slot);
      if (!slot->nv12_ready)
        return false;
    }
    frame.format = Frame::Format::Nv12;
    frame.width = plan_.width;
    frame.height = plan_.height;
    return true;
  }
  ReadbackPreparation prepare_readback(Frame &frame, Readback &readback) override {
    // Shared scene/camera contexts keep their own completion protocol.
    if(camera_ || (!source_ && !duplication_) || !plan_.defer_conversion)
      return ReadbackPreparation::Unsupported;
    device_->thread();
    const auto *surface=dynamic_cast<const GpuSurface *>(frame.surface.get());
    if(!surface || surface->slot->device!=device_)
      throw Error(NSS_E_ARGUMENT,NSS_STAGE_PROCESS,0,"Queued readback requires this pipeline's surface");
    const auto &slot=surface->slot;
    auto converted=frame;
    converted.format=Frame::Format::Nv12;
    converted.width=plan_.width;converted.height=plan_.height;
    if(duplication_ && compute_selected_ && !slot->nv12_ready && !slot->conversion_queued) {
      healthy();
      if(closed_)
        throw Error(NSS_E_STATE,NSS_STAGE_PROCESS,0,"Cannot queue conversion after GPU capture closes");
      struct Writer {GpuPipeline *pipeline;Slot *slot;} writer{this,slot.get()};
      const auto packed=readback.prepare_packed_queued(converted,device_->context.Get(),
          {&writer,[](void *state,void *view) {
            auto &work=*static_cast<Writer *>(state);
            auto &self=*work.pipeline;
            auto &item=*work.slot;
            item.video->compute->prepare_packed(self.device_->device.Get());
            const auto timing=self.conversion_timing_.begin(self.device_->device.Get(),self.device_->context.Get());
            item.video->compute->dispatch_packed(self.device_->context.Get(),item.compute_views.source.Get(),
                static_cast<ID3D11UnorderedAccessView *>(view));
            self.conversion_timing_.end(self.device_->context.Get(),timing);
            item.completion_value=self.device_->mark_completion(item.done.Get(),true);
            self.submit_completion();
            ++self.stats_->converted;++self.stats_->shader_converted;++self.stats_->packed_converted;
          }});
      if(packed.supported) {
        flush_pending_=flush_pending_ || packed.submitted;
        if(!packed.ready) return ReadbackPreparation::Pending;
        frame=std::move(converted);
        return ReadbackPreparation::Ready;
      }
    }
    if(!slot->nv12_ready) {
      healthy();
      if(closed_)
        throw Error(NSS_E_STATE,NSS_STAGE_PROCESS,0,"Cannot queue conversion after GPU capture closes");
      reap_conversions();
      queue_conversion(slot);
    }
    // The owned BGRA copy, NV12 conversion and staging copy are ordered on
    // the same immediate context. Only the staging fence permits CPU Map.
    // Neither the borrowed acquisition nor a generic getter changes contract.
    // Seal the upstream batch before the staging copy's own completion marker.
    // Thus readback readiness still implies that NV12's generic GPU fence has
    // completed, without issuing separate markers for copy and conversion.
    submit_completion();
    const auto result=readback.prepare_queued(converted,slot->nv12.Get(),device_->context.Get());
    flush_pending_=flush_pending_ || result.submitted;
    if(!result.ready) return ReadbackPreparation::Pending;
    reap_conversions();
    frame=std::move(converted);
    return ReadbackPreparation::Ready;
  }
  std::optional<Frame> poll() override {
    return poll_after((std::numeric_limits<int64_t>::min)());
  }
  std::optional<Frame> poll_after(int64_t minimum_source_time) override {
    driver_timings_.call(DriverOperation::DeviceHealth, [&] { healthy(); });
    if (closed_)
      throw Error(NSS_E_STATE, NSS_STAGE_CAPTURE, 0, "GPU capture is closed");
    if (camera_)
      return poll_camera();
    const auto now = qpc_hns();
    if (now - health_at_ >= hns_per_second) {
      driver_timings_.call(DriverOperation::MonitorHealth, [&] {
      require_interactive_desktop();
      if (!window_) {
        const auto list = displays();
        const auto found =
            std::find_if(list.begin(), list.end(), [&](const Display &d) {
              return d.id == plan_.display.id;
            });
        if (found == list.end() || !found->color_known || found->hdr ||
            found->adapter_luid != plan_.display.adapter_luid)
          throw Error(NSS_E_UNAVAILABLE, NSS_STAGE_CAPTURE, 0,
                      "Selected SDR monitor/adaptor is no longer available");
      }
      health_at_ = now;
      });
    }
    if (duplication_) return poll_duplication(minimum_source_time);
    try {
      auto ready = completed();
      // Return unselected acquisitions on the owner thread before asking WGC
      // for another frame; otherwise its finite frame pool could be exhausted.
      discard_unused_acquisitions();
      if (resize_ && pending_.empty() && conversions_.empty()) {
        const auto size = *resize_;
        if (!plan_.full_source)
          (void)local_crop({0, 0, size.Width, size.Height}, plan_.crop, false);
        else {
          plan_.crop = {0, 0, size.Width, size.Height};
          plan_.viewport = fit_viewport(static_cast<uint32_t>(size.Width),
                                        static_cast<uint32_t>(size.Height),
                                        plan_.width, plan_.height);
        }
        source_->recreate(device_, size);
        generation();
        resize_.reset();
        trace("source-resize;new-generation");
        return ready;
      }
      if (resize_) return ready;
      auto frame = source_->pop();
      if (!frame)
        return ready;
      const auto size = frame.ContentSize();
      if (size.Width <= 0 || size.Height <= 0)
        throw Error(NSS_E_UNAVAILABLE, NSS_STAGE_CAPTURE, 0,
                    "WGC content is empty");
      if (size.Width != source_->size.Width ||
          size.Height != source_->size.Height) {
        frame.Close();
        ++stats_->resize_rejected;
        resize_ = size;
        // A pool recreation must not invalidate an acquisition retained by
        // the CFR selector. Materialize the remaining old generation first.
        if (plan_.defer_conversion)
          for (const auto &slot : generation_->slots)
            if (slot->acquisition) queue_conversion(slot);
        return ready;
      }
      auto found = std::find_if(generation_->slots.begin(),
                                generation_->slots.end(), [](const auto &slot) {
                                  return slot.use_count() == 1 &&
                                         slot->stage != SlotStage::Writing;
                                });
      if (found == generation_->slots.end()) {
        ++stats_->pressure;
        frame.Close();
        return ready;
      }
      const auto &slot = *found;
      const auto at = frame.SystemRelativeTime().count();
      if (at < 0)
        throw Error(NSS_E_UNAVAILABLE, NSS_STAGE_CAPTURE, 0,
                    "WGC acquisition timestamp is invalid");
      if (plan_.defer_conversion) {
        slot->nv12_ready = false;
        slot->conversion_queued = false;
        slot->acquisition_crop = plan_.crop;
        slot->acquisition = std::move(frame);
        ++stats_->deferred_sources;
        Frame acquired{std::make_shared<GpuSurface>(slot), ++next_id_, slot->generation,
                       at, at, at, static_cast<uint32_t>((plan_.crop.width + 1) & ~1),
                       static_cast<uint32_t>((plan_.crop.height + 1) & ~1), 0, false};
        acquired.format = Frame::Format::Bgra;
        ++stats_->delivered;
        return acquired;
      }
      auto access = frame.Surface().as<IDirect3DDxgiInterfaceAccess>();
      ComPtr<ID3D11Texture2D> texture;
      checked(access->GetInterface(IID_PPV_ARGS(&texture)), NSS_STAGE_CAPTURE,
              "WGC texture interface unavailable");
      ComPtr<ID3D11Device> origin;
      texture->GetDevice(&origin);
      if (origin.Get() != device_->device.Get())
        throw Error(NSS_E_UNAVAILABLE, NSS_STAGE_PROCESS, 0,
                    "Cross-device texture transfer was not negotiated");
      D3D11_TEXTURE2D_DESC desc{};
      texture->GetDesc(&desc);
      if (desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM ||
          desc.Width < static_cast<UINT>(size.Width) ||
          desc.Height < static_cast<UINT>(size.Height))
        throw Error(NSS_E_UNAVAILABLE, NSS_STAGE_PROCESS, 0,
                    "WGC resource does not cover valid content");
      (void)local_crop({0, 0, size.Width, size.Height}, plan_.crop, false);
      slot->stage = SlotStage::Writing;
      slot->nv12_ready = false;
      slot->conversion_queued = false;
      // Odd crops leave one padded row/column in the even-sized texture.
      // Even crops overwrite the complete BGRA resource with this copy.
      if ((plan_.crop.width & 1) || (plan_.crop.height & 1)) {
        const float black[] = {0, 0, 0, 1};
        device_->context->ClearRenderTargetView(slot->clear.Get(), black);
        ++stats_->padding_clears;
      } else {
        ++stats_->full_coverage_copies;
      }
      const D3D11_BOX box{static_cast<UINT>(plan_.crop.x),
                          static_cast<UINT>(plan_.crop.y),
                          0,
                          static_cast<UINT>(plan_.crop.x + plan_.crop.width),
                          static_cast<UINT>(plan_.crop.y + plan_.crop.height),
                          1};
      device_->context->CopySubresourceRegion(slot->bgra.Get(), 0, 0, 0, 0,
                                              texture.Get(), 0, &box);
      ++stats_->copies;
      stats_->copy_bytes += uint64_t(plan_.crop.width) * plan_.crop.height * 4;
      trace("GPU-copy-selected-crop;black-owned-padding");
      if (!plan_.defer_conversion) {
        D3D11_VIDEO_PROCESSOR_STREAM stream{};
        stream.Enable = TRUE;
        stream.pInputSurface = slot->input.Get();
        checked(device_->video_context->VideoProcessorBlt(
                    generation_->video->processor.Get(), slot->output.Get(), 0, 1,
                    &stream),
                NSS_STAGE_PROCESS, "BGRA crop/scale to NV12 failed");
        ++stats_->converted;
      }
      slot->completion_value = device_->mark_completion(slot->done.Get(), plan_.defer_conversion);
      pending_.push_back({slot, std::move(frame), ++next_id_, at});
      if (plan_.defer_conversion)
        flush_pending_ = true;
      else
        device_->context->Flush();
      trace("VideoProcessorBlt;query-end;Flush-no-wait");
      return ready;
    } catch (const winrt::hresult_error &e) {
      throw Error(NSS_E_UNAVAILABLE, NSS_STAGE_CAPTURE, e.code(),
                  "WGC acquisition/resize failed");
    }
  }
  Json diagnostic_acquisition_history() const override {
    return duplication_ ? duplication_->acquisition_history() : Json::object();
  }
  Json metrics() const override {
    Json result = camera_   ? camera_->metrics()
                  : source_ ? source_->metrics()
                  : duplication_ ? duplication_->metrics()
                            : final_source_;
    result["backend"] =
        camera_ ? "directshow-native-ram-to-d3d11" : plan_.desktop_duplication
            ? "dxgi-desktop-duplication-experimental" : "wgc-freethreaded";
    result["cameraCpuToGpuUploadBytes"] = camera_upload_bytes_;
    result["codeLifetime"] =
        camera_ ? "Native camera callback code retained; source/GPU resources "
                  "released normally"
                : plan_.desktop_duplication ? "Owner-thread duplication; owned resources retained through GPU completion"
                : "WGC system image and native delegate module pinned after "
                  "first GPU "
                  "prepare; capture resources released normally";
    result["processor"] = shader_selected_ ? "native-shader-triangular-BT709-limited-NV12"
                                            : "d3d11-video-processor-BT709-limited-NV12";
    result["shaderConversionSelected"] = shader_selected_;
    result["computeConversionSelected"] = compute_selected_;
    result["ownedBgraShaderOnly"] = bgra_shader_only_;
    result["videoProcessorViewsSelected"] = !shader_selected_;
    result["shaderConversionMethod"] = compute_selected_ ? "compute" : (shader_selected_ ? "raster" : "video-processor");
    result["computeTextureProbeResult"] = compute_texture_result_;
    result["computePlaneViewProbeResult"] = compute_view_result_;
    result["shaderPlaneViewProbeResult"] = shader_view_result_;
    result["shaderConvertedFrames"] = stats_->shader_converted.load();
    result["packedReadbackConvertedFrames"] = stats_->packed_converted.load();
    if (plan_.trace)
      result["gpuTimingDiagnostic"] = {{"desktopCopy", copy_timing_.metrics()},
                                      {"selectedConversion", conversion_timing_.metrics()}};
    result["device"] = device_->info;
    result["completionMechanism"] = device_->completion_fence
        ? "owner-context monotonic D3D11 GPU fence" : "D3D11 event query";
    result["timelineFenceSignals"] = device_->fence_signals;
    result["timelineFenceReservedValues"] = device_->next_completion;
    result["timelineFenceCoalescedMarkers"] = device_->coalesced_completions;
    result["timelineFenceUnsubmittedValue"] = device_->pending_completion;
    result["timelineFenceReads"] = device_->fence_reads;
    result["timelineFenceCompletedEvidenceReuse"] = device_->fence_cache_hits;
    result["eventQueryReads"] = device_->query_reads;
    result["gpuCompletionEventAvailable"] = bool(device_->completion_event) && !device_->notification_disabled;
    result["gpuCompletionEventResult"] = device_->notification_result;
    result["gpuCompletionEventRegistrations"] = device_->notification_registrations;
    result["gpuCompletionEventRegistrationFailures"] = device_->notification_failures;
    result["gpuCompletionEventPendingValue"] = device_->notification_value;
    result["gpuCopies"] = stats_->copies.load();
    result["gpuCopyBytes"] = stats_->copy_bytes.load();
    result["acquisitionCopyDeferredUntilSelection"] = !camera_ && !plan_.desktop_duplication && plan_.defer_conversion;
    result["ownedCopyDeliveredBeforeCompletion"] = !camera_ && plan_.desktop_duplication && plan_.defer_conversion;
    result["duplicationCopyFlushDeferredToOwner"] = plan_.desktop_duplication && plan_.defer_conversion;
    result["retainedUnreturnedAcquisitions"] = stats_->deferred_sources.load();
    result["acquisitionsReturnedWithoutGpuCopy"] = stats_->uncopied_releases.load();
    result["servedWindowAcquisitionsReturnedWithoutCopy"] = stats_->served_window_releases.load();
    result["acquisitionsReturnedAfterCopySubmission"] = stats_->submitted_source_returns.load();
    result["paddingClears"] = stats_->padding_clears.load();
    result["fullCoverageCopiesWithoutClear"] = stats_->full_coverage_copies.load();
    result["converted"] = stats_->converted.load();
    result["conversionPolicy"] = plan_.defer_conversion
        ? "selected output or active preview frames; completion query before use"
        : "eager acquisition conversion";
    result["inflightConversions"] = conversions_.size();
    result["ownerBatchFlushes"] = batch_flushes_;
    if (plan_.trace && plan_.desktop_duplication)
      result["driverCallTiming"] = driver_timings_.report();
    result["deviceFallbackProbeIntervalMs"] = 20;
    result["delivered"] = stats_->delivered.load();
    result["leasesReleased"] = stats_->released.load();
    result["poolPressureDrops"] = stats_->pressure.load();
    result["poolBytes"] = stats_->bytes.load();
    result["poolPeakBytes"] = stats_->peak_bytes.load();
    result["resizeFramesRejected"] = stats_->resize_rejected.load();
    result["poolByteBasis"] = "logical BGRA plus NV12 texture payload; driver "
                              "allocation overhead separate";
    result["maxPoolBytes"] = plan_.max_bytes;
    result["generations"] = stats_->generations.load();
    result["inflightCopies"] = pending_.size();
    result["completionQueries"] = stats_->queries.load();
    result["recordingReadbackBytes"] = 0;
    result["recordingReadbackCalls"] = 0;
    result["previewReadbackBytes"] = 0;
    result["diagnosticReadbackBytes"] = 0;
    result["trace"] = trace_;
    result["traceSuppressed"] = trace_suppressed_;
    result["slotStates"] = Json::array();
    if (generation_)
      for (const auto &slot : generation_->slots)
        result["slotStates"].push_back(
            static_cast<uint32_t>(slot->stage.load()));
    return result;
  }
  void close() override try {
    device_->thread();
    if (closed_)
      return;
    closed_ = true;
    completion_wait_ = nullptr;
    // Stop acquisition first, but keep the frame pool alive until every
    // submitted copy has completed and its borrowed WGC frame is returned.
    if (source_)
      source_->stop();
    if (camera_)
      camera_->close();
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    submit_completion();
    device_->context->Flush();
    flush_pending_ = false;
    while (!pending_.empty() || !conversions_.empty()) {
      auto completed_frame = completed();
      if (!completed_frame) {
        device_->healthy();
        if (std::chrono::steady_clock::now() >= deadline)
          throw Error(NSS_E_TIMEOUT, NSS_STAGE_PROCESS, 0,
                      "GPU copies/conversions did not quiesce during capture stop");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    }
    discard_unused_acquisitions(true);
    if ((source_ || duplication_) && plan_.defer_conversion && device_->notification_value) {
      const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
          deadline - std::chrono::steady_clock::now()).count();
      const auto wait_ms = static_cast<DWORD>((std::clamp)(remaining, int64_t{0}, int64_t{2000}));
      const auto notified = WaitForSingleObject(device_->completion_event.get(), wait_ms);
      if (notified != WAIT_OBJECT_0)
        throw Error(NSS_E_TIMEOUT, NSS_STAGE_PROCESS,
                    notified == WAIT_FAILED ? HRESULT_FROM_WIN32(GetLastError()) : 0,
                    "GPU completion notification did not quiesce during capture stop");
      (void)device_->completion_notification(0);
    }
    trace("callbacks-stopped;GPU-work-complete;resources-released");
    if (stats_->shader_converted) {
      // This direct-screen device has no shared camera/scene context owner.
      // Release shader/state bindings here, on its owner, before retained
      // leases can later destroy the device on another caller thread.
      device_->context->ClearState();
      device_->context->Flush();
    }
    generation_.reset();
    if (source_) {
      source_->close();
      final_source_ = source_->metrics();
    }
    source_.reset();
    if (duplication_) {
      duplication_->close();
      final_source_ = duplication_->metrics();
      duplication_.reset();
    }
    device_->winrt_device = nullptr;
  } catch (const winrt::hresult_error &e) {
    throw Error(NSS_E_UNAVAILABLE, NSS_STAGE_CAPTURE, e.code(),
                "WGC stop/release failed");
  }
};
} // namespace
std::unique_ptr<Pipeline> make_gpu(const Plan &value) {
  if (!value.gpu)
    throw Error(NSS_E_ARGUMENT, NSS_STAGE_PLAN, 0,
                "GPU factory requires a strict GPU capture plan");
  return std::make_unique<GpuPipeline>(value, nullptr);
}
std::unique_ptr<Pipeline> make_camera_gpu(const camera::Plan &source,
                                          const Plan &output) {
  if (!output.gpu)
    throw Error(NSS_E_ARGUMENT, NSS_STAGE_PLAN, 0,
                "GPU camera requires explicit hardware processing");
  return std::make_unique<GpuPipeline>(output, source);
}
std::unique_ptr<Pipeline> make_camera_gpu_shared(const camera::Plan& source,
                                                 const Plan& output, const Frame& reference) {
  const auto* surface=dynamic_cast<const GpuSurface*>(reference.surface.get());
  if (!output.gpu || !surface)
    throw Error(NSS_E_ARGUMENT,NSS_STAGE_PLAN,0,"Scene camera requires a native shared GPU device reference");
  return std::make_unique<GpuPipeline>(output,source,surface->slot->device);
}
#ifdef NSS_CAPTURE_QUALIFICATION
// Internal native qualification entry: window resize/Closed exercise the same
// WGC pool implementation without changing the user's physical monitor
// configuration.
std::unique_ptr<Pipeline> make_gpu_for_test_window(const Plan &value,
                                                   void *window) {
  return std::make_unique<GpuPipeline>(value, static_cast<HWND>(window));
}
#endif
} // namespace nss::capture
