#pragma once
// clang-format off
#include <windows.h>
#include <avrt.h>
// clang-format on

namespace nss::capture::detail {
// Applies only to the creating capture/processing thread. No registry, global
// timer, power-plan or process-priority setting is changed.
class CaptureTask final {
  DWORD index_{};
  HANDLE task_ = AvSetMmThreadCharacteristicsW(L"Capture", &index_);
  DWORD error_ = task_ ? 0 : GetLastError();

public:
  CaptureTask() {
    if (task_ && !AvSetMmThreadPriority(task_, AVRT_PRIORITY_HIGH))
      error_ = GetLastError();
  }
  CaptureTask(const CaptureTask &) = delete;
  CaptureTask &operator=(const CaptureTask &) = delete;
  ~CaptureTask() {
    if (task_)
      AvRevertMmThreadCharacteristics(task_);
  }
  bool registered() const noexcept { return task_ != nullptr; }
  DWORD error() const noexcept { return error_; }
};
} // namespace nss::capture::detail
