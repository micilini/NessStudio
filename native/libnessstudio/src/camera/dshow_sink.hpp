#pragma once
#include "dshow_types.hpp"
#include <mutex>

namespace nss::camera::detail {
struct SinkReceiver {
  virtual HRESULT receive(IMediaSample *) noexcept=0;
  virtual HRESULT run(REFERENCE_TIME) noexcept=0;
  virtual void flush() noexcept=0;
  virtual void end_stream() noexcept=0;
  virtual ~SinkReceiver()=default;
};
// Original, explicitly connected terminal filter. No third-party base classes,
// graph auto-rendering, video renderer, Sample Grabber or hidden converter.
class Sink final : public IBaseFilter,public IPin,public IMemInputPin {
  std::atomic<ULONG> refs_{1};
  mutable std::mutex gate_;
  MediaType type_;
  VideoFormat format_;
  std::shared_ptr<SinkReceiver> receiver_;
  FILTER_STATE state_{State_Stopped};
  ComPtr<IReferenceClock> clock_;
  ComPtr<IPin> connected_;
  ComPtr<IMemAllocator> allocator_;
  IFilterGraph *graph_{}; // Graph owns filters; deliberately not a COM cycle.
  wchar_t name_[128]{L"NessStudio camera receiver"};
  bool flushing_{};
public:
  Sink(const AM_MEDIA_TYPE &,std::shared_ptr<SinkReceiver>);
  STDMETHODIMP QueryInterface(REFIID,void **) override;
  STDMETHODIMP_(ULONG) AddRef() override {return ++refs_;}
  STDMETHODIMP_(ULONG) Release() override {const auto count=--refs_;if(!count) delete this;return count;}
  STDMETHODIMP GetClassID(CLSID *) override;
  STDMETHODIMP Stop() override;
  STDMETHODIMP Pause() override;
  STDMETHODIMP Run(REFERENCE_TIME) override;
  STDMETHODIMP GetState(DWORD,FILTER_STATE *) override;
  STDMETHODIMP SetSyncSource(IReferenceClock *) override;
  STDMETHODIMP GetSyncSource(IReferenceClock **) override;
  STDMETHODIMP EnumPins(IEnumPins **) override;
  STDMETHODIMP FindPin(LPCWSTR,IPin **) override;
  STDMETHODIMP QueryFilterInfo(FILTER_INFO *) override;
  STDMETHODIMP JoinFilterGraph(IFilterGraph *,LPCWSTR) override;
  STDMETHODIMP QueryVendorInfo(LPWSTR *) override;
  STDMETHODIMP Connect(IPin *,const AM_MEDIA_TYPE *) override;
  STDMETHODIMP ReceiveConnection(IPin *,const AM_MEDIA_TYPE *) override;
  STDMETHODIMP Disconnect() override;
  STDMETHODIMP ConnectedTo(IPin **) override;
  STDMETHODIMP ConnectionMediaType(AM_MEDIA_TYPE *) override;
  STDMETHODIMP QueryPinInfo(PIN_INFO *) override;
  STDMETHODIMP QueryDirection(PIN_DIRECTION *) override;
  STDMETHODIMP QueryId(LPWSTR *) override;
  STDMETHODIMP QueryAccept(const AM_MEDIA_TYPE *) override;
  STDMETHODIMP EnumMediaTypes(IEnumMediaTypes **) override;
  STDMETHODIMP QueryInternalConnections(IPin **,ULONG *) override;
  STDMETHODIMP EndOfStream() override;
  STDMETHODIMP BeginFlush() override;
  STDMETHODIMP EndFlush() override;
  STDMETHODIMP NewSegment(REFERENCE_TIME,REFERENCE_TIME,double) override;
  STDMETHODIMP GetAllocator(IMemAllocator **) override;
  STDMETHODIMP NotifyAllocator(IMemAllocator *,BOOL) override;
  STDMETHODIMP GetAllocatorRequirements(ALLOCATOR_PROPERTIES *) override;
  STDMETHODIMP Receive(IMediaSample *) override;
  STDMETHODIMP ReceiveMultiple(IMediaSample **,long,long *) override;
  STDMETHODIMP ReceiveCanBlock() override {return S_FALSE;}
};
} // namespace nss::camera::detail
