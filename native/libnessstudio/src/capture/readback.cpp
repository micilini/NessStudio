#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "readback.hpp"
#include "gpu_timing.hpp"
#include <windows.h>
#include <d3d11_4.h>
#include <wrl/client.h>
#include <array>
#include <cstring>
#include <thread>

namespace nss::capture {
#ifdef NSS_CAPTURE_QUALIFICATION
static thread_local uint32_t qualification_map_busy_count{};
static thread_local uint32_t qualification_fence_pending_count{};
static thread_local HRESULT qualification_packed_result=S_OK;
void set_qualification_readback_packed_failure(bool fail) {
  qualification_packed_result=fail?DXGI_ERROR_UNSUPPORTED:S_OK;
}
void set_qualification_readback_map_busy(uint32_t attempts) {
  qualification_map_busy_count=attempts;
}
void set_qualification_readback_fence_pending(uint32_t attempts) {
  qualification_fence_pending_count=attempts;
}
#endif
namespace {
using Microsoft::WRL::ComPtr;
struct CpuImage final : Surface {
  std::vector<std::byte> bytes;
  explicit CpuImage(size_t size):bytes(size) {live_capture_bytes+=bytes.size();}
  ~CpuImage() override {live_capture_bytes-=bytes.size();}
  std::span<const std::byte> cpu_pixels() const override {return bytes;}
};
struct Key {
  const Surface *surface{};
  uint64_t id{},generation{};
  uint32_t width{},height{};
  bool operator==(const Key &) const = default;
};
Key key(const Frame &frame) {return {frame.surface.get(),frame.id,frame.generation,frame.width,frame.height};}
}
struct Readback::Impl {
  const std::thread::id owner=std::this_thread::get_id();
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  ComPtr<ID3D11Texture2D> staging;
  ComPtr<ID3D11Buffer> packed, packed_staging;
  ComPtr<ID3D11UnorderedAccessView> packed_view;
  ComPtr<ID3D11Resource> pending_resource;
  uint32_t packed_stride{}, pending_stride{};
  uint64_t packed_copies{};
  HRESULT packed_result=S_FALSE;
  ComPtr<ID3D11Fence> fence;
  ComPtr<ID3D11DeviceContext4> timeline;
  ComPtr<ID3D11Query> query;
  GpuTiming copy_timing;
  bool prefer_query_completion;
  explicit Impl(bool timing_diagnostic, bool prefer_queries)
      : copy_timing(timing_diagnostic), prefer_query_completion(prefer_queries) {}
  HANDLE event{};
  uint64_t signal{},copies{},bytes{},cache_hits{},superseded{},polls{},map_would_block{},ordered_copies{};
  static constexpr size_t output_limit = 3;
  std::array<std::shared_ptr<CpuImage>,output_limit> outputs;
  uint64_t output_allocations{},output_reuses{},output_pressure{};
  bool output_blocked{},map_blocked{};
  bool pending_query{},pending_failed{};
  uint32_t width{},height{};
  std::optional<Frame> input;
  std::optional<Key> cached_key;
  std::shared_ptr<const Surface> cached;
  std::shared_ptr<CpuImage> acquire_output() {
    // Only this owner publishes buffers. A consumer's strong surface lease
    // prevents reuse, including after this bridge has been destroyed.
    // The cache is an internal reference: invalidate it before recycling its
    // pixels when no caller still owns them. No source identity is retimed.
    for(auto &output:outputs) {
      if(!output) continue;
      const bool is_cached=output.get()==cached.get();
      if(output.use_count()!=(is_cached?2:1)) continue;
      if(is_cached) {cached.reset();cached_key.reset();}
      ++output_reuses;output_blocked=false;return output;
    }
    for(auto &output:outputs) {
      if(output) continue;
      output=std::make_shared<CpuImage>(size_t(width)*height*3/2);
      ++output_allocations;output_blocked=false;return output;
    }
    ++output_pressure;output_blocked=true;return {};
  }
  ~Impl() {
    // Session cleanup polls quiesce before destruction. Keep standalone callers
    // safe too: an event registered with a live GPU fence must outlive its copy.
    if(input) context->Flush();
    while(input) {
      if(FAILED(device->GetDeviceRemovedReason())) break;
      try {if(complete()) break;} catch(...) {break;}
      Sleep(1);
    }
    if(event) CloseHandle(event);
  }
  void thread() const {
    if(std::this_thread::get_id()!=owner)
      throw Error(NSS_E_STATE,NSS_STAGE_PROCESS,0,"GPU readback requires its creating thread");
  }
  void initialize(const Frame &frame, ID3D11DeviceContext *ordered_context) {
    if(!frame.surface || !frame.surface->gpu_device() ||
       frame.format!=Frame::Format::Nv12 || !frame.width || !frame.height ||
       (frame.width&1) || (frame.height&1) || frame.width>4096 || frame.height>4096)
      throw Error(NSS_E_ARGUMENT,NSS_STAGE_PROCESS,0,"Software encoder readback requires even NV12 GPU input");
    auto *actual=static_cast<ID3D11Device *>(frame.surface->gpu_device());
    if(ordered_context) {
      ComPtr<ID3D11DeviceContext> immediate;actual->GetImmediateContext(&immediate);
      if(ordered_context!=immediate.Get())
        throw Error(NSS_E_ARGUMENT,NSS_STAGE_PROCESS,0,"Queued readback requires the producer's immediate context");
    }
    if(device) {
      if(device.Get()!=actual || width!=frame.width || height!=frame.height)
        throw Error(NSS_E_UNAVAILABLE,NSS_STAGE_PROCESS,0,"GPU readback device/geometry changed during recording");
      return;
    }
    device=actual;device->GetImmediateContext(&context);width=frame.width;height=frame.height;
    // The query also protects queued resources if a producer throws after
    // writing, before the ordinary fence can be registered.
    D3D11_QUERY_DESC description{D3D11_QUERY_EVENT,0};
    checked(device->CreateQuery(&description,&query),NSS_STAGE_PROCESS,"GPU readback completion query failed");
    ComPtr<ID3D11Device5> version5;
    if(!prefer_query_completion && SUCCEEDED(device.As(&version5)) && SUCCEEDED(context.As(&timeline)) &&
       SUCCEEDED(version5->CreateFence(0,D3D11_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)))) {
      // One wake per copy. A completed input can remain retained while the
      // caller prepares a replacement frame; a manual-reset event would
      // repeatedly wake that unrelated wait until prepare() retires the input.
      event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
      if(!event) checked(HRESULT_FROM_WIN32(GetLastError()),NSS_STAGE_PROCESS,"GPU readback event creation failed");
    } else {
      fence.Reset();timeline.Reset();
    }
  }
  void initialize_texture(const Frame &frame, ID3D11Texture2D *source,
                          ID3D11DeviceContext *ordered_context) {
    if(!source) throw Error(NSS_E_ARGUMENT,NSS_STAGE_PROCESS,0,"GPU readback texture is not ready");
    ComPtr<ID3D11Device> actual;source->GetDevice(&actual);
    D3D11_TEXTURE2D_DESC desc{};source->GetDesc(&desc);
    if(!frame.surface || actual.Get()!=frame.surface->gpu_device() || desc.Format!=DXGI_FORMAT_NV12 ||
       desc.Width<frame.width || desc.Height<frame.height || desc.ArraySize!=1 || desc.MipLevels!=1 ||
       desc.SampleDesc.Count!=1)
      throw Error(NSS_E_ARGUMENT,NSS_STAGE_PROCESS,0,"GPU readback texture layout differs from the frame");
    initialize(frame,ordered_context);
    if(staging) return;
    D3D11_TEXTURE2D_DESC target{};target.Width=width;target.Height=height;
    target.MipLevels=target.ArraySize=1;target.Format=DXGI_FORMAT_NV12;target.SampleDesc.Count=1;
    target.Usage=D3D11_USAGE_STAGING;target.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    checked(device->CreateTexture2D(&target,nullptr,&staging),NSS_STAGE_PROCESS,"GPU readback staging allocation failed");
  }
  bool initialize_packed() {
    if(packed_result!=S_FALSE) return packed_result==S_OK;
    packed_stride=(width+3)&~3U;
    D3D11_BUFFER_DESC desc{};desc.ByteWidth=packed_stride*(height+height/2);
    desc.Usage=D3D11_USAGE_DEFAULT;desc.BindFlags=D3D11_BIND_UNORDERED_ACCESS;
    HRESULT result=S_OK;
#ifdef NSS_CAPTURE_QUALIFICATION
    result=qualification_packed_result;
#endif
    if(SUCCEEDED(result)) result=device->CreateBuffer(&desc,nullptr,&packed);
    D3D11_UNORDERED_ACCESS_VIEW_DESC view{};
    view.Format=DXGI_FORMAT_R8G8B8A8_UNORM;view.ViewDimension=D3D11_UAV_DIMENSION_BUFFER;
    view.Buffer.NumElements=desc.ByteWidth/4;
    if(SUCCEEDED(result)) result=device->CreateUnorderedAccessView(packed.Get(),&view,&packed_view);
    desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    if(SUCCEEDED(result)) result=device->CreateBuffer(&desc,nullptr,&packed_staging);
    if(FAILED(result)) {
      packed.Reset();packed_staging.Reset();packed_view.Reset();
      checked(device->GetDeviceRemovedReason(),NSS_STAGE_PROCESS,"Packed readback device was removed");
      if(result!=E_INVALIDARG && result!=E_NOINTERFACE && result!=DXGI_ERROR_UNSUPPORTED)
        checked(result,NSS_STAGE_PROCESS,"Packed NV12 readback allocation failed");
    }
    packed_result=result;return result==S_OK;
  }
  bool complete() {
    ++polls;
#ifdef NSS_CAPTURE_QUALIFICATION
    if(qualification_fence_pending_count) {--qualification_fence_pending_count;return false;}
#endif
    checked(device->GetDeviceRemovedReason(),NSS_STAGE_PROCESS,"GPU readback device was removed");
    if(fence && !pending_query) {
      const auto completed=fence->GetCompletedValue();
      if(completed==UINT64_MAX) {
        checked(device->GetDeviceRemovedReason(),NSS_STAGE_PROCESS,"GPU readback fence device failed");
        throw Error(NSS_E_INTERNAL,NSS_STAGE_PROCESS,0,"GPU readback fence returned an invalid value");
      }
      return completed>=signal;
    }
    BOOL ready{};const auto result=context->GetData(query.Get(),&ready,sizeof(ready),D3D11_ASYNC_GETDATA_DONOTFLUSH);
    if(result==S_FALSE) return false;
    checked(result,NSS_STAGE_PROCESS,"GPU readback completion query failed");return ready!=FALSE;
  }
  std::optional<bool> retire(Frame &frame) {
    const auto requested=key(frame);
    if(cached_key && *cached_key==requested) {
      ++cache_hits;frame.surface=cached;frame.stride=width;return true;
    }
    if(input) {
      if(!complete()) return false;
      if(!pending_failed && key(*input)==requested) {
        auto output=acquire_output();
        if(!output) return false;
        D3D11_MAPPED_SUBRESOURCE map{};
#ifdef NSS_CAPTURE_QUALIFICATION
        const auto result=qualification_map_busy_count
            ? (--qualification_map_busy_count,DXGI_ERROR_WAS_STILL_DRAWING)
            : context->Map(pending_resource.Get(),0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&map);
#else
        const auto result=context->Map(pending_resource.Get(),0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&map);
#endif
        if(result==DXGI_ERROR_WAS_STILL_DRAWING) {
          // Fence completion and CPU map availability are distinct. Waiting
          // on the already-signaled fence here would busy-loop; the owner
          // must use its bounded polling fallback until Map is available.
          ++map_would_block;map_blocked=true;return false;
        }
        map_blocked=false;
        checked(result,NSS_STAGE_PROCESS,"GPU readback mapping failed");
        struct Unmap {ID3D11DeviceContext *context;ID3D11Resource *resource;~Unmap(){context->Unmap(resource,0);}} unmap{context.Get(),pending_resource.Get()};
        const auto row_pitch=pending_stride?pending_stride:map.RowPitch;
        if(!map.pData || row_pitch<width)
          throw Error(NSS_E_INTERNAL,NSS_STAGE_PROCESS,0,"GPU readback returned an invalid NV12 row layout");
        const auto *mapped_source=static_cast<const std::byte *>(map.pData);
        for(uint32_t row=0;row<height+height/2;++row)
          std::memcpy(output->bytes.data()+size_t(row)*width,mapped_source+size_t(row)*row_pitch,width);
        cached_key=requested;cached=output;frame.surface=output;frame.stride=width;
        bytes+=output->bytes.size();input.reset();return true;
      }
      input.reset();++superseded;
    }
    return std::nullopt;
  }
  void mark_completion(bool ordered) {
    pending_query=!fence;pending_failed=false;
    if(fence) {
      if(signal>=UINT64_MAX-1) throw Error(NSS_E_STATE,NSS_STAGE_PROCESS,0,"GPU readback fence domain exhausted");
      if(!ResetEvent(event)) checked(HRESULT_FROM_WIN32(GetLastError()),NSS_STAGE_PROCESS,"GPU readback event reset failed");
      checked(timeline->Signal(fence.Get(),++signal),NSS_STAGE_PROCESS,"GPU readback fence signal failed");
      checked(fence->SetEventOnCompletion(signal,event),NSS_STAGE_PROCESS,"GPU readback completion registration failed");
    } else context->End(query.Get());
    if(ordered) ++ordered_copies;
    else context->Flush();
    ++copies;
  }
  void protect_failed_submission() noexcept {
    // Even a failing callback can already have enqueued GPU writes. Retain
    // its source until this later marker completes; never wait on an unissued
    // fence value or expose its partially written pixels as a successful frame.
    pending_query=true;pending_failed=true;context->End(query.Get());context->Flush();
  }
  bool prepare(Frame &frame, ID3D11Texture2D *source,
               ID3D11DeviceContext *ordered_context, bool &submitted) {
    submitted=false;thread();
    if(!ordered_context)
      source=frame.surface?static_cast<ID3D11Texture2D *>(frame.surface->gpu_texture()):nullptr;
    initialize_texture(frame,source,ordered_context);
    if(const auto retired=retire(frame)) return *retired;
    input=frame;output_blocked=false;map_blocked=false;
    pending_resource=staging;pending_stride=0;
    try {
      const D3D11_BOX box{0,0,0,width,height,1};
      const auto timing=copy_timing.begin(device.Get(),context.Get());
      context->CopySubresourceRegion(staging.Get(),0,0,0,0,source,0,&box);
      copy_timing.end(context.Get(),timing);
      mark_completion(ordered_context!=nullptr);
    } catch(...) {protect_failed_submission();throw;}
    submitted=true;return false;
  }
  PackedPreparation prepare_packed(Frame &frame, ID3D11DeviceContext *ordered_context, PackedWriter writer) {
    thread();
    if(!ordered_context || !writer.write)
      throw Error(NSS_E_ARGUMENT,NSS_STAGE_PROCESS,0,"Packed readback requires its producer context and writer");
    initialize(frame,ordered_context);
    if(!initialize_packed()) return {};
    if(const auto retired=retire(frame)) return {true,*retired,false};
    input=frame;output_blocked=false;map_blocked=false;
    pending_resource=packed_staging;pending_stride=packed_stride;
    try {
      writer.write(writer.state,packed_view.Get());
      const auto timing=copy_timing.begin(device.Get(),context.Get());
      context->CopyResource(packed_staging.Get(),packed.Get());
      copy_timing.end(context.Get(),timing);
      mark_completion(true);
    } catch(...) {protect_failed_submission();throw;}
    ++packed_copies;return {true,false,true};
  }
};
Readback::Readback(bool timing_diagnostic, bool prefer_query_completion)
    :impl_(std::make_unique<Impl>(timing_diagnostic,prefer_query_completion)) {}
