#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "capture/capture.hpp"
#include "visual_marker.hpp"
#include <condition_variable>
#include <dwmapi.h>
#include <future>
#include <mutex>
#include <thread>
#include <windows.h>

namespace fixture {
inline void check(bool value, const char *text) {
  if (!value)
    throw std::runtime_error(text);
}
class Window {
  std::thread thread_;
  std::atomic<HWND> window_{};
  std::mutex mutex_;
  std::condition_variable painted_;
  uint32_t width_, height_, mode_{}, number_{};
  uint64_t paints_{};
  int32_t x_, y_;
  HCURSOR cursor_{};
  std::vector<uint8_t> pixels_, marker_background_;
  bool moving_marker_{};
  uint32_t marker_row_{32};
  std::exception_ptr failure_;
  void image() {
    pixels_.assign(size_t(width_) * height_ * 4, 255);
    for (uint32_t y = 0; y < height_; ++y)
      for (uint32_t x = 0; x < width_; ++x) {
        auto *p = pixels_.data() + (size_t(y) * width_ + x) * 4;
        if (mode_ == 2) {
          p[0] = p[1] = p[2] = 0;
        } else if (mode_ == 1) {
          const bool inside = x >= 96 && x < 429 && y >= 128 && y < 369;
          p[0] = 255;
          p[1] = inside ? 255 : 0;
          p[2] = 255;
        } else {
          const auto stripe = (uint64_t(x) * 8 / width_);
          const uint8_t colors[8][3] = {
              {0, 0, 0},   {255, 255, 255}, {0, 0, 255},   {0, 255, 0},
              {255, 0, 0}, {0, 255, 255},   {255, 255, 0}, {255, 0, 255}};
          for (int c = 0; c < 3; ++c)
            p[c] = colors[stripe][c];
          if (y > height_ * 3 / 4)
            p[0] = p[1] = p[2] =
                static_cast<uint8_t>(uint64_t(x) * 255 / (width_ - 1));
          if (!moving_marker_ && number_ && y >= 32 && y < 64) {
            const auto bit = (x / 24) % 16;
            p[0] = p[1] = p[2] = (number_ & (1u << bit)) ? 255 : 0;
          }
        }
      }
    if (mode_ == 0) {
      HDC screen = GetDC(nullptr), memory = CreateCompatibleDC(screen);
      BITMAPINFO info{};
      info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
      info.bmiHeader.biWidth = static_cast<LONG>(width_);
      info.bmiHeader.biHeight = -static_cast<LONG>(height_);
      info.bmiHeader.biPlanes = 1;
      info.bmiHeader.biBitCount = 32;
      void *bits{};
      HBITMAP bitmap =
          CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
      check(bitmap && memory, "Fixture DIB failed");
      const auto previous = SelectObject(memory, bitmap);
      std::memcpy(bits, pixels_.data(), pixels_.size());
      SetBkColor(memory, RGB(0, 0, 0));
      SetTextColor(memory, RGB(255, 255, 255));
      constexpr wchar_t sample_text[] = L"NessStudio CAPTURE TEXT 0123456789";
      TextOutW(memory, 16, 80, sample_text,
               static_cast<int>(sizeof(sample_text) / sizeof(wchar_t) - 1));
      check(GdiFlush() != 0, "Fixture GdiFlush failed");
      std::memcpy(pixels_.data(), bits, pixels_.size());
      SelectObject(memory, previous);
      DeleteObject(bitmap);
      DeleteDC(memory);
      ReleaseDC(nullptr, screen);
    }
    if (moving_marker_) {
      marker_background_ = pixels_;
      number_strip();
    }
  }
  void number_strip() {
    if (moving_marker_) {
      const auto previous = size_t(marker_row_) * width_ * 4;
      std::memcpy(pixels_.data() + previous, marker_background_.data() + previous,
                  size_t(width_) * 32 * 4);
      marker_row_ = marker::row(number_);
      for (uint32_t y = marker_row_; y < marker_row_ + 32; ++y)
        for (uint32_t x = 0; x < width_; ++x) {
          auto *pixel = pixels_.data() + (size_t(y) * width_ + x) * 4;
          pixel[0] = pixel[1] = pixel[2] = marker::level(number_, x);
          pixel[3] = 255;
        }
      return;
    }
    for (uint32_t y = 32; y < (std::min)(height_, uint32_t{64}); ++y)
      for (uint32_t x = 0; x < width_; ++x) {
        auto *pixel = pixels_.data() + (size_t(y) * width_ + x) * 4;
        const auto value =
            static_cast<uint8_t>((number_ & (1u << ((x / 24) % 16))) ? 255 : 0);
        pixel[0] = pixel[1] = pixel[2] = value;
        pixel[3] = 255;
      }
  }
  static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wp,
                                    LPARAM lp) {
    auto *self =
        reinterpret_cast<Window *>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
      self = static_cast<Window *>(
          reinterpret_cast<CREATESTRUCTW *>(lp)->lpCreateParams);
      SetWindowLongPtrW(window, GWLP_USERDATA,
                        reinterpret_cast<LONG_PTR>(self));
    }
    if (!self)
      return DefWindowProcW(window, message, wp, lp);
    if (message == WM_ERASEBKGND)
      return 1;
    if (message == WM_SETCURSOR && self->cursor_) {
      SetCursor(self->cursor_);
      return TRUE;
    }
    if (message == WM_APP + 3) {
      self->cursor_ = reinterpret_cast<HCURSOR>(lp);
      SetCursor(self->cursor_);
      return 0;
    }
    if (message == WM_APP + 4) {
      std::lock_guard lock(self->mutex_);
      const auto previous_row = self->marker_row_;
      self->number_ = static_cast<uint32_t>(lp);
      self->number_strip();
      const auto top = self->moving_marker_ ? (std::min)(previous_row, self->marker_row_) : 32;
      const auto bottom = self->moving_marker_ ? (std::max)(previous_row, self->marker_row_) + 32 : 64;
      const RECT changed{0, static_cast<LONG>(top), static_cast<LONG>(self->width_), static_cast<LONG>(bottom)};
      InvalidateRect(window, &changed, FALSE);
      return 0;
    }
    if (message == WM_APP + 7) {
      std::lock_guard lock(self->mutex_);
      self->moving_marker_ = self->width_ == 1920 && self->height_ >= 1024;
      self->image();
      InvalidateRect(window, nullptr, FALSE);
      return self->moving_marker_ ? 1 : 0;
    }
    if (message == WM_APP + 5) {
      std::lock_guard lock(self->mutex_);
      for (uint32_t y = 128; y < (std::min)(self->height_, uint32_t{256}); ++y)
        for (uint32_t x = 32; x < (std::min)(self->width_, uint32_t{160}); ++x) {
          auto *p = self->pixels_.data() + (size_t(y) * self->width_ + x) * 4;
          p[0] = p[1] = p[2] = wp ? 255 : 0;
        }
      const RECT changed{32, 128, 160, 256};
      InvalidateRect(window, &changed, FALSE);
      return 0;
    }
    if (message == WM_APP + 1) {
      std::lock_guard lock(self->mutex_);
      self->mode_ = static_cast<uint32_t>(wp);
      self->number_ = static_cast<uint32_t>(lp);
      self->image();
      InvalidateRect(window, nullptr, FALSE);
      return 0;
    }
    if (message == WM_APP + 2) {
      const auto w = LOWORD(lp), h = HIWORD(lp);
      {
        std::lock_guard lock(self->mutex_);
        self->width_ = w;
        self->height_ = h;
        self->image();
      }
      SetWindowPos(window, HWND_TOPMOST, static_cast<int32_t>(wp), self->y_, w,
                   h, SWP_NOACTIVATE);
      InvalidateRect(window, nullptr, FALSE);
      return 0;
    }
    if (message == WM_PAINT) {
      PAINTSTRUCT paint{};
      const auto dc = BeginPaint(window, &paint);
      {
        std::lock_guard lock(self->mutex_);
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = static_cast<LONG>(self->width_);
        const auto first=std::clamp(paint.rcPaint.top,LONG{0},static_cast<LONG>(self->height_));
        const auto last=std::clamp(paint.rcPaint.bottom,first,static_cast<LONG>(self->height_));
        const auto rows=static_cast<UINT>(last-first);
        info.bmiHeader.biHeight = -static_cast<LONG>(rows);
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        // WM_APP+4 changes only the numbered strip. Upload the invalidated rows
        // rather than marshalling the entire desktop bitmap on every 60Hz paint.
        // Pixel content, source cadence and GdiFlush completion are unchanged.
        if(rows)
          SetDIBitsToDevice(dc, 0, first, self->width_, rows, 0, 0, 0,
                            rows, self->pixels_.data()+size_t(first)*self->width_*4, &info,
                            DIB_RGB_COLORS);
        GdiFlush();
        ++self->paints_;
        self->painted_.notify_all();
      }
      EndPaint(window, &paint);
      return 0;
    }
    if (message == WM_CLOSE) {
      DestroyWindow(window);
      return 0;
    }
    if (message == WM_DESTROY) {
      PostQuitMessage(0);
      return 0;
    }
    return DefWindowProcW(window, message, wp, lp);
  }

