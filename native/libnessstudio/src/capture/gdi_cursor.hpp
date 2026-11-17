#pragma once
#include <windows.h>
#include <cstdint>
#include <limits>

namespace nss::capture::detail {
enum class CursorStage { None, Query, Copy, Information, Position, Draw };
inline const char *cursor_stage_name(CursorStage stage) noexcept {
  switch (stage) {
  case CursorStage::Query: return "query";
  case CursorStage::Copy: return "copy";
  case CursorStage::Information: return "information";
  case CursorStage::Position: return "position";
  case CursorStage::Draw: return "draw";
  default: return "none";
  }
}
struct CursorResult {
  CursorStage failure = CursorStage::None;
  DWORD error{};
  unsigned requeries{};
  bool drawn{}, restore_background{};
};
struct SystemCursorApi {
  bool query(CURSORINFO &value) { return GetCursorInfo(&value) != FALSE; }
  HICON copy(HICON value) { return CopyIcon(value); }
  bool information(HICON icon, ICONINFO &value) {
    return GetIconInfo(icon, &value) != FALSE;
  }
  bool position(POINT &value) { return GetPhysicalCursorPos(&value) != FALSE; }
  bool draw(HDC dc, int x, int y, HICON icon) {
    return DrawIconEx(dc, x, y, icon, 0, 0, 0, nullptr, DI_NORMAL) != FALSE;
  }
  void destroy(HICON icon) { (void)DestroyIcon(icon); }
  void release(HBITMAP bitmap) { (void)DeleteObject(bitmap); }
};

// A cursor belongs to another application and may disappear between Win32
// calls. Own its image before reading its hotspot or drawing. Only a failed
// copy is retried, with one fresh query; drawing is never repeated over the
// same pixels. Cursor omissions are reported separately from screen capture.
template<class Api>
CursorResult draw_cursor_snapshot(Api &api, HDC dc, int origin_x, int origin_y) {
  CursorResult result;
  const auto failed = [&](CursorStage stage) {
    result.failure = stage;
    const auto error = GetLastError();
    result.error = error ? error : ERROR_GEN_FAILURE;
    result.restore_background = stage == CursorStage::Draw;
    return result;
  };
  for (unsigned attempt = 0; attempt != 2; ++attempt) {
    CURSORINFO cursor{};
    cursor.cbSize = sizeof(cursor);
    SetLastError(ERROR_SUCCESS);
    if (!api.query(cursor)) return failed(CursorStage::Query);
    if (!(cursor.flags & CURSOR_SHOWING) || (cursor.flags & CURSOR_SUPPRESSED))
      return result;
    struct OwnedIcon {
      Api &api;
      HICON value;
      ~OwnedIcon() { if (value) api.destroy(value); }
    } icon{api, nullptr};
    SetLastError(ERROR_SUCCESS);
    icon.value = api.copy(cursor.hCursor);
    if (!icon.value) {
      if (attempt == 0) { ++result.requeries; continue; }
      return failed(CursorStage::Copy);
    }
    struct OwnedInformation {
      Api &api;
      ICONINFO value{};
      ~OwnedInformation() {
        if (value.hbmMask) api.release(value.hbmMask);
        if (value.hbmColor) api.release(value.hbmColor);
      }
    } info{api};
    SetLastError(ERROR_SUCCESS);
    if (!api.information(icon.value, info.value))
      return failed(CursorStage::Information);
    POINT position{};
    SetLastError(ERROR_SUCCESS);
    if (!api.position(position)) return failed(CursorStage::Position);
    const int64_t x = int64_t(position.x) - origin_x - info.value.xHotspot;
    const int64_t y = int64_t(position.y) - origin_y - info.value.yHotspot;
    if (x < (std::numeric_limits<int>::min)() || x > (std::numeric_limits<int>::max)() ||
        y < (std::numeric_limits<int>::min)() || y > (std::numeric_limits<int>::max)()) {
      SetLastError(ERROR_INVALID_PARAMETER);
      return failed(CursorStage::Position);
    }
    SetLastError(ERROR_SUCCESS);
    if (!api.draw(dc, static_cast<int>(x), static_cast<int>(y), icon.value))
      return failed(CursorStage::Draw);
    result.drawn = true;
    return result;
  }
  return result;
}
} // namespace nss::capture::detail
