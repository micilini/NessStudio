#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "capture/readback.hpp"
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <array>
#include <future>
#include <iostream>

using Microsoft::WRL::ComPtr;
using namespace nss::capture;
namespace nss::capture {
void set_qualification_readback_map_busy(uint32_t attempts);
void set_qualification_readback_fence_pending(uint32_t attempts);
void set_qualification_readback_packed_failure(bool fail);
}
void check(bool condition,const char *message) {if(!condition) throw std::runtime_error(message);}
struct GpuImage final : Surface {
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11Texture2D> texture;
  void *gpu_device() const noexcept override {return device.Get();}
  mutable bool available=true;
  void *gpu_texture() const noexcept override {return available?texture.Get():nullptr;}
};
uint8_t sample(uint32_t row,uint32_t column,uint8_t seed) {
  return static_cast<uint8_t>(16+(row*7+column*3+seed)%220);
}
static Json packed_contract(ID3D11Device *device,ID3D11DeviceContext *context,bool queries,uint32_t width) {
  constexpr uint32_t height=32;
  const auto stride=(width+3)&~3U;
  struct Writer {
    ID3D11DeviceContext *context;
    std::vector<uint8_t> pixels;
    uint32_t calls{};
    bool fail{};
  } writer{context,std::vector<uint8_t>(size_t(stride)*(height+height/2),0xfe)};
  const Readback::PackedWriter callback{&writer,[](void *opaque,void *view) {
    auto &state=*static_cast<Writer *>(opaque);
    ComPtr<ID3D11Resource> target;
    static_cast<ID3D11UnorderedAccessView *>(view)->GetResource(&target);
    state.context->UpdateSubresource(target.Get(),0,nullptr,state.pixels.data(),0,0);
    ++state.calls;
    if(state.fail) throw std::runtime_error("Injected producer failure after queued writes");
  }};
  auto input=[&](uint64_t id,uint8_t seed) {
    auto surface=std::make_shared<GpuImage>();surface->device=device;surface->available=false;
    for(uint32_t row=0;row<height+height/2;++row)
      for(uint32_t column=0;column<width;++column) writer.pixels[row*stride+column]=sample(row,column,seed);
    Frame frame;frame.surface=surface;frame.id=id;frame.generation=13;
    frame.begin_hns=10;frame.end_hns=14;frame.capture_hns=12;frame.presentation_hns=15;
    frame.width=width;frame.height=height;frame.format=Frame::Format::Nv12;return frame;
  };
  auto verify=[&](const Frame &frame,uint64_t id,uint8_t seed) {
    check(frame.id==id && frame.generation==13 && frame.begin_hns==10 && frame.end_hns==14 &&
          frame.capture_hns==12 && frame.presentation_hns==15,"Packed readback changed frame identity");
    const auto bytes=frame.surface->cpu_pixels();
    check(frame.stride==width && bytes.size()==size_t(width)*(height+height/2),"Packed readback copied row padding");
    for(uint32_t row=0;row<height+height/2;++row)
      for(uint32_t column=0;column<width;++column)
        check(std::to_integer<uint8_t>(bytes[size_t(row)*width+column])==sample(row,column,seed),
              "Packed readback changed bytes or overwrote a retained CPU lease");
  };
  auto bridge=std::make_unique<Readback>(false,queries);
  auto prepare=[&](Frame &frame) {
    const auto result=bridge->prepare_packed_queued(frame,context,callback);
    check(result.supported,"WARP packed buffer support was not exercised");
    if(result.submitted) context->Flush();
    return result;
  };
  auto finish=[&](Frame &frame) {
    const auto deadline=GetTickCount64()+3000;
    while(!prepare(frame).ready) {check(GetTickCount64()<deadline,"Packed readback stalled");Sleep(1);}
  };
  auto original=input(1,1),first=original;
  ComPtr<ID3D11DeviceContext> deferred;
  checked(device->CreateDeferredContext(0,&deferred),NSS_STAGE_PROCESS,"Packed deferred context test failed");
  bool rejected=false;
  try {bridge->prepare_packed_queued(first,deferred.Get(),callback);}
  catch(const Error &e) {rejected=e.code==NSS_E_ARGUMENT;}
  check(rejected && writer.calls==0,"Packed readback accepted an unordered producer context");
  check(std::async(std::launch::async,[&] {
    try {bridge->prepare_packed_queued(first,context,callback);return false;}
    catch(const Error &e) {return e.code==NSS_E_STATE;}
  }).get(),"Packed readback accepted a foreign owner thread");
  check(prepare(first).submitted,"Packed copy was not submitted");
  set_qualification_readback_fence_pending(1);
  check(!prepare(first).ready && writer.calls==1,"Packed output was published before completion");
  set_qualification_readback_map_busy(1);
  finish(first);verify(first,1,1);
  check(bridge->metrics().at("mapWouldBlockRetries")==1,"Packed Map busy recovery was not tested");
  auto repeat=original;
  check(prepare(repeat).ready && writer.calls==1,"Packed cache repeated conversion");
  auto second=input(2,2);finish(second);
  auto third=input(3,3);finish(third);
  auto pressured=input(4,4);
  const auto pressure_deadline=GetTickCount64()+3000;
  while(bridge->metrics().at("cpuOutputPressure")==0) {
    check(!prepare(pressured).ready,"Packed copy overwrote an owned CPU buffer");
    check(GetTickCount64()<pressure_deadline,"Packed output pressure was not observed");Sleep(1);
  }
  check(!bridge->completion_event(),"Packed CPU pressure exposed a stale GPU wakeup");
  verify(first,1,1);verify(second,2,2);verify(third,3,3);
  second={};finish(pressured);verify(pressured,4,4);pressured={};
  for(uint32_t index=0;index<128;++index) {
    auto reused=input(100+index,static_cast<uint8_t>(index));finish(reused);
    verify(reused,100+index,static_cast<uint8_t>(index));
  }
  verify(first,1,1);verify(third,3,3);
  check(bridge->metrics().at("cpuOutputAllocations")==3,"Packed CPU storage exceeded its lease budget");
  auto failed=input(500,7);writer.fail=true;
  bool injected=false;
  try {prepare(failed);}catch(const std::runtime_error &) {injected=true;}
  check(injected && bridge->pending() && !bridge->completion_event(),"Failed producer was not retained safely");
  writer.fail=false;
  const auto failed_calls=writer.calls;
  finish(failed);verify(failed,500,7);
  check(writer.calls==failed_calls+1,"Failed producer pixels were accepted without a fresh conversion");
  failed={};
  auto abandoned=input(501,8);
  std::weak_ptr<const Surface> lifetime=abandoned.surface;
  const auto discarded=bridge->prepare_packed_queued(abandoned,context,callback);
  check(discarded.submitted,"Packed abandonment test did not queue writes");
  abandoned={};check(!lifetime.expired(),"Packed copy lost its GPU source before completion");
  const auto close_deadline=GetTickCount64()+3000;
  while(!bridge->quiesce()) {check(GetTickCount64()<close_deadline,"Unflushed packed close stalled");Sleep(1);}
  check(lifetime.expired(),"Packed close retained an abandoned source");
  const auto metrics=bridge->metrics();bridge.reset();original={};
  verify(first,1,1);verify(third,3,3);
  return metrics;
}
int main() {
  try {
    Json report;
    for(const bool prefer_queries : {false,true}) {
      ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
      checked(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,
        &device,nullptr,&context),NSS_STAGE_PROCESS,"WARP device creation failed");
      auto input=[&](uint64_t id,uint8_t seed) {
        auto image=std::make_shared<GpuImage>();image->device=device;
        constexpr uint32_t stride=96,width=64,height=32;
        std::array<uint8_t,stride*height*3/2> storage{};storage.fill(0xfe);
        for(uint32_t row=0;row<height+height/2;++row)
          for(uint32_t column=0;column<width;++column) storage[row*stride+column]=sample(row,column,seed);
        D3D11_TEXTURE2D_DESC td{};td.Width=width;td.Height=height;td.MipLevels=td.ArraySize=1;
        td.SampleDesc.Count=1;td.Format=DXGI_FORMAT_NV12;td.Usage=D3D11_USAGE_DEFAULT;
        const D3D11_SUBRESOURCE_DATA initial{storage.data(),stride,0};
        checked(device->CreateTexture2D(&td,&initial,&image->texture),NSS_STAGE_PROCESS,"NV12 test upload failed");
        Frame frame;frame.surface=image;frame.id=id;frame.generation=7;frame.begin_hns=120;
        frame.end_hns=125;frame.capture_hns=123;frame.presentation_hns=130;
        frame.width=width;frame.height=height;frame.format=Frame::Format::Nv12;return frame;
      };
      auto verify=[](const Frame &frame,uint64_t id,uint8_t seed) {
        check(frame.id==id && frame.generation==7 && frame.begin_hns==120 && frame.end_hns==125 &&
          frame.capture_hns==123 && frame.presentation_hns==130,"Readback changed acquisition identity/timestamps");
        const auto bytes=frame.surface->cpu_pixels();
        check(frame.stride==64 && bytes.size()==64*32*3/2 && !frame.surface->gpu_texture(),"Readback layout or ownership is invalid");
        for(uint32_t row=0;row<48;++row)
          for(uint32_t column=0;column<64;++column)
            check(std::to_integer<uint8_t>(bytes[row*64+column])==sample(row,column,seed),"Readback changed visible NV12 bytes or copied padding");
      };
      if(!prefer_queries) {
        Readback notifications;
        auto waiting=input(88,2);
        check(!notifications.prepare(waiting),"Notification test did not queue a copy");
        auto event=notifications.completion_event();
        check(event && WaitForSingleObject(event,3000)==WAIT_OBJECT_0,
              "WARP readback fence notification was not delivered");
        // Keep the completed copy pending, as happens while another frame's
        // conversion is in progress. Its old notification must not wake again.
        check(notifications.pending() && WaitForSingleObject(event,0)==WAIT_TIMEOUT,
              "A consumed readback notification would repeatedly wake another conversion");
        const auto deadline=GetTickCount64()+3000;
        while(!notifications.prepare(waiting)) {
          check(GetTickCount64()<deadline,"Readback did not finish after its single wake");Sleep(1);
        }
        verify(waiting,88,2);
        auto next_copy=input(89,3);
        check(!notifications.prepare(next_copy),"Second notification copy did not queue");
        event=notifications.completion_event();
        // Model a late signal from a previous registration while the current
        // copy is not yet complete. A signal is only a wakeup hint. The pending
        // response is injected only in the qualification library.
        set_qualification_readback_fence_pending(1);
        check(event && SetEvent(event) && WaitForSingleObject(event,0)==WAIT_OBJECT_0 &&
              WaitForSingleObject(event,0)==WAIT_TIMEOUT,
              "A stale readback signal could cause repeated immediate wakeups");
        check(!notifications.prepare(next_copy) && next_copy.surface->cpu_pixels().empty(),
              "A stale event published pixels before the actual copy fence");
        while(!notifications.prepare(next_copy)) {
          check(GetTickCount64()<deadline,"The copy did not become readable after a stale wake");Sleep(1);
        }
        verify(next_copy,89,3);
        while(!notifications.quiesce()) {
          check(GetTickCount64()<deadline,"Notification test failed to quiesce");Sleep(1);
        }
      }
      {
        Readback retry_bridge(false,prefer_queries);
        auto retried=input(90,4);
        set_qualification_readback_map_busy(1);
        bool observed_retry=false;
        const auto retry_deadline=GetTickCount64()+3000;
        while(!retry_bridge.prepare(retried)) {
          check(GetTickCount64()<retry_deadline,"Nonblocking Map retry stalled");
          if(retry_bridge.metrics().at("mapWouldBlockRetries").get<uint64_t>()) {
            observed_retry=true;
            check(retry_bridge.pending() && !retry_bridge.completion_event(),
                  "Map retry exposed the already-signaled fence and would busy-loop");
          }
          if(auto event=retry_bridge.completion_event()) WaitForSingleObject(event,10);else Sleep(1);
        }
        check(observed_retry && !retry_bridge.pending() &&
              !retry_bridge.metrics().at("mapRetryPending").get<bool>(),
              "Map retry did not recover after preserving its source lease");
        verify(retried,90,4);
        auto following=input(91,5);
        check(!retry_bridge.prepare(following),"Following copy unexpectedly completed inline");
        check(!retry_bridge.metrics().at("mapRetryPending").get<bool>(),
              "New copy inherited an earlier Map retry state");
        while(!retry_bridge.quiesce()) {
          check(GetTickCount64()<retry_deadline,"Following copy did not quiesce");Sleep(1);
        }
      }
      {
        Readback ordered(false,prefer_queries);
        auto queued=input(3000,11);
        auto image=std::dynamic_pointer_cast<const GpuImage>(queued.surface);
        image->available=false;
        constexpr uint32_t stride=96;
        std::array<uint8_t,stride*48> update{};update.fill(0xfe);
        for(uint32_t row=0;row<48;++row)
          for(uint32_t column=0;column<64;++column) update[row*stride+column]=sample(row,column,31);
        context->UpdateSubresource(image->texture.Get(),0,nullptr,update.data(),stride,0);
        bool ordinary_rejected=false;
        try {ordered.prepare(queued);}catch(const Error &e){ordinary_rejected=e.code==NSS_E_ARGUMENT;}
        check(ordinary_rejected,"Normal readback accepted a GPU surface before readiness");
        ComPtr<ID3D11DeviceContext> deferred;
        checked(device->CreateDeferredContext(0,&deferred),NSS_STAGE_PROCESS,"Deferred test context creation failed");
        bool deferred_rejected=false;
        try {ordered.prepare_queued(queued,image->texture.Get(),deferred.Get());}
        catch(const Error &e){deferred_rejected=e.code==NSS_E_ARGUMENT;}
        check(deferred_rejected && ordered.metrics().at("copies")==0,
              "Queued readback accepted a context outside the producer's ordered stream");
        check(std::async(std::launch::async,[&] {
          try {ordered.prepare_queued(queued,image->texture.Get(),context.Get());return false;}
          catch(const Error &e){return e.code==NSS_E_STATE;}
        }).get(),"Queued readback accepted a foreign owner thread");
        auto prepared=ordered.prepare_queued(queued,image->texture.Get(),context.Get());
        check(!prepared.ready && prepared.submitted && ordered.pending(),
              "Queued readback did not retain its in-flight copy");
        context->Flush();
        auto event=ordered.completion_event();
        check(prefer_queries ? !event : (event && WaitForSingleObject(event,3000)==WAIT_OBJECT_0 &&
              WaitForSingleObject(event,0)==WAIT_TIMEOUT),
              "Queued readback reused an already-consumed completion notification");
        set_qualification_readback_fence_pending(1);
        prepared=ordered.prepare_queued(queued,image->texture.Get(),context.Get());
        check(!prepared.ready && !prepared.submitted && queued.surface->cpu_pixels().empty(),
              "Queued readback published pixels based only on its event");
        const auto copy_deadline=GetTickCount64()+3000;
        while(!prepared.ready) {
          check(GetTickCount64()<copy_deadline,"Queued readback did not complete");
          Sleep(1);
          prepared=ordered.prepare_queued(queued,image->texture.Get(),context.Get());
          check(!prepared.submitted,"Queued readback submitted the same copy twice");
        }
        verify(queued,3000,31);
        check(!image->gpu_texture(),"Queued readback relaxed generic GPU readiness");
        check(ordered.metrics().at("orderedCopies")==1,"Ordered readback was not accounted for");
        auto discarded=input(3001,32);
        auto discarded_image=std::dynamic_pointer_cast<const GpuImage>(discarded.surface);
        discarded_image->available=false;
        std::weak_ptr<const Surface> lifetime=discarded.surface;
        const auto pending_copy=ordered.prepare_queued(discarded,discarded_image->texture.Get(),context.Get());
        check(!pending_copy.ready && pending_copy.submitted,"Abandoned ordered copy was not queued");
        // Deliberately abandon before flushing: cleanup must submit its batch
        // and retain the source/event until the final staging fence completes.
        discarded={};discarded_image.reset();
        check(!lifetime.expired(),"Queued copy lost its source lease before completion");
        const auto close_deadline=GetTickCount64()+3000;
        while(!ordered.quiesce()) {check(GetTickCount64()<close_deadline,"Queued copy failed to quiesce");Sleep(1);}
        check(lifetime.expired(),"Completed ordered copy retained its abandoned source");
      }
      auto readback=std::make_unique<Readback>(false,prefer_queries);
      report[prefer_queries?"packedQuery":"packedFence"]=packed_contract(device.Get(),context.Get(),prefer_queries,64);
      report[prefer_queries?"packedPaddedQuery":"packedPaddedFence"]=packed_contract(device.Get(),context.Get(),prefer_queries,66);
      {
        Readback fallback(false,prefer_queries);
        auto unavailable=input(6000,33);
        set_qualification_readback_packed_failure(true);
        const auto unsupported=fallback.prepare_packed_queued(unavailable,context.Get(),
            {nullptr,[](void *,void *) {throw std::runtime_error("Unsupported packed producer was called");}});
        set_qualification_readback_packed_failure(false);
        check(!unsupported.supported && !unsupported.submitted && !fallback.pending(),
              "Unsupported packed readback did not preserve the texture fallback");
        const auto deadline=GetTickCount64()+3000;
        while(!fallback.prepare(unavailable)) {check(GetTickCount64()<deadline,"Texture fallback stalled");Sleep(1);}
        verify(unavailable,6000,33);
      }
      auto original=input(100,5),frame=original;
      auto finish=[&](Frame &value) {
        const auto deadline=GetTickCount64()+3000;
        while(!readback->prepare(value)) {
          check(GetTickCount64()<deadline,"Readback completion stalled");
          if(auto event=readback->completion_event()) WaitForSingleObject(event,10);else Sleep(1);
        }
      };
      finish(frame);verify(frame,100,5);
      auto repeated=original;check(readback->prepare(repeated),"Repeated source missed readback cache");verify(repeated,100,5);
      check(readback->metrics().at("copies")==1,"Repeated identity copied GPU pixels again");
      check(std::async(std::launch::async,[&] {
        try {readback->prepare(original);return false;}catch(const Error &e){return e.code==NSS_E_STATE;}
      }).get(),"Readback accepted a foreign owner thread");
      auto abandoned=input(101,6),newer=input(102,7);
      check(!readback->prepare(abandoned),"Readback unexpectedly completed inline");
      finish(newer);verify(newer,102,7);
      check(readback->metrics().at("supersededCopies")==1,"Superseded copy was not accounted for");
      auto third=input(104,9);finish(third);verify(third,104,9);
      auto blocked=input(105,10);
      const auto pressure_deadline=GetTickCount64()+3000;
      while(readback->metrics().at("cpuOutputPressure")==0) {
        check(!readback->prepare(blocked),"Readback overwrote a retained CPU lease");
        check(GetTickCount64()<pressure_deadline,"CPU lease pressure was not observed");
        Sleep(1);
      }
      check(readback->pending() && !readback->completion_event(),
        "CPU lease pressure exposed an already signaled GPU wait");
      verify(frame,100,5);verify(newer,102,7);verify(third,104,9);
      newer={};finish(blocked);verify(blocked,105,10);
      check(readback->metrics().at("cpuOutputAllocations")==3,
        "CPU readback storage exceeded its bounded lease budget");
      blocked={};
      for(uint64_t index=0;index<512;++index) {
        const auto seed=static_cast<uint8_t>(index);
        auto recycling=input(1000+index,seed);finish(recycling);verify(recycling,1000+index,seed);
      }
      check(readback->metrics().at("cpuOutputAllocations")==3 &&
        readback->metrics().at("cpuOutputReuses").get<uint64_t>()>=513,
        "Steady-state readback allocated new pixels instead of reusing released leases");
      verify(frame,100,5);verify(third,104,9);
      auto pending=input(103,8);check(!readback->prepare(pending),"Pending copy unexpectedly completed inline");
      const auto deadline=GetTickCount64()+3000;
      while(!readback->quiesce()) {check(GetTickCount64()<deadline,"Readback cleanup stalled");Sleep(1);}
      const auto completed_metrics=readback->metrics();readback.reset();
      original={};abandoned={};pending={};verify(frame,100,5);verify(third,104,9);
      check(!completed_metrics.at("pending").get<bool>(),"Readback cleanup retained a pending copy");
      check(completed_metrics.at("queryCompletionPreferred")==prefer_queries &&
            completed_metrics.at("completion")== (prefer_queries?"D3D11 event query":"D3D11 fence event"),
            "Requested readback completion policy was not exercised");
      report[prefer_queries?"query":"fence"]=completed_metrics;
    }
    check(!live_surfaces && !live_capture_bytes,"Readback leaked native ownership");
    std::cout<<Json{{"status","PASS"},{"nv12VisibleBytesExact",true},{"stridePaddingExcluded",true},
      {"acquisitionTimesPreserved",true},{"cpuLeaseAfterBridgeRelease",true},{"quiescence",true},
      {"retainedCpuPixelsSurvive512Reuses",true},{"boundedCpuLeasePressure",true},
      {"mapBusyAfterFenceUsesBoundedRetry",true},{"mapRetryPreservesPixelsAndOwnership",true},
      {"readbackCompletionSingleWakePerCopy",true},
      {"staleReadbackSignalCannotPublishPixels",true},
      {"queuedWriteReadbackBytesExact",true},{"foreignQueuedContextRejected",true},
      {"genericGpuReadinessPreserved",true},{"abandonedUnflushedCopyLeaseReleased",true},
      {"queuedReadbackUsesSingleWakeAndFinalFence",true},
      {"eventQueryPathAlsoVerified",true},
      {"readback",report}}.dump()<<'\n';
    return 0;
  } catch(const std::exception &e) {std::cerr<<e.what()<<'\n';return 1;}
}
