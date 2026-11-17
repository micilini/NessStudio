#include "dshow_clock.hpp"
#include <limits>

namespace nss::camera::detail {
QpcClock::QpcClock() {
  (void)qpc_hns();
  changed_=CreateEventW(nullptr,FALSE,FALSE,nullptr);
  timer_=CreateWaitableTimerExW(nullptr,nullptr,CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,TIMER_ALL_ACCESS);
  if(!changed_ || !timer_) {
    const auto error=HRESULT_FROM_WIN32(GetLastError());
    if(changed_) CloseHandle(changed_);
    if(timer_) CloseHandle(timer_);
    checked(error,"DirectShow reference clock wait objects are unavailable");
  }
  try {worker_=std::thread([this] {run();});}
  catch(...) {CloseHandle(timer_);CloseHandle(changed_);throw;}
}
QpcClock::~QpcClock() {
  {std::lock_guard lock(gate_);stopping_=true;SetEvent(changed_);}
  if(worker_.joinable()) worker_.join();
  CancelWaitableTimer(timer_);CloseHandle(timer_);CloseHandle(changed_);
}
STDMETHODIMP QpcClock::QueryInterface(REFIID iid,void **out) {
  if(!out) return E_POINTER;
  *out=nullptr;
  if(iid!=IID_IUnknown && iid!=IID_IReferenceClock) return E_NOINTERFACE;
  *out=static_cast<IReferenceClock *>(this);AddRef();return S_OK;
}
STDMETHODIMP QpcClock::GetTime(REFERENCE_TIME *out) {
  if(!out) return E_POINTER;
  const auto error=failure_.load();if(FAILED(error)) return error;
  try {
    const auto now=qpc_hns();auto last=last_time_.load();
    while(now>last && !last_time_.compare_exchange_weak(last,now)) {}
    *out=std::max(now,last);return now>last?S_OK:S_FALSE;
  }catch(...) {return E_FAIL;}
}
HRESULT QpcClock::add(int64_t at,int64_t period,HANDLE handle,DWORD_PTR *cookie) noexcept {
  if(!cookie) return E_POINTER;
  *cookie=0;
  if(!handle || handle==INVALID_HANDLE_VALUE || at<=0 || period<0) return E_INVALIDARG;
  try {
    std::lock_guard lock(gate_);
    if(stopping_) return VFW_E_WRONG_STATE;
    if(FAILED(failure_)) return failure_;
    if(!serial_) return E_OUTOFMEMORY;
    for(auto &entry:advice_) if(!entry) {
      const auto key=serial_++;
      entry=Advice{key,at,period,handle};*cookie=key;SetEvent(changed_);return S_OK;
    }
    return E_OUTOFMEMORY;
  } catch(...) {return E_FAIL;}
}
STDMETHODIMP QpcClock::AdviseTime(REFERENCE_TIME base,REFERENCE_TIME stream,HEVENT event,DWORD_PTR *cookie) {
  if((stream>0 && base>std::numeric_limits<int64_t>::max()-stream) ||
     (stream<0 && base<std::numeric_limits<int64_t>::min()-stream)) return E_INVALIDARG;
  return add(base+stream,0,reinterpret_cast<HANDLE>(event),cookie);
}
STDMETHODIMP QpcClock::AdvisePeriodic(REFERENCE_TIME start,REFERENCE_TIME period,HSEMAPHORE semaphore,DWORD_PTR *cookie) {
  if(period<=0) return E_INVALIDARG;
  return add(start,period,reinterpret_cast<HANDLE>(semaphore),cookie);
}
STDMETHODIMP QpcClock::Unadvise(DWORD_PTR cookie) {
  std::lock_guard lock(gate_);
  for(auto &entry:advice_) if(entry && entry->cookie==cookie) {
    // Dispatch also holds this lock. After return no signal can use the
    // caller's event/semaphore, so its handle may be closed immediately.
    entry.reset();SetEvent(changed_);return S_OK;
  }
  return S_FALSE;
}
void QpcClock::run() noexcept {
  try {
    for(;;) {
      std::optional<int64_t> next;
      {
        std::lock_guard lock(gate_);
        if(stopping_) return;
        const auto now=qpc_hns();
        for(auto &entry:advice_) {
          if(!entry) continue;
          if(entry->at<=now) {
            if(entry->period) {
              const auto missed=1+(now-entry->at)/entry->period;
              if(missed>LONG_MAX || missed>(std::numeric_limits<int64_t>::max()-entry->at)/entry->period) {
                failure_=E_INVALIDARG;entry.reset();continue;
              }
              if(!ReleaseSemaphore(entry->handle,static_cast<LONG>(missed),nullptr)) {
                failure_=HRESULT_FROM_WIN32(GetLastError());entry.reset();continue;
              }
              entry->at+=missed*entry->period;
            } else {
              if(!SetEvent(entry->handle)) failure_=HRESULT_FROM_WIN32(GetLastError());
              entry.reset();continue;
            }
          }
          if(!next || entry->at<*next) next=entry->at;
        }
      }
      HANDLE handles[2]{changed_,timer_};DWORD count=1;
      if(next) {
        LARGE_INTEGER due{};due.QuadPart=-std::max<int64_t>(1,*next-qpc_hns());
        if(!SetWaitableTimerEx(timer_,&due,0,nullptr,nullptr,nullptr,0)) {
          failure_=HRESULT_FROM_WIN32(GetLastError());return;
        }
        count=2;
      }
      const auto status=WaitForMultipleObjects(count,handles,FALSE,INFINITE);
      if(status!=WAIT_OBJECT_0 && status!=WAIT_OBJECT_0+1) {
        failure_=HRESULT_FROM_WIN32(GetLastError());return;
      }
    }
  } catch(...) {failure_=E_FAIL;}
}
} // namespace nss::camera::detail
