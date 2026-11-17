#include "dshow_source.hpp"
#include "dshow_clock.hpp"
#include "dshow_controls.hpp"
#include <ks.h>
#include <ksmedia.h>
#include <ksproxy.h>
#include <future>
#include <cstring>
#include <map>
#include <set>

namespace nss::camera {
namespace {
using namespace detail;
struct Device {ComPtr<IMoniker> moniker;std::string id,name;};
std::string property(IPropertyBag *bag,const wchar_t *name) {
  VARIANT value{};VariantInit(&value);
  const auto status=bag->Read(name,&value,nullptr);
  std::string result;
  try {if(SUCCEEDED(status) && value.vt==VT_BSTR && value.bstrVal) result=utf8({value.bstrVal,SysStringLen(value.bstrVal)});}
  catch(...) {VariantClear(&value);throw;}
  VariantClear(&value);return result;
}
std::vector<Device> enumerate() {
  ComPtr<ICreateDevEnum> devices;
  checked(CoCreateInstance(CLSID_SystemDeviceEnum,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&devices)),"DirectShow device enumerator unavailable");
  ComPtr<IEnumMoniker> enumeration;
  const auto status=devices->CreateClassEnumerator(CLSID_VideoInputDeviceCategory,&enumeration,0);
  checked(status,"DirectShow camera enumeration failed");
  std::vector<Device> result;if(status==S_FALSE) return result;
  for(;;) {
    ComPtr<IMoniker> moniker;
    const auto next=enumeration->Next(1,&moniker,nullptr);if(next==S_FALSE) break;
    checked(next,"DirectShow camera enumeration failed");
    if(result.size()>=32) throw std::length_error("DirectShow camera enumeration exceeds its budget");
    ComPtr<IPropertyBag> bag;
    checked(moniker->BindToStorage(nullptr,nullptr,IID_PPV_ARGS(&bag)),"Camera device identity is unavailable");
    auto id=property(bag.Get(),L"DevicePath");
    if(id.empty()) {
      LPOLESTR name{};checked(moniker->GetDisplayName(nullptr,nullptr,&name),"Camera moniker identity is unavailable");
      try {id=utf8(name);}catch(...) {CoTaskMemFree(name);throw;}
      CoTaskMemFree(name);
    }
    result.push_back({moniker,id,property(bag.Get(),L"FriendlyName")});
  }
  return result;
}
std::mutex inventory_gate;
std::set<std::string> active;
std::map<std::string,Json> known_formats;
struct Reservation {
  std::string id;
  explicit Reservation(std::string identity):id(std::move(identity)) {
    std::lock_guard lock(inventory_gate);
    for(const auto &owner:active) if(same_device(owner,id))
      throw capture::Error(NSS_E_BUSY,NSS_STAGE_CAPTURE,0,"Camera already belongs to another native session");
    active.insert(id);
  }
  ~Reservation() {std::lock_guard lock(inventory_gate);active.erase(id);}
};
struct CapturePin {ComPtr<IPin> pin;ComPtr<IAMStreamConfig> config;};
CapturePin capture_pin(IBaseFilter *source) {
  ComPtr<IEnumPins> enumeration;checked(source->EnumPins(&enumeration),"Camera pins unavailable");
  CapturePin fallback;
  for(unsigned index=0;index<64;++index) {
    ComPtr<IPin> pin;const auto next=enumeration->Next(1,&pin,nullptr);if(next==S_FALSE) break;
    checked(next,"Camera pin enumeration failed");
    PIN_DIRECTION direction{};checked(pin->QueryDirection(&direction),"Camera pin direction unavailable");
    if(direction!=PINDIR_OUTPUT) continue;
    ComPtr<IAMStreamConfig> config;if(FAILED(pin.As(&config))) continue;
    ComPtr<IKsPropertySet> properties;GUID category{};DWORD returned{};
    if(SUCCEEDED(pin.As(&properties)) && SUCCEEDED(properties->Get(AMPROPSETID_Pin,AMPROPERTY_PIN_CATEGORY,nullptr,0,
      &category,sizeof(category),&returned)) && returned==sizeof(category)) {
      if(category==PIN_CATEGORY_CAPTURE) return {pin,config};
      // A documented preview/still pin is not interchangeable with capture.
      continue;
    }
    if(!fallback.pin) fallback={pin,config};
  }
  if(!fallback.pin) throw capture::Error(NSS_E_UNAVAILABLE,NSS_STAGE_CAPTURE,VFW_E_NOT_FOUND,"Camera exposes no configurable video capture pin");
  return fallback;
}
struct Capability {MediaType type;VideoFormat format;VIDEO_STREAM_CONFIG_CAPS limits{};uint32_t index{};};
std::vector<Capability> capabilities(IAMStreamConfig *config) {
  int count{},size{};checked(config->GetNumberOfCapabilities(&count,&size),"Camera format capabilities unavailable");
  if(count<0 || count>512 || size<static_cast<int>(sizeof(VIDEO_STREAM_CONFIG_CAPS)) || size>65536)
    throw capture::Error(NSS_E_UNAVAILABLE,NSS_STAGE_CAPTURE,VFW_E_INVALIDMEDIATYPE,"Camera capability structure exceeds its budget");
  std::vector<BYTE> bytes(static_cast<size_t>(size));std::vector<Capability> result;
  for(int index=0;index<count;++index) {
    AM_MEDIA_TYPE *raw{};const auto status=config->GetStreamCaps(index,&raw,bytes.data());
    if(FAILED(status)) {free_type(raw);checked(status,"Camera native format enumeration failed");}
    if(!raw) continue;
    MediaType type;type.value=*raw;CoTaskMemFree(raw);
    try {
      const auto format=video_format(type.value);VIDEO_STREAM_CONFIG_CAPS limits{};
      std::memcpy(&limits,bytes.data(),sizeof(limits));
      result.push_back({std::move(type),format,limits,static_cast<uint32_t>(index)});
    } catch(const capture::Error &e) {
      if(e.code!=NSS_E_UNAVAILABLE) throw;
    }
  }
  return result;
}
MediaType select_format(const Plan &plan,CapturePin &pin) {
  const auto requested=static_cast<int64_t>(10000000ULL*plan.fps_den/plan.fps_num);
  for(auto &cap:capabilities(pin.config.Get())) {
    const auto &f=cap.format;
    if(f.width!=plan.width || f.height!=plan.height || plan.subtype!=subtype_name(f.pixels)) continue;
    const bool advertised=std::abs(f.interval-requested)<=1 ||
      (cap.limits.MinFrameInterval>0 && cap.limits.MaxFrameInterval>=cap.limits.MinFrameInterval &&
       requested>=cap.limits.MinFrameInterval-1 && requested<=cap.limits.MaxFrameInterval+1);
    if(!advertised) continue;
    set_interval(cap.type.value,requested);
    checked(pin.config->SetFormat(&cap.type.value),"Explicit DirectShow camera format negotiation failed");
    AM_MEDIA_TYPE *raw{};const auto negotiated=pin.config->GetFormat(&raw);
    if(FAILED(negotiated)) {free_type(raw);checked(negotiated,"Camera negotiated format unavailable");}
    if(!raw) throw capture::Error(NSS_E_IO,NSS_STAGE_CAPTURE,E_POINTER,"Camera returned a null negotiated media type");
    MediaType actual;actual.value=*raw;CoTaskMemFree(raw);
    const auto selected=video_format(actual.value);
    if(selected.width!=plan.width || selected.height!=plan.height || plan.subtype!=subtype_name(selected.pixels) ||
       std::abs(selected.interval-requested)>1)
      throw capture::Error(NSS_E_UNAVAILABLE,NSS_STAGE_CAPTURE,VFW_E_INVALIDMEDIATYPE,"Camera silently changed requested geometry, format or cadence");
    return actual;
  }
  throw capture::Error(NSS_E_UNAVAILABLE,NSS_STAGE_CAPTURE,VFW_E_INVALIDMEDIATYPE,"The requested DirectShow camera format/cadence is not advertised");
}
ComPtr<IBaseFilter> open_device(const Device &device) {
  ComPtr<IBaseFilter> source;
  checked(device.moniker->BindToObject(nullptr,nullptr,IID_PPV_ARGS(&source)),"DirectShow camera activation failed");return source;
}
}
Json devices() {
  return std::async(std::launch::async,[] {
    detail::ComRuntime runtime;Json result=Json::array();
    for(const auto &device:enumerate()) {
      Json formats=Json::array(),controls=Json::object();bool busy{};HRESULT error=S_OK;
      std::unique_ptr<Reservation> owner;
      try {owner=std::make_unique<Reservation>(device.id);}
      catch(const capture::Error &failure) {if(failure.code!=NSS_E_BUSY) throw;busy=true;}
      if(busy) {
        std::lock_guard lock(inventory_gate);
        const auto known=known_formats.find(device.id);if(known!=known_formats.end()) formats=known->second;
      } else {
        try {
          const auto source=open_device(device);auto pin=capture_pin(source.Get());
          controls=detail::Controls(source.Get()).inventory();
          for(const auto &cap:capabilities(pin.config.Get())) {
            auto item=detail::format_json(cap.format,cap.index);
            item["minimumFrameIntervalHns"]=cap.limits.MinFrameInterval;
            item["maximumFrameIntervalHns"]=cap.limits.MaxFrameInterval;
            formats.push_back(std::move(item));
          }
          std::lock_guard lock(inventory_gate);known_formats[device.id]=formats;
        } catch(const capture::Error &failure) {error=failure.native?failure.native:E_FAIL;}
      }
      result.push_back({{"id",device.id},{"aliases",detail::identity_aliases(device.id)},
        {"name",device.name},{"kind",NSS_SOURCE_CAMERA},{"formats",formats},{"controls",controls},
        {"nativeSessionOwnsDevice",busy},{"formatEnumerationHresult",error},{"default",result.empty()}});
    }
    return Json{{"version",1},{"kind",NSS_SOURCE_CAMERA},{"devices",result},{"acceleratedDeviceCreated",false},
      {"captureApi","DirectShow; explicitly connected native capture pin"},
      {"timestampModes",Json::array({"DeliveryQpc","StrictStreamQpc"})},{"defaultTimestampMode","DeliveryQpc"}};
  }).get();
}
struct Source::Impl {
  detail::ComRuntime runtime;
  Reservation reservation;
  std::shared_ptr<detail::SampleReceiver> receiver;
  detail::ComPtr<IGraphBuilder> graph;
  detail::ComPtr<IBaseFilter> source;
  CapturePin capture;
  detail::ComPtr<detail::Sink> sink;
  detail::ComPtr<detail::QpcClock> clock;
  detail::ComPtr<IMediaControl> control;
  detail::ComPtr<IMediaEvent> events;
  std::unique_ptr<detail::Controls> controls;
  Json controls_final=Json::object();
  bool stopped{},counted{},streaming_controls_applied{};
  HRESULT close_error{S_OK};
  explicit Impl(const Plan &request):reservation(request.device_id) {
    try {
      const auto list=enumerate();
      const auto selected=std::find_if(list.begin(),list.end(),[&](const auto &d) {return detail::same_device(d.id,request.device_id);});
      if(selected==list.end()) throw capture::Error(NSS_E_UNAVAILABLE,NSS_STAGE_CAPTURE,0,"Requested camera is disconnected or unavailable");
      source=open_device(*selected);capture=capture_pin(source.Get());
      auto format=select_format(request,capture);
      controls=std::make_unique<detail::Controls>(source.Get());controls->apply(request.controls);
      receiver=std::make_shared<detail::SampleReceiver>(request,detail::video_format(format.value));
      sink.Attach(new detail::Sink(format.value,receiver));
      clock.Attach(new detail::QpcClock());
      detail::checked(CoCreateInstance(CLSID_FilterGraph,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&graph)),"Camera DirectShow graph creation failed");
      detail::checked(graph->AddFilter(source.Get(),L"NessStudio camera source"),"Camera source could not join its graph");
      detail::checked(graph->AddFilter(sink.Get(),L"NessStudio camera receiver"),"Camera receiver could not join its graph");
      detail::checked(graph->ConnectDirect(capture.pin.Get(),static_cast<IPin *>(sink.Get()),&format.value),"Camera direct connection rejected the selected format");
      detail::ComPtr<IMediaFilter> filter;detail::checked(graph.As(&filter),"Camera graph timing interface unavailable");
      detail::checked(filter->SetSyncSource(clock.Get()),"Camera graph rejected the QPC reference clock");
      detail::checked(graph.As(&control),"Camera graph control unavailable");
      detail::checked(graph.As(&events),"Camera graph event interface unavailable");
      // Driver callbacks may release their final COM reference after Stop.
      // Keep this module's receiver code valid even if the client unloads it.
      static const wchar_t anchor[]=L"NessStudio.DirectShow.Callback.Code";HMODULE module{};
      if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,anchor,&module))
        detail::checked(HRESULT_FROM_WIN32(GetLastError()),"Cannot retain DirectShow callback code");
      ++live_sources;counted=true;
      detail::checked(control->Run(),"Camera graph failed to run");
    } catch(...) {close();throw;}
  }
  void inspect_events() {
    if(!events || stopped) return;
    for(unsigned i=0;i<64;++i) {
      LONG code{};LONG_PTR first{},second{};
      const auto result=events->GetEvent(&code,&first,&second,0);
      if(result==E_ABORT) break;
      if(FAILED(result)) {receiver->fail(result,"Camera event queue failed");break;}
      if(code==EC_ERRORABORT) receiver->fail(static_cast<HRESULT>(first),"Camera driver reported a streaming error");
      else if(code==EC_DEVICE_LOST && second==0)
        receiver->fail(HRESULT_FROM_WIN32(ERROR_DEVICE_NOT_CONNECTED),"Mandatory camera device disconnected");
      else if(code==EC_COMPLETE) receiver->end_stream();
      const auto release=events->FreeEventParams(code,first,second);
      if(FAILED(release)) {receiver->fail(release,"Camera event cleanup failed");break;}
    }
  }
  void close() noexcept {
    if(stopped) return;stopped=true;
    // Reject borrowed samples and finish our bounded pixel worker first.
    if(receiver) receiver->close();
    auto remember=[&](HRESULT status) {if(FAILED(status) && SUCCEEDED(close_error)) close_error=status;};
    if(control) remember(control->Stop());
    if(controls) {
      remember(controls->restore());
      try {controls_final=controls->metrics();}catch(...) {remember(E_OUTOFMEMORY);}
      controls.reset();
    }
    if(graph) {
      if(capture.pin) remember(graph->Disconnect(capture.pin.Get()));
      if(sink) remember(graph->Disconnect(static_cast<IPin *>(sink.Get())));
      if(sink) remember(graph->RemoveFilter(sink.Get()));
      if(source) remember(graph->RemoveFilter(source.Get()));
      detail::ComPtr<IMediaFilter> filter;
      if(SUCCEEDED(graph.As(&filter))) remember(filter->SetSyncSource(nullptr));
    }
    events.Reset();control.Reset();graph.Reset();capture={};sink.Reset();source.Reset();clock.Reset();
    if(counted) {--live_sources;counted=false;}
  }
  ~Impl() {close();}
};
Source::Source(const Plan &plan):impl_(std::make_unique<Impl>(plan)) {}
Source::~Source()=default;
void Source::close() {impl_->close();detail::checked(impl_->close_error,"DirectShow camera Stop/disconnect did not complete successfully");}
std::optional<capture::Frame> Source::poll() {
  impl_->inspect_events();
  auto frame=impl_->receiver->poll();
  if(frame && !impl_->streaming_controls_applied) {
    impl_->controls->apply_streaming();
    impl_->streaming_controls_applied=true;
  }
  return frame;
}
Json Source::metrics() const {
  auto result=impl_->receiver->metrics();result["shutdownHresult"]=impl_->close_error;
  result["controls"]=impl_->controls?impl_->controls->metrics():impl_->controls_final;
  result["captureApi"]="DirectShow native receiver";return result;
}
void *Source::wait_event() const noexcept {return impl_->receiver->ready_event();}
} // namespace nss::camera