public:
  Window(nss::capture::Rect rect)
      : width_(static_cast<uint32_t>(rect.width)),
        height_(static_cast<uint32_t>(rect.height)), x_(rect.x), y_(rect.y) {
    std::promise<void> ready;
    auto start = ready.get_future();
    thread_ = std::thread([&, this] {
      try {
        SetThreadDpiAwarenessContext(
            DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        const auto instance = GetModuleHandleW(nullptr);
        const auto name = L"NessStudio.NativeCaptureFixture";
        WNDCLASSW klass{};
        klass.lpfnWndProc = procedure;
        klass.hInstance = instance;
        klass.lpszClassName = name;
        const auto atom = RegisterClassW(&klass);
        check(atom || GetLastError() == ERROR_CLASS_ALREADY_EXISTS,
              "Fixture registration failed");
        {
          std::lock_guard lock(mutex_);
          image();
        }
        window_ =
            CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, name,
                            L"NessStudio capture fixture", WS_POPUP, x_, y_,
                            static_cast<int>(width_), static_cast<int>(height_),
                            nullptr, nullptr, instance, this);
        check(window_ != nullptr, "Fixture window creation failed");
        ShowWindow(window_, SW_SHOWNOACTIVATE);
        UpdateWindow(window_);
        DwmFlush();
        ready.set_value();
        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
          TranslateMessage(&message);
          DispatchMessageW(&message);
        }
        window_ = nullptr;
        UnregisterClassW(name, instance);
      } catch (...) {
        failure_ = std::current_exception();
        try {
          ready.set_exception(failure_);
        } catch (...) {
        }
      }
    });
    try {
      start.get();
    } catch (...) {
      if (thread_.joinable())
        thread_.join();
      throw;
    }
  }
  ~Window() { close(); }
  HWND handle() const { return window_; }
  void cursor(HCURSOR value) {
    SendMessageW(window_, WM_APP + 3, 0, reinterpret_cast<LPARAM>(value));
  }
  void close() {
    if (window_)
      PostMessageW(window_, WM_CLOSE, 0, 0);
    if (thread_.joinable())
      thread_.join();
  }
  void pattern(uint32_t mode, uint32_t number = 0) {
    uint64_t previous;
    {
      std::lock_guard lock(mutex_);
      previous = paints_;
    }
    SendMessageW(window_, WM_APP + 1, mode, number);
    std::unique_lock lock(mutex_);
    check(painted_.wait_for(lock, std::chrono::seconds(2),
                            [&] { return paints_ > previous; }),
          "Fixture paint timed out");
    lock.unlock();
    DwmFlush();
  }
  void resize(uint16_t w, uint16_t h, int32_t x) {
    SendMessageW(window_, WM_APP + 2, static_cast<WPARAM>(x), MAKELPARAM(w, h));
    DwmFlush();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  std::vector<uint8_t> reference() {
    std::lock_guard lock(mutex_);
    return pixels_;
  }
};
} // namespace fixture
