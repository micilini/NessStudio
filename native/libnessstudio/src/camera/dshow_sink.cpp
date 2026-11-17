#include "dshow_sink.hpp"
#include <cstring>
#include <new>

namespace nss::camera::detail {
namespace {
class Pins final : public IEnumPins {
  std::atomic<ULONG> refs_{1};ComPtr<IPin> pin_;bool consumed_{};
public:
  Pins(IPin *pin,bool consumed=false):pin_(pin),consumed_(consumed) {}
  STDMETHODIMP QueryInterface(REFIID iid,void **out) override {
    if(!out) return E_POINTER;*out=nullptr;
    if(iid!=IID_IUnknown && iid!=IID_IEnumPins) return E_NOINTERFACE;
    *out=static_cast<IEnumPins *>(this);AddRef();return S_OK;
  }
  STDMETHODIMP_(ULONG) AddRef() override {return ++refs_;}
  STDMETHODIMP_(ULONG) Release() override {const auto count=--refs_;if(!count) delete this;return count;}
  STDMETHODIMP Next(ULONG count,IPin **out,ULONG *fetched) override {
    if(fetched) *fetched=0;
    if((count && !out) || (count!=1 && !fetched)) return E_POINTER;
    if(!count) return S_OK;
    out[0]=nullptr;
    if(consumed_) return S_FALSE;
    *out=pin_.Get();(*out)->AddRef();consumed_=true;if(fetched) *fetched=1;
    return count==1?S_OK:S_FALSE;
  }
  STDMETHODIMP Skip(ULONG count) override {
    if(!count) return S_OK;
    const bool present=!consumed_;consumed_=true;return present && count==1?S_OK:S_FALSE;
  }
  STDMETHODIMP Reset() override {consumed_=false;return S_OK;}
  STDMETHODIMP Clone(IEnumPins **out) override {
    if(!out) return E_POINTER;*out=new(std::nothrow) Pins(pin_.Get(),consumed_);return *out?S_OK:E_OUTOFMEMORY;
  }
};
class Types final : public IEnumMediaTypes {
  std::atomic<ULONG> refs_{1};MediaType type_;bool consumed_{};
public:
  Types(const AM_MEDIA_TYPE &type,bool consumed=false):type_(type),consumed_(consumed) {}
  STDMETHODIMP QueryInterface(REFIID iid,void **out) override {
    if(!out) return E_POINTER;*out=nullptr;
    if(iid!=IID_IUnknown && iid!=IID_IEnumMediaTypes) return E_NOINTERFACE;
    *out=static_cast<IEnumMediaTypes *>(this);AddRef();return S_OK;
  }
  STDMETHODIMP_(ULONG) AddRef() override {return ++refs_;}
  STDMETHODIMP_(ULONG) Release() override {const auto count=--refs_;if(!count) delete this;return count;}
  STDMETHODIMP Next(ULONG count,AM_MEDIA_TYPE **out,ULONG *fetched) override {
    if(fetched) *fetched=0;
    if((count && !out) || (count!=1 && !fetched)) return E_POINTER;
    if(!count) return S_OK;
    out[0]=nullptr;
    if(consumed_) return S_FALSE;
    auto copy=static_cast<AM_MEDIA_TYPE *>(CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE)));
    if(!copy) return E_OUTOFMEMORY;
    const auto result=type_.copy_to(copy);
    if(FAILED(result)) {CoTaskMemFree(copy);return result;}
    *out=copy;consumed_=true;if(fetched) *fetched=1;return count==1?S_OK:S_FALSE;
  }
  STDMETHODIMP Skip(ULONG count) override {
    if(!count) return S_OK;
    const bool present=!consumed_;consumed_=true;return present && count==1?S_OK:S_FALSE;
  }
  STDMETHODIMP Reset() override {consumed_=false;return S_OK;}
  STDMETHODIMP Clone(IEnumMediaTypes **out) override {
    if(!out) return E_POINTER;*out=nullptr;
    try {*out=new Types(type_.value,consumed_);return S_OK;}catch(...) {return E_OUTOFMEMORY;}
  }
};
}
Sink::Sink(const AM_MEDIA_TYPE &type,std::shared_ptr<SinkReceiver> receiver)
  :type_(type),format_(video_format(type)),receiver_(std::move(receiver)) {}
