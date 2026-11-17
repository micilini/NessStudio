#pragma once
#include "capture.hpp"
#include <functional>
#include <windows.h>

namespace nss::capture::detail {
// One auxiliary CPU thread per pipeline. Submitted rows are joined before any
// borrowed DIB/tap table or writable destination can leave its owner scope.
class CpuWorker final {
  PTP_POOL pool_{};
  PTP_WORK work_{};
  TP_CALLBACK_ENVIRON environment_{};
  std::function<void()> job_;
  std::exception_ptr failure_;
  std::atomic<DWORD> thread_id_{};

public:
  uint64_t completed{};
  CpuWorker() {
    pool_ = CreateThreadpool(nullptr);
    if (!pool_)
      checked(HRESULT_FROM_WIN32(GetLastError()), NSS_STAGE_PROCESS,
              "CPU processing pool unavailable");
    SetThreadpoolThreadMaximum(pool_, 1);
    if (!SetThreadpoolThreadMinimum(pool_, 1)) {
      const auto error = GetLastError();
      CloseThreadpool(pool_);
      pool_ = nullptr;
      checked(HRESULT_FROM_WIN32(error), NSS_STAGE_PROCESS,
              "CPU processing worker unavailable");
    }
    InitializeThreadpoolEnvironment(&environment_);
    SetThreadpoolCallbackPool(&environment_, pool_);
    work_ = CreateThreadpoolWork(
        [](PTP_CALLBACK_INSTANCE, void *context, PTP_WORK) {
          auto &self = *static_cast<CpuWorker *>(context);
          try {
            const auto id = GetCurrentThreadId();
            if (self.thread_id_.load(std::memory_order_relaxed) != id) {
              self.thread_id_.store(id, std::memory_order_relaxed);
              (void)SetThreadDescription(GetCurrentThread(), L"Ness CPU pixels");
            }
            self.job_();
          } catch (...) {
            self.failure_ = std::current_exception();
          }
        },
        this, &environment_);
    if (!work_) {
      const auto error = GetLastError();
      DestroyThreadpoolEnvironment(&environment_);
      CloseThreadpool(pool_);
      pool_ = nullptr;
      checked(HRESULT_FROM_WIN32(error), NSS_STAGE_PROCESS,
              "CPU processing work allocation failed");
    }
  }
  CpuWorker(const CpuWorker &) = delete;
  CpuWorker &operator=(const CpuWorker &) = delete;
  DWORD thread_id() const noexcept { return thread_id_.load(std::memory_order_relaxed); }
  ~CpuWorker() {
    if (work_) {
      WaitForThreadpoolWorkCallbacks(work_, FALSE);
      CloseThreadpoolWork(work_);
    }
    DestroyThreadpoolEnvironment(&environment_);
    if (pool_)
      CloseThreadpool(pool_);
  }
  template <class Remote, class Local>
  void parallel(Remote remote, Local local) {
    job_ = std::move(remote);
    failure_ = nullptr;
    SubmitThreadpoolWork(work_);
    std::exception_ptr local_failure;
    try {
      local();
    } catch (...) {
      local_failure = std::current_exception();
    }
    WaitForThreadpoolWorkCallbacks(work_, FALSE);
    job_ = {};
    ++completed;
    if (local_failure)
      std::rethrow_exception(local_failure);
    if (failure_)
      std::rethrow_exception(failure_);
  }
};
} // namespace nss::capture::detail
