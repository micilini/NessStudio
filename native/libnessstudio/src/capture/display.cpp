#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "capture.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <shellscalingapi.h>
#include <windows.h>

namespace nss::capture {
void checked(int32_t result, uint32_t stage, const char *operation) {
  if (result < 0)
    throw Error(NSS_E_UNAVAILABLE, stage, result, operation);
}
static std::string utf8(const wchar_t *text) {
  const auto count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text,
                                         -1, nullptr, 0, nullptr, nullptr);
  if (!count)
    throw Error(NSS_E_UNAVAILABLE, NSS_STAGE_CAPTURE,
                HRESULT_FROM_WIN32(GetLastError()), "Invalid display identity");
  std::string value(static_cast<size_t>(count), '\0');
  WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, -1, value.data(),
                      count, nullptr, nullptr);
  value.pop_back();
  return value;
}
struct DpiScope {
  DPI_AWARENESS_CONTEXT previous =
      SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  ~DpiScope() {
    if (previous)
      SetThreadDpiAwarenessContext(previous);
  }
};
void require_interactive_desktop() {
  const auto input = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
  if (!input)
    throw Error(NSS_E_UNAVAILABLE, NSS_STAGE_CAPTURE,
                HRESULT_FROM_WIN32(GetLastError()),
                "Interactive input desktop is unavailable");
  struct owner {
    HDESK value;
    ~owner() { CloseDesktop(value); }
  } opened{input};
  wchar_t active[256]{}, current[256]{};
  DWORD bytes{};
  if (!GetUserObjectInformationW(input, UOI_NAME, active, sizeof(active),
                                 &bytes) ||
      !GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()),
                                 UOI_NAME, current, sizeof(current), &bytes))
    throw Error(NSS_E_UNAVAILABLE, NSS_STAGE_CAPTURE,
                HRESULT_FROM_WIN32(GetLastError()),
                "Desktop identity query failed");
  if (std::wcscmp(active, current) != 0)
    throw Error(NSS_E_UNAVAILABLE, NSS_STAGE_CAPTURE, 0,
                "Capture desktop is no longer the interactive input desktop");
}
std::vector<Display> displays() {
  DpiScope scope;
  std::vector<Display> result;
  struct Context {
    std::vector<Display> *values;
    std::exception_ptr error;
  } context{&result, {}};
  const auto ok = EnumDisplayMonitors(
      nullptr, nullptr,
      [](HMONITOR monitor, HDC, LPRECT, LPARAM opaque) -> BOOL {
        auto &ctx = *reinterpret_cast<Context *>(opaque);
        try {
          MONITORINFOEXW info{};
          info.cbSize = sizeof(info);
          if (!GetMonitorInfoW(monitor, &info))
            throw Error(NSS_E_UNAVAILABLE, NSS_STAGE_CAPTURE,
                        HRESULT_FROM_WIN32(GetLastError()),
                        "Monitor information failed");
          Display display;
          display.monitor = monitor;
          display.name = utf8(info.szDevice);
          display.id = display.name;
          display.bounds = {info.rcMonitor.left, info.rcMonitor.top,
                            info.rcMonitor.right - info.rcMonitor.left,
                            info.rcMonitor.bottom - info.rcMonitor.top};
          display.primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
          UINT x{}, y{};
          if (SUCCEEDED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &x, &y)))
            display.dpi = x;
          ctx.values->push_back(std::move(display));
          return TRUE;
        } catch (...) {
          ctx.error = std::current_exception();
          return FALSE;
        }
      },
      reinterpret_cast<LPARAM>(&context));
  if (context.error)
    std::rethrow_exception(context.error);
  if (!ok)
    throw Error(NSS_E_UNAVAILABLE, NSS_STAGE_CAPTURE,
                HRESULT_FROM_WIN32(GetLastError()),
                "Display enumeration failed");
  std::vector<DISPLAYCONFIG_PATH_INFO> paths;
  std::vector<DISPLAYCONFIG_MODE_INFO> modes;
  LONG query = ERROR_INSUFFICIENT_BUFFER;
  for (int attempt = 0; attempt < 4 && query == ERROR_INSUFFICIENT_BUFFER;
       ++attempt) {
    UINT32 np{}, nm{};
    query = GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm);
    if (query != ERROR_SUCCESS)
      break;
    paths.resize(np);
    modes.resize(nm);
    query = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, paths.data(), &nm,
                               modes.data(), nullptr);
    paths.resize(np);
    modes.resize(nm);
  }
  if (query == ERROR_SUCCESS)
    for (const auto &path : paths) {
      DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
      source.header = {DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME,
                       sizeof(source), path.sourceInfo.adapterId,
                       path.sourceInfo.id};
      if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS)
        continue;
      const auto name = utf8(source.viewGdiDeviceName);
      for (auto &display : result)
        if (display.name == name) {
          display.adapter_luid = (uint64_t(static_cast<uint32_t>(
                                      path.sourceInfo.adapterId.HighPart))
                                  << 32) |
                                 path.sourceInfo.adapterId.LowPart;
          display.source_id = path.sourceInfo.id;
          display.target_id = path.targetInfo.id;
          DISPLAYCONFIG_TARGET_DEVICE_NAME target{};
          target.header = {DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME,
                           sizeof(target), path.targetInfo.adapterId,
                           path.targetInfo.id};
          if (DisplayConfigGetDeviceInfo(&target.header) == ERROR_SUCCESS &&
              target.monitorDevicePath[0])
            display.id = utf8(target.monitorDevicePath);
          DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO color{};
          color.header = {DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO,
                          sizeof(color), path.targetInfo.adapterId,
                          path.targetInfo.id};
          display.color_known =
              DisplayConfigGetDeviceInfo(&color.header) == ERROR_SUCCESS;
          display.hdr = display.color_known && color.advancedColorEnabled;
        }
    }
  std::stable_sort(result.begin(), result.end(),
                   [](const Display &a, const Display &b) {
                     return a.primary && !b.primary;
                   });
  return result;
}
Json display_json(const Display &d) {
  return {{"id", d.id},
          {"name", d.name},
          {"primary", d.primary},
          {"x", d.bounds.x},
          {"y", d.bounds.y},
          {"width", d.bounds.width},
          {"height", d.bounds.height},
          {"dpi", d.dpi},
          {"adapterLuid", d.adapter_luid},
          {"sdrKnown", d.color_known},
          {"hdr", d.hdr},
          {"coordinates", "physical-pixels"}};
}
Rect local_crop(Rect monitor, Rect selection, bool global) {
  int64_t x = selection.x, y = selection.y;
  if (global) {
    x -= monitor.x;
    y -= monitor.y;
  }
  if (x < 0 || y < 0 || selection.width <= 0 || selection.height <= 0 ||
      x + selection.width > monitor.width ||
      y + selection.height > monitor.height)
    throw Error(NSS_E_ARGUMENT, NSS_STAGE_PLAN, 0,
                "Crop exceeds the physical source rectangle");
  return {static_cast<int32_t>(x), static_cast<int32_t>(y), selection.width,
          selection.height};
}
Rect fit_viewport(uint32_t sw, uint32_t sh, uint32_t width, uint32_t height) {
  if (!sw || !sh || width < 2 || height < 2)
    throw Error(NSS_E_ARGUMENT, NSS_STAGE_PLAN, 0, "Invalid aspect geometry");
  uint32_t cw = width, ch = height;
  if (uint64_t(sw) * height > uint64_t(sh) * width)
    ch = static_cast<uint32_t>(uint64_t(width) * sh / sw);
  else
    cw = static_cast<uint32_t>(uint64_t(height) * sw / sh);
  cw &= ~1u;
  ch &= ~1u;
  if (!cw || !ch)
    throw Error(NSS_E_ARGUMENT, NSS_STAGE_PLAN, 0,
                "Aspect ratio has no even NV12 viewport");
  return {static_cast<int32_t>(((width - cw) / 2) & ~1u),
          static_cast<int32_t>(((height - ch) / 2) & ~1u),
          static_cast<int32_t>(cw), static_cast<int32_t>(ch)};
}
static uint64_t unsigned_value(const Json &object, const char *key,
                               uint64_t fallback,
                               uint64_t maximum = UINT32_MAX) {
  if (!object.is_object())
    throw Error(NSS_E_ARGUMENT, NSS_STAGE_PLAN, 0,
                "Capture control must be an object");
  if (!object.contains(key))
    return fallback;
  const auto &value = object.at(key);
  if (!value.is_number_integer() ||
      (!value.is_number_unsigned() && value.get<int64_t>() < 0))
    throw Error(NSS_E_ARGUMENT, NSS_STAGE_PLAN, 0,
                "Capture integer field has an invalid type or sign");
  const auto result = value.get<uint64_t>();
  if (result > maximum)
    throw Error(NSS_E_ARGUMENT, NSS_STAGE_PLAN, 0,
                "Capture integer field exceeds its domain");
  return result;
}
static int32_t coordinate(const Json &object, const char *key) {
  const auto &value = object.at(key);
  if (!value.is_number_integer() ||
      (value.is_number_unsigned() && value.get<uint64_t>() > INT32_MAX))
    throw Error(NSS_E_ARGUMENT, NSS_STAGE_PLAN, 0,
                "Physical coordinates must be signed 32-bit integers");
  const auto result = value.get<int64_t>();
  if (result < INT32_MIN || result > INT32_MAX)
    throw Error(NSS_E_ARGUMENT, NSS_STAGE_PLAN, 0,
                "Physical coordinate overflow");
  return static_cast<int32_t>(result);
}
Plan plan(const Json &request) {
  try {
    if (!request.is_object() || unsigned_value(request, "version", 0) != 1)
      throw Error(NSS_E_VERSION, NSS_STAGE_PLAN, 0,
                  "Unsupported capture request version");
    for (const auto &entry : request.items())
      if (entry.key() != "version" && entry.key() != "mode" &&
          entry.key() != "monitor" && entry.key() != "crop" &&
          entry.key() != "output" && entry.key() != "cursor" &&
          entry.key() != "slots" && entry.key() != "maxBytes" &&
          entry.key() != "fpsNum" && entry.key() != "fpsDen" &&
          entry.key() != "durationMs" && entry.key() != "trace" &&
          entry.key() != "sourceApi")
        throw Error(NSS_E_ARGUMENT, NSS_STAGE_PLAN, 0,
                    "Unknown capture control field");
    Plan result;
    const auto mode =
        request.value("mode", std::string("RequireGpuResidentHardware"));
    if (mode != "RequireGpuResidentHardware" && mode != "FullSoftware")
      throw Error(NSS_E_ARGUMENT, NSS_STAGE_PLAN, 0,
                  "Capture mode must be explicitly GPU or FullSoftware");
    result.gpu = mode != "FullSoftware";
    const auto api = request.value("sourceApi", std::string("Auto"));
    if (api != "Auto" && api != "WindowsGraphicsCapture" && api != "DesktopDuplication")
      throw Error(NSS_E_ARGUMENT, NSS_STAGE_PLAN, 0, "Unknown monitor capture API");
    if (!result.gpu && api != "Auto")
      throw Error(NSS_E_ARGUMENT, NSS_STAGE_PLAN, 0, "Explicit GPU capture API requires a GPU capture mode");
    result.desktop_duplication = api == "DesktopDuplication";
    if (result.desktop_duplication && request.value("cursor", true))
      throw Error(NSS_E_UNAVAILABLE, NSS_STAGE_PLAN, 0,
                  "Experimental Desktop Duplication requires cursor=false until cursor composition is qualified");
    const auto list = displays();
    const auto id = request.value("monitor", std::string("primary"));
    const auto found =
        std::find_if(list.begin(), list.end(), [&](const auto &d) {
          return id == "primary" ? d.primary : d.id == id;
        });
    if (found == list.end())
      throw Error(NSS_E_UNAVAILABLE, NSS_STAGE_CAPTURE, 0,
                  "Selected monitor is not available");
    result.display = *found;
    if (!result.display.color_known || result.display.hdr)
      throw Error(NSS_E_UNAVAILABLE, NSS_STAGE_PROCESS, 0,
                  "Only a positively identified SDR desktop is qualified; "
                  "HDR/tone mapping is unavailable");
    result.crop = {0, 0, result.display.bounds.width,
                   result.display.bounds.height};
    if (request.contains("crop")) {
      const auto &c = request.at("crop");
      if (!c.is_object())
        throw Error(NSS_E_ARGUMENT, NSS_STAGE_PLAN, 0,
                    "Crop must be an object");
      for (const auto &v : c.items())
        if (v.key() != "x" && v.key() != "y" && v.key() != "width" &&
            v.key() != "height" && v.key() != "space")
          throw Error(NSS_E_ARGUMENT, NSS_STAGE_PLAN, 0, "Unknown crop field");
      const auto space = c.value("space", std::string("monitor-physical"));
      if (space != "monitor-physical" && space != "global-physical")
        throw Error(NSS_E_ARGUMENT, NSS_STAGE_PLAN, 0,
                    "Unknown crop coordinate space");
      result.crop =
          local_crop(result.display.bounds,
                     {coordinate(c, "x"), coordinate(c, "y"),
                      coordinate(c, "width"), coordinate(c, "height")},
                     space == "global-physical");
      result.full_source = false;
    }
    const auto output = request.value("output", Json::object());
    if (!output.is_object())
      throw Error(NSS_E_ARGUMENT, NSS_STAGE_PLAN, 0,
                  "Output must be an object");
    for (const auto &v : output.items())
      if (v.key() != "width" && v.key() != "height" && v.key() != "oddPolicy")
        throw Error(NSS_E_ARGUMENT, NSS_STAGE_PLAN, 0, "Unknown output field");
    result.requested_width = static_cast<uint32_t>(unsigned_value(
        output, "width", static_cast<uint32_t>(result.crop.width)));
    result.requested_height = static_cast<uint32_t>(unsigned_value(
        output, "height", static_cast<uint32_t>(result.crop.height)));
    const auto odd = output.value("oddPolicy", std::string("Reject"));
    if (odd != "Reject" && odd != "PadEven")
      throw Error(NSS_E_ARGUMENT, NSS_STAGE_PLAN, 0,
                  "Unknown odd-dimension policy");
    result.pad_even = odd == "PadEven";
    if (!result.requested_width || !result.requested_height ||
        result.requested_width > 4096 || result.requested_height > 4096 ||
        (!result.pad_even &&
         ((result.requested_width | result.requested_height) & 1)))
      throw Error(NSS_E_ARGUMENT, NSS_STAGE_PLAN, 0,
                  "NV12 output must be even, or explicitly padded, and at most "
                  "4096 pixels");
    result.width = (result.requested_width + 1) & ~1u;
    result.height = (result.requested_height + 1) & ~1u;
    result.viewport = fit_viewport(static_cast<uint32_t>(result.crop.width),
                                   static_cast<uint32_t>(result.crop.height),
                                   result.width, result.height);
    result.fps_num =
        static_cast<uint32_t>(unsigned_value(request, "fpsNum", 30));
    result.fps_den =
        static_cast<uint32_t>(unsigned_value(request, "fpsDen", 1));
    if (!result.fps_num || !result.fps_den || result.fps_den > 100000 ||
        result.fps_num < result.fps_den ||
        uint64_t(result.fps_num) > 120ULL * result.fps_den)
      throw Error(NSS_E_ARGUMENT, NSS_STAGE_PLAN, 0,
                  "Invalid rational capture cadence");
    result.duration_ms = static_cast<uint32_t>(
        unsigned_value(request, "durationMs", 2000, 120000));
    if (result.duration_ms < 100)
      throw Error(NSS_E_ARGUMENT, NSS_STAGE_PLAN, 0,
                  "Capture probe must run at least 100 ms");
    result.cursor = request.value("cursor", true);
    result.trace = request.value("trace", false);
    result.slots = static_cast<uint32_t>(
        unsigned_value(request, "slots", result.gpu ? 4 : 3, 8));
    result.max_bytes =
        unsigned_value(request, "maxBytes",
                       result.gpu ? 128ULL * 1024 * 1024 : 64ULL * 1024 * 1024,
                       256ULL * 1024 * 1024);
    const auto pixels = uint64_t((result.crop.width + 1) & ~1) *
                        ((result.crop.height + 1) & ~1);
    const auto bytes =
        (pixels * 4 + uint64_t(result.width) * result.height * 3 / 2) *
        result.slots;
    if (result.slots < 2 || result.slots > 8 || !result.max_bytes ||
        result.max_bytes > 256ULL * 1024 * 1024 || bytes > result.max_bytes)
      throw Error(NSS_E_BACKPRESSURE, NSS_STAGE_PLAN, 0,
                  "Capture/process pools exceed the configured memory budget");
    return result;
  } catch (const Json::exception &) {
    throw Error(
        NSS_E_ARGUMENT, NSS_STAGE_PLAN, 0,
        "Invalid capture control field type or missing crop coordinate");
  }
}
Json plan_json(const Plan &p) {
  return {{"mode", p.gpu ? "RequireGpuResidentHardware" : "FullSoftware"},
          {"sourceApi", !p.gpu ? "Gdi" : p.desktop_duplication ? "DesktopDuplication" : "WindowsGraphicsCapture"},
          {"display", display_json(p.display)},
          {"crop",
           {{"x", p.crop.x},
            {"y", p.crop.y},
            {"width", p.crop.width},
            {"height", p.crop.height}}},
          {"requestedWidth", p.requested_width},
          {"requestedHeight", p.requested_height},
          {"width", p.width},
          {"height", p.height},
          {"viewport",
           {{"x", p.viewport.x},
            {"y", p.viewport.y},
            {"width", p.viewport.width},
            {"height", p.viewport.height}}},
          {"oddPolicy", p.pad_even ? "PadEven" : "Reject"},
          {"aspect", "Fit; even centered viewport; black padding"},
          {"filter", p.gpu ? "D3D11 Video Processor driver scaling; reference "
                             "comparison qualified per adapter"
                           : "bilinear"},
          {"color", "BT709-limited-SDR"},
          {"fpsNum", p.fps_num},
          {"fpsDen", p.fps_den},
          {"slots", p.slots},
          {"maxBytes", p.max_bytes},
          {"cursor", p.cursor}};
}
} // namespace nss::capture
