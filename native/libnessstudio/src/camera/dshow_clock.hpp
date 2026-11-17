#pragma once
#include "dshow_types.hpp"
#include <array>
#include <mutex>
#include <optional>
#include <thread>

namespace nss::camera::detail {
// The graph's reference clock uses exactly the engine's QPC/100ns domain.
// IMediaFilter::Run supplies stream zero; callback arrival is not an offset.
class QpcClock final : public IReferenceClock {
  struct Advice { DWORD_PTR cookie{}; int64_t at{},period{}; HANDLE handle{}; };
  std::atomic<ULONG> refs_{1};
  std::atomic<HRESULT> failure_{S_OK};
  std::atomic<int64_t> last_time_{};
  std::mutex gate_;
  std::array<std::optional<Advice>,64> advice_{};
  DWORD_PTR serial_{1};
  HANDLE changed_{},timer_{};
  bool stopping_{};
  std::thread worker_;
  HRESULT add(int64_t at,int64_t period,HANDLE handle,DWORD_PTR *cookie) noexcept;
  void run() noexcept;
  ~QpcClock();
public:
  QpcClock();
  STDMETHODIMP QueryInterface(REFIID,void **) override;
  STDMETHODIMP_(ULONG) AddRef() override {return ++refs_;}
  STDMETHODIMP_(ULONG) Release() override {const auto count=--refs_;if(!count) delete this;return count;}
  STDMETHODIMP GetTime(REFERENCE_TIME *) override;
  STDMETHODIMP AdviseTime(REFERENCE_TIME,REFERENCE_TIME,HEVENT,DWORD_PTR *) override;
  STDMETHODIMP AdvisePeriodic(REFERENCE_TIME,REFERENCE_TIME,HSEMAPHORE,DWORD_PTR *) override;
  STDMETHODIMP Unadvise(DWORD_PTR) override;
};
} // namespace nss::camera::detail
