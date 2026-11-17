#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "capture/gdi_cursor.hpp"
#include <algorithm>
#include <climits>
#include <iostream>
#include <stdexcept>
using namespace nss::capture::detail;
namespace {
void check(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}
struct Canvas {
  HDC dc = CreateCompatibleDC(nullptr);
  HBITMAP bitmap{};
  HGDIOBJ previous{};
  uint32_t *pixels{};
  Canvas() {
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = 64; info.bmiHeader.biHeight = -64;
    info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32;
    bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS,
                             reinterpret_cast<void **>(&pixels), nullptr, 0);
    check(dc && bitmap, "Test DIB creation failed");
    previous = SelectObject(dc, bitmap);
    clear();
  }
  void clear() { (void)GdiFlush(); std::fill_n(pixels, 64 * 64, 0x00402010U); }
  bool changed() const {
    (void)GdiFlush();
    for (unsigned i = 0; i != 64 * 64; ++i)
      if ((pixels[i] & 0xffffffU) != 0x402010U) return true;
    return false;
  }
  ~Canvas() { SelectObject(dc, previous); DeleteObject(bitmap); DeleteDC(dc); }
};
struct Api : SystemCursorApi {
  HICON borrowed = CopyIcon(LoadCursor(nullptr, IDC_ARROW));
  CursorStage fail = CursorStage::None;
  unsigned copy_failures{}, queries{}, copies{}, destroys{}, draws{};
  bool invalidate_at_position{}, hidden{}, suppressed{}, extreme_position{};
  HICON drawn_icon{};
  Api() { check(borrowed != nullptr, "Test cursor creation failed"); }
  ~Api() { if (borrowed) DestroyIcon(borrowed); }
  bool rejects(CursorStage stage) {
    if (fail != stage) return false;
    SetLastError(ERROR_INVALID_HANDLE); return true;
  }
  bool query(CURSORINFO &value) {
    ++queries;
    if (rejects(CursorStage::Query)) return false;
    check(value.cbSize == sizeof(value), "Cursor query structure invalid");
    value.flags = hidden ? 0 : CURSOR_SHOWING;
    if (suppressed) value.flags |= CURSOR_SUPPRESSED;
    value.hCursor = reinterpret_cast<HCURSOR>(borrowed);
    return true;
  }
  HICON copy(HICON value) {
    ++copies;
    if (copy_failures) { --copy_failures; SetLastError(ERROR_INVALID_HANDLE); return nullptr; }
    if (rejects(CursorStage::Copy)) return nullptr;
    return SystemCursorApi::copy(value);
  }
  bool information(HICON icon, ICONINFO &value) {
    const bool ok = SystemCursorApi::information(icon, value);
    // Deliberately fail after allocating both bitmaps to exercise cleanup.
    return rejects(CursorStage::Information) ? false : ok;
  }
  bool position(POINT &value) {
    if (invalidate_at_position) {
      check(DestroyIcon(borrowed) != FALSE, "Owner failed to destroy its cursor");
      borrowed = nullptr;
    }
    if (rejects(CursorStage::Position)) return false;
    value = extreme_position ? POINT{LONG_MIN, LONG_MIN} : POINT{12, 12};
    return true;
  }
  bool draw(HDC dc, int x, int y, HICON icon) {
    ++draws; drawn_icon = icon;
    if (rejects(CursorStage::Draw)) {
      SetPixel(dc, 0, 0, RGB(255, 0, 0));
      SetLastError(ERROR_SUCCESS); // A failed API may provide no useful error.
      return false;
    }
    return SystemCursorApi::draw(dc, x, y, icon);
  }
  void destroy(HICON value) { ++destroys; SystemCursorApi::destroy(value); }
};
}
int main() {
  try {
    Canvas canvas;
    // Reproduce the previous sequence with a real cursor whose owner goes away.
    {
      Api api; api.invalidate_at_position = true;
      const auto borrowed = api.borrowed;
      ICONINFO info{};
      check(GetIconInfo(borrowed, &info) != FALSE, "Legacy icon-info failed too early");
      POINT position{}; api.position(position);
      SetLastError(ERROR_SUCCESS);
      check(!DrawIconEx(canvas.dc, 12, 12, borrowed, 0, 0, 0, nullptr, DI_NORMAL),
            "Borrowed-icon invalidation did not reproduce the old failure");
      if (info.hbmMask) DeleteObject(info.hbmMask);
      if (info.hbmColor) DeleteObject(info.hbmColor);
    }
    canvas.clear();
    {
      Api api; api.invalidate_at_position = true;
      const auto result = draw_cursor_snapshot(api, canvas.dc, 0, 0);
      check(result.drawn && result.failure == CursorStage::None && canvas.changed() &&
                api.draws == 1 && api.destroys == 1,
            "Owned snapshot failed after the cursor owner released its image");
    }
    for (bool suppress : {false, true}) {
      Api api; api.hidden = !suppress; api.suppressed = suppress;
      canvas.clear();
      const auto result = draw_cursor_snapshot(api, canvas.dc, 0, 0);
      check(!result.drawn && result.failure == CursorStage::None && !api.copies &&
                !canvas.changed(), "Hidden/suppressed cursor was allocated or drawn");
    }
    {
      Api api; api.copy_failures = 1;
      const auto result = draw_cursor_snapshot(api, canvas.dc, 0, 0);
      check(result.drawn && result.requeries == 1 && api.queries == 2 && api.draws == 1,
            "Cursor copy race did not recover with one fresh query");
    }
    const auto baseline_gdi = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    const auto baseline_user = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
    for (unsigned cycle = 0; cycle != 500; ++cycle) {
      for (auto stage : {CursorStage::None, CursorStage::Query, CursorStage::Copy,
                        CursorStage::Information, CursorStage::Position, CursorStage::Draw}) {
        Api api; api.fail = stage;
        canvas.clear();
        const auto result = draw_cursor_snapshot(api, canvas.dc, -2, -2);
        check(result.failure == stage, "Failed cursor operation was hidden or misclassified");
        check(result.drawn == (stage == CursorStage::None), "Cursor success was misreported");
        check(result.restore_background == (stage == CursorStage::Draw),
              "Partial cursor draw did not request restoration of desktop pixels");
        check(api.draws <= 1 && api.queries <= 2 && api.copies <= 2,
              "Cursor failure exceeded its bounded work");
        if (stage != CursorStage::None)
          check(result.error != ERROR_SUCCESS, "Failed cursor operation was reported as success");
      }
    }
    (void)GdiFlush();
    check(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS) == baseline_gdi &&
              GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS) == baseline_user,
          "Cursor/bitmap handles leaked across successful and failed acquisitions");
    {
      Api api; api.extreme_position = true;
      const auto result = draw_cursor_snapshot(api, canvas.dc, INT_MAX, INT_MAX);
      check(result.failure == CursorStage::Position && api.draws == 0,
            "Extreme cursor coordinates overflowed into a drawable location");
    }
    std::cout << "PASS: borrowed lifetime failure reproduced; owned lifetime, bounded retry, "
                 "suppression, partial draw, coordinate bounds and 3000 resource cycles\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n'; return 1;
  }
}
