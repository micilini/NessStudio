#include "camera/dshow_source.hpp"
#include "camera/dshow_clock.hpp"
#include "camera/dshow_controls.hpp"
#include "camera/dshow_timestamp.hpp"
#include <array>
#include <cstring>
#include <limits>
#include <wincodec.h>

namespace nss::camera {
namespace {
using namespace detail;
void check(bool value,const char *message) {if(!value) throw std::runtime_error(message);}
MediaType media(Pixels pixels=Pixels::Bgra,bool bottom_up=false,bool info2=false) {
  MediaType result;auto &type=result.value;type.majortype=MEDIATYPE_Video;
  type.subtype=pixels==Pixels::Bgra?MEDIASUBTYPE_RGB32:pixels==Pixels::Yuy2?MEDIASUBTYPE_YUY2:
    pixels==Pixels::Nv12?MEDIASUBTYPE_NV12:MEDIASUBTYPE_MJPG;
  type.formattype=info2?FORMAT_VideoInfo2:FORMAT_VideoInfo;type.bFixedSizeSamples=TRUE;
  type.cbFormat=info2?sizeof(VIDEOINFOHEADER2):sizeof(VIDEOINFOHEADER);
  type.pbFormat=static_cast<BYTE *>(CoTaskMemAlloc(type.cbFormat));check(type.pbFormat!=nullptr,"Media type allocation failed");
  std::memset(type.pbFormat,0,type.cbFormat);
  auto &bitmap=info2?reinterpret_cast<VIDEOINFOHEADER2 *>(type.pbFormat)->bmiHeader:
    reinterpret_cast<VIDEOINFOHEADER *>(type.pbFormat)->bmiHeader;
  bitmap.biSize=sizeof(bitmap);bitmap.biWidth=2;bitmap.biHeight=bottom_up?2:-2;bitmap.biPlanes=1;
  bitmap.biBitCount=pixels==Pixels::Bgra?32:pixels==Pixels::Yuy2?16:pixels==Pixels::Nv12?12:24;
  bitmap.biCompression=pixels==Pixels::Bgra?BI_RGB:type.subtype.Data1;
  bitmap.biSizeImage=pixels==Pixels::Bgra?16:pixels==Pixels::Yuy2?8:pixels==Pixels::Nv12?6:0;
  if(info2) reinterpret_cast<VIDEOINFOHEADER2 *>(type.pbFormat)->AvgTimePerFrame=333333;
  else reinterpret_cast<VIDEOINFOHEADER *>(type.pbFormat)->AvgTimePerFrame=333333;
  type.lSampleSize=bitmap.biSizeImage;return result;
}
// Preserve the earlier driver-clock contract tests under its explicit mode.
Plan request(TimestampMode mode=TimestampMode::StrictStreamQpc) {
  Plan plan;plan.device_id="qualification-only";plan.subtype="RGB32";
  plan.width=plan.height=2;plan.timestamp_mode=mode;return plan;
}
struct Sample {
  ComPtr<IMemAllocator> allocator;ComPtr<IMediaSample> sample;
  explicit Sample(LONG capacity=64) {
    checked(CoCreateInstance(CLSID_MemoryAllocator,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&allocator)),"Test allocator unavailable");
    ALLOCATOR_PROPERTIES wanted{2,capacity,1,0},actual{};
    checked(allocator->SetProperties(&wanted,&actual),"Test allocator configuration failed");
    checked(allocator->Commit(),"Test allocator commit failed");
    checked(allocator->GetBuffer(&sample,nullptr,nullptr,0),"Test sample allocation failed");
    BYTE *bytes{};checked(sample->GetPointer(&bytes),"Test sample memory unavailable");
    for(LONG i=0;i<capacity;++i) bytes[i]=static_cast<BYTE>(i);
    checked(sample->SetActualDataLength(16),"Test sample length failed");
    checked(sample->SetPreroll(FALSE),"Test sample preroll failed");
    checked(sample->SetDiscontinuity(FALSE),"Test sample continuity failed");
  }
  void timestamp(int64_t begin) {REFERENCE_TIME end=begin+333333;checked(sample->SetTime(&begin,&end),"Test timestamp failed");}
  void payload(std::span<const BYTE> value) {
    check(value.size()<=static_cast<size_t>(sample->GetSize()),"Test payload exceeds allocator");
    BYTE *bytes{};checked(sample->GetPointer(&bytes),"Test payload allocation unavailable");
    std::memcpy(bytes,value.data(),value.size());
    checked(sample->SetActualDataLength(static_cast<LONG>(value.size())),"Test payload size rejected");
  }
  ~Sample() {sample.Reset();if(allocator) allocator->Decommit();}
};
std::vector<BYTE> jpeg_fixture() {
  ComPtr<IWICImagingFactory> factory;
  checked(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory)),"Test JPEG factory unavailable");
  ComPtr<IStream> stream;checked(CreateStreamOnHGlobal(nullptr,TRUE,&stream),"Test JPEG stream unavailable");
  ComPtr<IWICBitmapEncoder> encoder;checked(factory->CreateEncoder(GUID_ContainerFormatJpeg,nullptr,&encoder),"Test JPEG encoder unavailable");
  checked(encoder->Initialize(stream.Get(),WICBitmapEncoderNoCache),"Test JPEG encoder initialization failed");
  ComPtr<IWICBitmapFrameEncode> frame;ComPtr<IPropertyBag2> properties;
  checked(encoder->CreateNewFrame(&frame,&properties),"Test JPEG frame unavailable");
  checked(frame->Initialize(properties.Get()),"Test JPEG frame initialization failed");
  checked(frame->SetSize(2,2),"Test JPEG size rejected");
  auto format=GUID_WICPixelFormat24bppBGR;checked(frame->SetPixelFormat(&format),"Test JPEG format rejected");
  check(format==GUID_WICPixelFormat24bppBGR,"Test JPEG substituted pixels");
  std::array<BYTE,12> pixels{0,0,0,255,255,255,0,0,0,255,255,255};
  checked(frame->WritePixels(2,6,static_cast<UINT>(pixels.size()),pixels.data()),"Test JPEG pixels rejected");
  checked(frame->Commit(),"Test JPEG frame commit failed");checked(encoder->Commit(),"Test JPEG commit failed");
  STATSTG stat{};checked(stream->Stat(&stat,STATFLAG_NONAME),"Test JPEG length unavailable");
  check(stat.cbSize.QuadPart>0 && stat.cbSize.QuadPart<=65536,"Test JPEG length exceeds budget");
  std::vector<BYTE> bytes(static_cast<size_t>(stat.cbSize.QuadPart));
  LARGE_INTEGER zero{};checked(stream->Seek(zero,STREAM_SEEK_SET,nullptr),"Test JPEG rewind failed");
  ULONG read{};checked(stream->Read(bytes.data(),static_cast<ULONG>(bytes.size()),&read),"Test JPEG read failed");
  check(read==bytes.size(),"Test JPEG was truncated");return bytes;
}
std::optional<capture::Frame> await_frame(SampleReceiver &receiver) {
  const auto deadline=GetTickCount64()+1000;
  for(;;) {auto frame=receiver.poll();if(frame) return frame;
    check(GetTickCount64()<deadline,"Camera handoff worker did not deliver its frame");
    WaitForSingleObject(receiver.ready_event(),10);
  }
}
class OutputPin final : public IPin {
  std::atomic<ULONG> refs_{1};
public:
  STDMETHODIMP QueryInterface(REFIID iid,void **out) override {
    if(!out) return E_POINTER;*out=nullptr;if(iid!=IID_IUnknown && iid!=IID_IPin) return E_NOINTERFACE;
    *out=static_cast<IPin *>(this);AddRef();return S_OK;
  }
  STDMETHODIMP_(ULONG) AddRef() override {return ++refs_;}
  STDMETHODIMP_(ULONG) Release() override {const auto count=--refs_;if(!count) delete this;return count;}
  STDMETHODIMP Connect(IPin *,const AM_MEDIA_TYPE *) override {return E_NOTIMPL;}
  STDMETHODIMP ReceiveConnection(IPin *,const AM_MEDIA_TYPE *) override {return E_UNEXPECTED;}
  STDMETHODIMP Disconnect() override {return S_OK;}
  STDMETHODIMP ConnectedTo(IPin **out) override {if(out) *out=nullptr;return VFW_E_NOT_CONNECTED;}
  STDMETHODIMP ConnectionMediaType(AM_MEDIA_TYPE *) override {return VFW_E_NOT_CONNECTED;}
  STDMETHODIMP QueryPinInfo(PIN_INFO *) override {return E_NOTIMPL;}
  STDMETHODIMP QueryDirection(PIN_DIRECTION *out) override {if(!out) return E_POINTER;*out=PINDIR_OUTPUT;return S_OK;}
  STDMETHODIMP QueryId(LPWSTR *) override {return E_NOTIMPL;}
  STDMETHODIMP QueryAccept(const AM_MEDIA_TYPE *) override {return S_OK;}
  STDMETHODIMP EnumMediaTypes(IEnumMediaTypes **) override {return E_NOTIMPL;}
  STDMETHODIMP QueryInternalConnections(IPin **,ULONG *) override {return E_NOTIMPL;}
  STDMETHODIMP EndOfStream() override {return S_OK;}
  STDMETHODIMP BeginFlush() override {return S_OK;}
  STDMETHODIMP EndFlush() override {return S_OK;}
  STDMETHODIMP NewSegment(REFERENCE_TIME,REFERENCE_TIME,double) override {return S_OK;}
};
struct Receiver final : SinkReceiver {
  unsigned samples{},flushes{},ends{};REFERENCE_TIME origin{};
  HRESULT receive(IMediaSample *) noexcept override {++samples;return S_OK;}
  HRESULT run(REFERENCE_TIME start) noexcept override {origin=start;return S_OK;}
  void flush() noexcept override {++flushes;}
  void end_stream() noexcept override {++ends;}
};
}
Json qualify(const std::string &scope) {
  ComRuntime runtime;Json cases=Json::array();
  const auto before_bytes=live_pool_bytes.load(),before_leases=live_frame_leases.load();
  if(scope=="startup-cadence") {
    StartupCadence warmup(30,1);
    check(!warmup.observe(10000000),"One USB frame cannot establish continuing delivery");
    check(!warmup.observe(11120000) && warmup.observe(12400000),
          "Measured C920 automatic-exposure delivery at roughly 8 fps was rejected");
    check(warmup.metrics().at("lastCaptureHns")==12400000,"Warm-up changed the source timestamp");
    check(!warmup.observe(12400000),"A repeated identity/time established another interval");
    check(!warmup.observe(33000000),"A source inactivity gap established readiness");
    check(!warmup.observe(33333333) && warmup.observe(33666666),"Continuing delivery after a real gap did not establish readiness");
    StartupCadence exposure(30,1);
    check(!exposure.observe(10000000) && !exposure.observe(10666667) && exposure.observe(11333334),
          "Valid automatic exposure at half nominal cadence was rejected");
    StartupCadence slow(5,1);
    check(!slow.observe(10000000) && !slow.observe(12000000) && slow.observe(14000000),
          "Advertised low camera cadence was treated as startup instability");
    StartupCadence quarter(30,1);
    check(!quarter.observe(10000000) && !quarter.observe(11250000) && quarter.observe(12500000),
          "Valid automatic 125 ms exposure was rejected");
    cases.push_back({{"sourceInactivityRejected",true},{"exposureAndLowCadencePreserved",true},
                     {"measuredC920EightFpsStartupAccepted",true},{"readinessDoesNotPromiseNominalFps",true}});
  } else if(scope=="faults") {
    for(unsigned kind=0;kind<5;++kind) {
      auto type=media();SampleReceiver receiver(request(),video_format(type.value));Sample sample;
      receiver.run(qpc_hns()-hns_per_second);sample.timestamp(9000000);
      HRESULT expected{};
      if(kind==0) {expected=E_ACCESSDENIED;receiver.fail(expected,"test failure");}
      if(kind==1) {expected=HRESULT_FROM_WIN32(ERROR_DEVICE_NOT_CONNECTED);receiver.end_stream();}
      if(kind==2) {expected=VFW_E_INVALIDMEDIATYPE;auto changed=media(Pixels::Yuy2);sample.sample->SetMediaType(&changed.value);receiver.receive(sample.sample.Get());}
      if(kind==3) {expected=VFW_E_BUFFER_UNDERFLOW;sample.sample->SetActualDataLength(1);receiver.receive(sample.sample.Get());}
      if(kind==4) {receiver.close();check(receiver.receive(sample.sample.Get())==S_FALSE,"Closed receiver accepted a new sample");}
      if(FAILED(expected)) receiver.fail(E_UNEXPECTED,"secondary failure must not replace first");
      check(receiver.metrics().at("sourceHresult")==expected,"Camera first failure was lost");
      receiver.close();cases.push_back({{"case",kind},{"hresult",expected}});
    }
  } else if(scope=="timestamp-policy") {
    auto config=plan_json(request());config.erase("timestampMode");
    check(plan(config).timestamp_mode==TimestampMode::DeliveryQpc,"Camera default still assumes an unproven device clock");
    for(const auto mode:{TimestampMode::DeliveryQpc,TimestampMode::StrictStreamQpc})
      check(plan(plan_json(request(mode))).timestamp_mode==mode,"Camera timing policy round trip changed");
    config["timestampMode"]="InventedClock";
    bool rejected{};try {(void)plan(config);}catch(const capture::Error &) {rejected=true;}
    check(rejected,"Unknown camera clock policy accepted");
    constexpr int64_t origin=1000000000000LL;
    for(const int ppm:{-4000,-2600,0,2600,4000}) {
      int64_t previous_raw{},previous_arrival{};uint64_t strict_rejections{};
      for(uint64_t index=0;index<=8ULL*60*60*30;++index) {
        const auto elapsed=frame_time(index,30,1);
        const auto raw=1+elapsed*(1000000+ppm)/1000000;
        const auto arrival=origin+elapsed+100000+static_cast<int64_t>(index%7)*137;
        const auto selected=camera_timestamp(TimestampMode::DeliveryQpc,origin,raw,arrival,
                                              index!=0,previous_raw,previous_arrival);
        check(selected.capture_qpc==arrival && selected.graph_projection==origin+raw,
              "Device drift contaminated measured delivery QPC or erased raw timing");
        if(index%1800==0) {
          try {(void)camera_timestamp(TimestampMode::StrictStreamQpc,origin,raw,arrival,
                                      index!=0,previous_raw,previous_arrival);}
          catch(const capture::Error &) {++strict_rejections;}
        }
        previous_raw=raw;previous_arrival=arrival;
      }
      check(ppm==0?strict_rejections==0:strict_rejections>0,"Strict clock validation was weakened");
      cases.push_back({{"modeledHours",8},{"deviceRateErrorPpm",ppm},
                       {"strictChecksEveryFrames",1800},{"strictRejections",strict_rejections}});
    }
    const auto first=camera_timestamp(TimestampMode::DeliveryQpc,origin,1,origin+100000,false,0,0);
    const auto stalled=camera_timestamp(TimestampMode::DeliveryQpc,origin,333334,origin+20100000,true,1,origin+100000);
    check(stalled.capture_qpc-first.capture_qpc==2*hns_per_second,"Real delivery gap was retimed away");
    for(const auto mode:{TimestampMode::DeliveryQpc,TimestampMode::StrictStreamQpc}) {
      const auto expect_rejected=[&](int64_t base,int64_t raw,int64_t arrival,bool previous,int64_t last_raw,int64_t last_arrival) {
        bool failed{};try {(void)camera_timestamp(mode,base,raw,arrival,previous,last_raw,last_arrival);}
        catch(const capture::Error &) {failed=true;}
        check(failed,"Malformed camera clock input was accepted");
      };
      expect_rejected(0,1,origin,false,0,0);
      expect_rejected(origin,-1,origin,false,0,0);
      expect_rejected(origin,std::numeric_limits<int64_t>::max(),origin,false,0,0);
      expect_rejected(origin,1,origin+1,true,1,origin);
      expect_rejected(origin,2,origin,true,1,origin);
      expect_rejected(origin,2,origin-1,true,1,origin);
    }
    {
      auto type=media();SampleReceiver receiver(request(TimestampMode::DeliveryQpc),video_format(type.value));Sample sample;
      const auto graph_origin=qpc_hns();checked(receiver.run(graph_origin),"Delivery clock origin rejected");
      sample.timestamp(2*hns_per_second);
      const auto before=qpc_hns();checked(receiver.receive(sample.sample.Get()),"Uncorrelated driver clock blocked delivery");const auto after=qpc_hns();
      auto frame=await_frame(receiver);
      check(frame->capture_hns>=before && frame->capture_hns<=after && frame->estimated,
            "Real callback time escaped its independently bracketed QPC observation");
      const auto metrics=receiver.metrics();
      check(metrics.at("rawSamplesOutsideGraphClockWindow")==1 && metrics.at("lastRawStreamHns")==2*hns_per_second &&
            metrics.at("rawTimestampClockCorrelationProven")==false && metrics.at("exposureBoundaryMeasured")==false,
            "Untrusted driver clock or exposure uncertainty disappeared from diagnostics");
      check(FAILED(receiver.receive(sample.sample.Get())),"Delivery mode silently accepted a duplicated raw sample timestamp");
      receiver.close();
    }
    cases.push_back({{"realCallbackQpcBracketed",true},{"rawClockEvidenceRetained",true},{"deliveryGapRetained",true}});
  } else if(scope=="timestamps-rows") {
    for(bool bottom:{false,true}) {
      auto type=media(Pixels::Bgra,bottom);auto receiver=std::make_unique<SampleReceiver>(request(),video_format(type.value));Sample sample;
      const auto origin=qpc_hns()-hns_per_second;checked(receiver->run(origin),"QPC origin rejected");sample.timestamp(9000000);
      checked(receiver->receive(sample.sample.Get()),"Valid sample rejected");auto frame=await_frame(*receiver);
      const auto pixels=frame->surface->cpu_pixels();
      check(frame->capture_hns==origin+9000000 && frame->begin_hns==frame->capture_hns && frame->estimated,
        "Stream timestamp was replaced with callback time or lost its estimated exposure flag");
      check(pixels.size()==16 && std::to_integer<unsigned>(pixels[0])==(bottom?8u:0u) &&
        std::to_integer<unsigned>(pixels[3])==255 && std::to_integer<unsigned>(pixels[15])==255,
        "Bottom-up rows or opaque BGRA alpha were corrupted");
      check(FAILED(receiver->receive(sample.sample.Get())),"Duplicate/nonmonotonic timestamp was accepted");
      receiver->close();receiver.reset();check(std::to_integer<unsigned>(pixels[0])==(bottom?8u:0u),"CPU lease died with camera source");
      frame.reset();cases.push_back({{"bottomUp",bottom},{"streamOffsetHns",9000000}});
    }
    for(const auto delta:{int64_t{-1},int64_t{30000000},int64_t{0}}) {
      auto type=media();SampleReceiver receiver(request(),video_format(type.value));Sample sample;
      receiver.run(qpc_hns()-(delta==0?10:1)*hns_per_second);sample.timestamp(delta);
      check(FAILED(receiver.receive(sample.sample.Get())),"Invalid or uncorrelated timestamp was accepted");
      cases.push_back({{"rejectedStreamTime",delta}});
    }
  } else if(scope=="jpeg-startup") {
    auto type=media(Pixels::Mjpg);const auto format=video_format(type.value);
    const auto valid=jpeg_fixture();const std::array<BYTE,16> malformed{};
    auto await_status=[&](SampleReceiver &receiver,uint64_t rejected,bool failed) {
      const auto deadline=GetTickCount64()+1000;
      for(;;) {
        const auto status=receiver.metrics();
        if(status.at("startupMalformedJpegSamples")==rejected &&
           FAILED(status.at("sourceHresult").get<HRESULT>())==failed) return;
        check(GetTickCount64()<deadline,"JPEG startup validation did not finish");
        WaitForSingleObject(receiver.ready_event(),10);
      }
    };
    {
      SampleReceiver receiver(request(),format);Sample sample(65536);
      const auto origin=qpc_hns();receiver.run(origin);
      sample.timestamp(1);sample.payload(malformed);checked(receiver.receive(sample.sample.Get()),"Startup input rejected");
      await_status(receiver,1,false);check(!receiver.poll(),"Malformed JPEG produced invented content");
      sample.timestamp(333334);sample.payload(valid);checked(receiver.receive(sample.sample.Get()),"Real first frame rejected");
      auto frame=await_frame(receiver);check(frame->id==2 && frame->capture_hns==origin+333334,"Startup recovery changed frame identity or time");
      frame.reset();sample.timestamp(666667);sample.payload(malformed);receiver.receive(sample.sample.Get());
      await_status(receiver,1,true);receiver.close();
      cases.push_back({{"realFirstFrameRetainsTime",true},{"corruptionAfterFirstFrameFatal",true}});
    }
    {
      SampleReceiver receiver(request(),format);Sample sample;receiver.run(qpc_hns());sample.payload(malformed);
      for(uint64_t index=0;index<9;++index) {
        sample.timestamp(static_cast<int64_t>(index+1));checked(receiver.receive(sample.sample.Get()),"Startup test submission failed");
        await_status(receiver,std::min(index+1,uint64_t{8}),index==8);
      }
      receiver.close();cases.push_back({{"malformedSampleBudget",8}});
    }
    {
      SampleReceiver receiver(request(),format);Sample sample;receiver.run(qpc_hns()-2*hns_per_second);
      sample.timestamp(2*hns_per_second);sample.payload(malformed);receiver.receive(sample.sample.Get());
      await_status(receiver,0,true);receiver.close();cases.push_back({{"expiredStartupWindowFatal",true}});
    }
  } else if(scope=="formats") {
    const std::string id="\\\\?\\usb#vid_046d&pid_0892&mi_00#6&instance&0&0000#{65e8773d-8f56-11d0-a3b9-00a0c9223196}\\global";
    const auto aliases=identity_aliases(id);check(aliases.size()==1 && same_device(id,aliases[0].get<std::string>()),
      "Camera previous MF identity did not resolve to its DirectShow USB instance");
    auto other=id;other.replace(other.find("mi_00"),5,"mi_01");
    check(!same_device(id,other) && !same_device(id,id.substr(0,id.find("#{"))),"Camera aliases matched a different function or incomplete instance");
    validate_controls(Json{{"exposure",{{"mode","manual"},{"value",-6}}},{"focus",{{"mode","auto"}}}});
    for(const auto &invalid:std::array<Json,4>{Json{{"unknown",{{"mode","auto"}}}},
      Json{{"exposure",{{"mode","auto"},{"value",1}}}},Json{{"exposure",{{"mode","manual"},{"value",1.5}}}},
      Json{{"exposure",{{"mode","manual"},{"value",std::numeric_limits<uint64_t>::max()}}}}}) {
      bool rejected{};try {validate_controls(invalid);}catch(const capture::Error &) {rejected=true;}
      check(rejected,"Invalid camera control request was accepted");
    }
    for(bool info2:{false,true}) for(const auto pixels:{Pixels::Bgra,Pixels::Yuy2,Pixels::Nv12,Pixels::Mjpg}) {
      auto type=media(pixels,false,info2);const auto f=video_format(type.value);
      check(f.width==2 && f.height==2 && f.interval==333333,"Video format parsing failed");
      auto copy=MediaType(type.value);check(copy.value.pbFormat!=type.value.pbFormat,"Media format retained borrowed storage");
      set_interval(copy.value,333666);check(video_format(copy.value).interval==333666 && video_format(type.value).interval==333333,"Media type copy/interval isolation failed");
      cases.push_back({{"subtype",subtype_name(pixels)},{"videoInfo2",info2}});
    }
    for(unsigned kind=0;kind<8;++kind) {
      auto type=media(kind==6?Pixels::Yuy2:kind==7?Pixels::Nv12:Pixels::Bgra);
      auto *info=reinterpret_cast<VIDEOINFOHEADER *>(type.value.pbFormat);
      if(kind==0) info->bmiHeader.biWidth=1;
      if(kind==1) info->bmiHeader.biHeight=std::numeric_limits<LONG>::min();
      if(kind==2) type.value.cbFormat=1;
      if(kind==3) info->rcSource={1,0,2,2};
      if(kind==4) info->bmiHeader.biSizeImage=128*1024*1024;
      if(kind==5) info->bmiHeader.biSize=65536;
      if(kind==6 || kind==7) info->bmiHeader.biCompression=BI_RGB;
      bool rejected{};try {(void)video_format(type.value);}catch(const capture::Error &) {rejected=true;}
      check(rejected,"Malformed camera layout was accepted");
    }
    const std::array<std::byte,8> yuy2{std::byte{16},std::byte{128},std::byte{235},std::byte{128},
      std::byte{16},std::byte{128},std::byte{235},std::byte{128}};
    auto ytype=media(Pixels::Yuy2);std::array<std::byte,16> output{};convert_raw(video_format(ytype.value),yuy2,output);
    check(output[0]==std::byte{0} && output[4]==std::byte{255} && output[7]==std::byte{255},"YUY2 black/white range conversion failed");
    const std::array<std::byte,6> nv12{std::byte{16},std::byte{16},std::byte{235},std::byte{235},std::byte{128},std::byte{128}};
    auto ntype=media(Pixels::Nv12);convert_raw(video_format(ntype.value),nv12,output);
    check(output[0]==std::byte{0} && output[8]==std::byte{255} && output[15]==std::byte{255},"NV12 chroma plane/range conversion failed");
  } else if(scope=="reference-clock") {
    ComPtr<QpcClock> clock;clock.Attach(new QpcClock());REFERENCE_TIME now{};
    const auto before=qpc_hns();checked(clock->GetTime(&now),"Clock GetTime failed");
    check(now>=before && now<=qpc_hns(),"Reference clock is not in engine QPC units");
    const auto event=CreateEventW(nullptr,FALSE,FALSE,nullptr),semaphore=CreateSemaphoreW(nullptr,0,1000,nullptr);
    check(event && semaphore,"Clock test handle allocation failed");
    DWORD_PTR one{},periodic{};
    checked(clock->AdviseTime(now,300000,reinterpret_cast<HEVENT>(event),&one),"Single clock advice failed");
    check(WaitForSingleObject(event,1000)==WAIT_OBJECT_0,"Single clock advice was not delivered");
    check(clock->Unadvise(one)==S_FALSE,"Completed one-shot advice remained registered");
    checked(clock->GetTime(&now),"Clock GetTime failed");
    checked(clock->AdvisePeriodic(now+200000,100000,reinterpret_cast<HSEMAPHORE>(semaphore),&periodic),"Periodic clock advice failed");
    check(WaitForSingleObject(semaphore,1000)==WAIT_OBJECT_0,"Periodic advice was not delivered");
    checked(clock->Unadvise(periodic),"Periodic unadvise failed");
    while(WaitForSingleObject(semaphore,0)==WAIT_OBJECT_0) {}
    check(WaitForSingleObject(semaphore,40)==WAIT_TIMEOUT,"Clock dispatched after Unadvise returned");
    std::array<DWORD_PTR,64> pending{};checked(clock->GetTime(&now),"Clock GetTime failed");
    for(auto &cookie:pending) checked(clock->AdviseTime(now,100000000,reinterpret_cast<HEVENT>(event),&cookie),"Clock advice budget rejected its declared capacity");
    DWORD_PTR overflow{};check(clock->AdviseTime(now,100000000,reinterpret_cast<HEVENT>(event),&overflow)==E_OUTOFMEMORY,"Unbounded clock advice was accepted");
    for(auto cookie:pending) checked(clock->Unadvise(cookie),"Clock advice cleanup failed");
    CloseHandle(event);CloseHandle(semaphore);clock.Reset();cases.push_back({{"boundedAdvices",64},{"periodicCancellation",true}});
  } else if(scope=="sink-contract") {
    auto type=media();auto receiver=std::make_shared<Receiver>();ComPtr<Sink> sink;sink.Attach(new Sink(type.value,receiver));
    ComPtr<OutputPin> output;output.Attach(new OutputPin());Sample sample;
    check(sink->Receive(sample.sample.Get())==VFW_E_WRONG_STATE,"Stopped camera sink accepted samples");
    checked(sink->ReceiveConnection(output.Get(),&type.value),"Camera sink rejected explicit media connection");
    check(sink->ReceiveConnection(output.Get(),&type.value)==VFW_E_ALREADY_CONNECTED,"Camera sink allowed duplicate connection");
    ComPtr<IPin> pin;checked(sink.As(&pin),"Sink pin QI failed");ComPtr<IUnknown> filter_identity,pin_identity;
    checked(sink.As(&filter_identity),"Filter identity failed");checked(pin.As(&pin_identity),"Pin identity failed");
    check(filter_identity.Get()==pin_identity.Get(),"COM canonical identity differs across sink interfaces");
    ComPtr<IEnumPins> pins;checked(sink->EnumPins(&pins),"Pin enumeration failed");ComPtr<IPin> first;
    checked(pins->Next(1,&first,nullptr),"Pin enumeration returned no input");check(first.Get()==pin.Get(),"Pin enumeration returned wrong identity");
    ComPtr<IEnumMediaTypes> types;checked(sink->EnumMediaTypes(&types),"Media type enumeration failed");AM_MEDIA_TYPE *copy{};
    checked(types->Next(1,&copy,nullptr),"Media type enumeration returned no type");check(copy->pbFormat!=type.value.pbFormat,"Enumerator returned borrowed format memory");free_type(copy);
    checked(sink->NotifyAllocator(sample.allocator.Get(),TRUE),"Sink rejected valid sample allocator");
    checked(sink->Pause(),"Sink pause failed");FILTER_STATE state{};
    check(sink->GetState(0,&state)==VFW_S_CANT_CUE && state==State_Paused,"Live sink waits for impossible paused preroll");
    const auto origin=qpc_hns();checked(sink->Run(origin),"Sink Run failed");
    std::array<IMediaSample *,2> samples{sample.sample.Get(),sample.sample.Get()};LONG processed{};
    checked(sink->ReceiveMultiple(samples.data(),2,&processed),"ReceiveMultiple failed");
    check(processed==2 && receiver->samples==2 && receiver->origin==origin,"Sink lost samples or graph Run origin");
    checked(sink->BeginFlush(),"BeginFlush failed");check(sink->Receive(sample.sample.Get())==S_FALSE,"Flushing sink accepted a sample");
    checked(sink->EndFlush(),"EndFlush failed");checked(sink->EndOfStream(),"EOS failed");check(receiver->ends==1,"EOS did not reach capture owner");
    check(sink->Disconnect()==VFW_E_NOT_STOPPED,"Running sink allowed disconnect");checked(sink->Stop(),"Sink Stop failed");
    checked(sink->Disconnect(),"Sink disconnect failed");cases.push_back({{"received",processed},{"canonicalComIdentity",true}});
  } else throw std::invalid_argument("Unknown camera contract scope");
  check(live_sources==0 && live_pool_bytes==before_bytes && live_frame_leases==before_leases,"Camera contract leaked a physical source or pool lease");
  return {{"status","PASS"},{"scope",scope},{"cases",cases},{"physicalSourceOpened",false},{"captureApi","DirectShow"}};
}
} // namespace nss::camera