Readback::~Readback()=default;
bool Readback::prepare(Frame &frame) {
  bool submitted{};
  return impl_->prepare(frame,nullptr,nullptr,submitted);
}
Readback::QueuedPreparation Readback::prepare_queued(Frame &frame,void *texture,void *immediate_context) {
  if(!immediate_context)
    throw Error(NSS_E_ARGUMENT,NSS_STAGE_PROCESS,0,"Queued readback requires a producer context");
  QueuedPreparation result;
  result.ready=impl_->prepare(frame,static_cast<ID3D11Texture2D *>(texture),
      static_cast<ID3D11DeviceContext *>(immediate_context),result.submitted);
  return result;
}
Readback::PackedPreparation Readback::prepare_packed_queued(Frame &frame,void *immediate_context,PackedWriter writer) {
  return impl_->prepare_packed(frame,static_cast<ID3D11DeviceContext *>(immediate_context),writer);
}
bool Readback::pending() const noexcept {return impl_->input.has_value();}
bool Readback::quiesce() {
  impl_->thread();
  if(!impl_->input) return true;
  // A producer may abandon a queued batch before flushing. Its pending event
  // and source lease must remain alive until the staging copy has completed.
  impl_->context->Flush();
  if(FAILED(impl_->device->GetDeviceRemovedReason()) || impl_->complete()) {
    impl_->input.reset();impl_->output_blocked=false;impl_->map_blocked=false;return true;
  }
  return false;
}
void *Readback::completion_event() const noexcept {
  // A completed GPU event cannot wake a consumer-held CPU lease or resolve a
  // nonblocking Map retry. Use the owner's bounded polling fallback instead
  // of repeatedly waking on that same signaled event.
  return pending()&&!impl_->pending_query&&!impl_->output_blocked&&!impl_->map_blocked?impl_->event:nullptr;
}
Json Readback::metrics() const {
  return {{"enabled",true},{"copies",impl_->copies},{"bytes",impl_->bytes},{"cacheHits",impl_->cache_hits},
    {"gpuTimingDiagnostic",impl_->copy_timing.metrics()},
    {"queryCompletionPreferred",impl_->prefer_query_completion},
    {"supersededCopies",impl_->superseded},{"completionPolls",impl_->polls},{"maximumPendingCopies",1},
    {"stagingBytes",(impl_->staging?uint64_t(impl_->width)*impl_->height*3/2:0)+
        (impl_->packed_staging?uint64_t(impl_->packed_stride)*(impl_->height+impl_->height/2):0)},
    {"pending",pending()},
    {"cpuOutputBufferLimit",Impl::output_limit},{"cpuOutputAllocations",impl_->output_allocations},
    {"cpuOutputReuses",impl_->output_reuses},{"cpuOutputPressure",impl_->output_pressure},
    {"cpuOutputPoolBytes",impl_->output_allocations*uint64_t(impl_->width)*impl_->height*3/2},
    {"cpuOutputBlocked",impl_->output_blocked},
    {"orderedCopies",impl_->ordered_copies},
    {"packedBufferCopies",impl_->packed_copies},{"packedBufferProbeResult",impl_->packed_result},
    {"packedBufferStride",impl_->packed_stride},
    {"packedBufferBytes",impl_->packed?uint64_t(impl_->packed_stride)*(impl_->height+impl_->height/2)*2:0},
    {"mapWouldBlockRetries",impl_->map_would_block},{"mapRetryPending",impl_->map_blocked},
    {"completion",impl_->fence?"D3D11 fence event":"D3D11 event query"}};
}
}