STDMETHODIMP Sink::QueryInterface(REFIID iid,void **out) {
  if(!out) return E_POINTER;*out=nullptr;
  if(iid==IID_IUnknown || iid==IID_IBaseFilter || iid==IID_IMediaFilter || iid==IID_IPersist)
    *out=static_cast<IBaseFilter *>(this);
  else if(iid==IID_IPin) *out=static_cast<IPin *>(this);
  else if(iid==IID_IMemInputPin) *out=static_cast<IMemInputPin *>(this);
  else return E_NOINTERFACE;
  AddRef();return S_OK;
}
STDMETHODIMP Sink::GetClassID(CLSID *out) {
  if(!out) return E_POINTER;
  *out={0x87da1b5a,0x8345,0x45d6,{0x9e,0x70,0xa8,0x65,0xf4,0x62,0x12,0xab}};return S_OK;
}
STDMETHODIMP Sink::Stop() {
  std::lock_guard lock(gate_);state_=State_Stopped;flushing_=false;receiver_->flush();return S_OK;
}
STDMETHODIMP Sink::Pause() {std::lock_guard lock(gate_);state_=State_Paused;return S_OK;}
STDMETHODIMP Sink::Run(REFERENCE_TIME start) {
  std::lock_guard lock(gate_);
  const auto result=receiver_->run(start);if(FAILED(result)) return result;
  flushing_=false;state_=State_Running;return S_OK;
}
STDMETHODIMP Sink::GetState(DWORD,FILTER_STATE *out) {
  if(!out) return E_POINTER;std::lock_guard lock(gate_);*out=state_;
  return state_==State_Paused?VFW_S_CANT_CUE:S_OK;
}
STDMETHODIMP Sink::SetSyncSource(IReferenceClock *clock) {std::lock_guard lock(gate_);clock_=clock;return S_OK;}
STDMETHODIMP Sink::GetSyncSource(IReferenceClock **out) {
  if(!out) return E_POINTER;std::lock_guard lock(gate_);*out=clock_.Get();if(*out) (*out)->AddRef();return S_OK;
}
STDMETHODIMP Sink::EnumPins(IEnumPins **out) {
  if(!out) return E_POINTER;*out=new(std::nothrow) Pins(this);return *out?S_OK:E_OUTOFMEMORY;
}
STDMETHODIMP Sink::FindPin(LPCWSTR id,IPin **out) {
  if(!out) return E_POINTER;*out=nullptr;if(!id) return E_POINTER;
  if(wcscmp(id,L"CameraInput")) return VFW_E_NOT_FOUND;
  *out=static_cast<IPin *>(this);AddRef();return S_OK;
}
STDMETHODIMP Sink::QueryFilterInfo(FILTER_INFO *out) {
  if(!out) return E_POINTER;std::lock_guard lock(gate_);*out={};
  wcscpy_s(out->achName,name_);out->pGraph=graph_;if(graph_) graph_->AddRef();return S_OK;
}
STDMETHODIMP Sink::JoinFilterGraph(IFilterGraph *graph,LPCWSTR name) {
  std::lock_guard lock(gate_);graph_=graph;wcsncpy_s(name_,name?name:L"NessStudio camera receiver",_TRUNCATE);return S_OK;
}
STDMETHODIMP Sink::QueryVendorInfo(LPWSTR *out) {if(!out) return E_POINTER;*out=nullptr;return E_NOTIMPL;}
STDMETHODIMP Sink::Connect(IPin *,const AM_MEDIA_TYPE *) {return E_UNEXPECTED;}
STDMETHODIMP Sink::ReceiveConnection(IPin *pin,const AM_MEDIA_TYPE *type) {
  if(!pin || !type) return E_POINTER;
  PIN_DIRECTION direction{};const auto direction_result=pin->QueryDirection(&direction);
  if(FAILED(direction_result)) return direction_result;
  if(direction!=PINDIR_OUTPUT) return VFW_E_INVALID_DIRECTION;
  if(QueryAccept(type)!=S_OK) return VFW_E_TYPE_NOT_ACCEPTED;
  std::lock_guard lock(gate_);
  if(state_!=State_Stopped) return VFW_E_NOT_STOPPED;
  if(connected_) return VFW_E_ALREADY_CONNECTED;
  try {MediaType copy(*type);type_=std::move(copy);connected_=pin;return S_OK;}
  catch(...) {return E_OUTOFMEMORY;}
}
STDMETHODIMP Sink::Disconnect() {
  std::lock_guard lock(gate_);if(state_!=State_Stopped) return VFW_E_NOT_STOPPED;
  if(!connected_) return S_FALSE;connected_.Reset();allocator_.Reset();return S_OK;
}
STDMETHODIMP Sink::ConnectedTo(IPin **out) {
  if(!out) return E_POINTER;std::lock_guard lock(gate_);*out=connected_.Get();
  if(!*out) return VFW_E_NOT_CONNECTED;(*out)->AddRef();return S_OK;
}
STDMETHODIMP Sink::ConnectionMediaType(AM_MEDIA_TYPE *out) {
  if(!out) return E_POINTER;std::lock_guard lock(gate_);*out={};
  return connected_?type_.copy_to(out):VFW_E_NOT_CONNECTED;
}
STDMETHODIMP Sink::QueryPinInfo(PIN_INFO *out) {
  if(!out) return E_POINTER;*out={};out->pFilter=this;AddRef();out->dir=PINDIR_INPUT;
  wcscpy_s(out->achName,L"CameraInput");return S_OK;
}
STDMETHODIMP Sink::QueryDirection(PIN_DIRECTION *out) {if(!out) return E_POINTER;*out=PINDIR_INPUT;return S_OK;}
STDMETHODIMP Sink::QueryId(LPWSTR *out) {
  if(!out) return E_POINTER;constexpr wchar_t id[]=L"CameraInput";
  *out=static_cast<wchar_t *>(CoTaskMemAlloc(sizeof(id)));if(!*out) return E_OUTOFMEMORY;
  std::memcpy(*out,id,sizeof(id));return S_OK;
}
STDMETHODIMP Sink::QueryAccept(const AM_MEDIA_TYPE *type) {
  if(!type) return E_POINTER;
  try {const auto actual=video_format(*type);return same_format(format_,actual) &&
    actual.maximum_bytes<=format_.maximum_bytes?S_OK:S_FALSE;}catch(...) {return S_FALSE;}
}
STDMETHODIMP Sink::EnumMediaTypes(IEnumMediaTypes **out) {
  if(!out) return E_POINTER;*out=nullptr;std::lock_guard lock(gate_);
  try {*out=new Types(type_.value);return S_OK;}catch(...) {return E_OUTOFMEMORY;}
}
STDMETHODIMP Sink::QueryInternalConnections(IPin **,ULONG *count) {if(!count) return E_POINTER;*count=0;return S_OK;}
STDMETHODIMP Sink::EndOfStream() {receiver_->end_stream();return S_OK;}
STDMETHODIMP Sink::BeginFlush() {std::lock_guard lock(gate_);flushing_=true;receiver_->flush();return S_OK;}
STDMETHODIMP Sink::EndFlush() {std::lock_guard lock(gate_);flushing_=false;return S_OK;}
STDMETHODIMP Sink::NewSegment(REFERENCE_TIME start,REFERENCE_TIME,double rate) {
  // This is a live source; seeking/rate changes would invalidate acquisition
  // timestamps. Pausing the recording never pauses or seeks this graph.
  return start==0 && rate==1.0?S_OK:E_INVALIDARG;
}
STDMETHODIMP Sink::GetAllocator(IMemAllocator **out) {
  if(!out) return E_POINTER;*out=nullptr;std::lock_guard lock(gate_);
  if(!allocator_) {
    const auto result=CoCreateInstance(CLSID_MemoryAllocator,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&allocator_));
    if(FAILED(result)) return result;
  }
  *out=allocator_.Get();(*out)->AddRef();return S_OK;
}
STDMETHODIMP Sink::NotifyAllocator(IMemAllocator *allocator,BOOL) {
  if(!allocator) return E_POINTER;
  ALLOCATOR_PROPERTIES props{};const auto result=allocator->GetProperties(&props);if(FAILED(result)) return result;
  if(props.cbBuffer<static_cast<LONG>(format_.minimum_bytes) || props.cBuffers<=0 || props.cBuffers>64 ||
    uint64_t(props.cbBuffer)*props.cBuffers>128ULL*1024*1024) return VFW_E_SIZENOTSET;
  std::lock_guard lock(gate_);allocator_=allocator;return S_OK;
}
STDMETHODIMP Sink::GetAllocatorRequirements(ALLOCATOR_PROPERTIES *out) {
  if(!out) return E_POINTER;*out={4,static_cast<LONG>(format_.maximum_bytes),1,0};return S_OK;
}
STDMETHODIMP Sink::Receive(IMediaSample *sample) {
  if(!sample) return E_POINTER;
  {
    std::lock_guard lock(gate_);
    if(state_==State_Stopped) return VFW_E_WRONG_STATE;
    if(flushing_) return S_FALSE;
    if(state_==State_Paused) return S_OK;
  }
  return receiver_->receive(sample);
}
STDMETHODIMP Sink::ReceiveMultiple(IMediaSample **samples,long count,long *processed) {
  if(!processed) return E_POINTER;*processed=0;
  if(count<0 || (count && !samples)) return E_INVALIDARG;
  for(long i=0;i<count;++i) {const auto result=Receive(samples[i]);if(result!=S_OK) return result;++*processed;}
  return S_OK;
}
} // namespace nss::camera::detail
