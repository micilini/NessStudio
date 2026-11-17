#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "scene.hpp"
#include "compute.hpp"
#include <chrono>
#include <cstring>
#include <d3d11_4.h>
#include <d3dcompiler.h>
#include <deque>
#include <dxgi1_6.h>
#include <thread>
#include <windows.h>
#include <wrl/client.h>

namespace nss::scene {
namespace {
using Microsoft::WRL::ComPtr;
void checked(HRESULT result, const char *message) {
  capture::checked(result, NSS_STAGE_PROCESS, message);
}
std::atomic<uint64_t> generations{};
constexpr char shader[] = R"hlsl(
cbuffer LayerState : register(b0) { float4 crop; float2 dimensions; uint rotation; uint mirror; float opacity; uint alphaMode; float2 padding; };
Texture2D<float4> image : register(t0);
struct Vertex { float4 position : SV_Position; float2 uv : TEXCOORD0; };
Vertex vs(uint id:SV_VertexID) { Vertex o; o.uv=float2((id<<1)&2,id&2);o.position=float4(o.uv*float2(2,-2)+float2(-1,1),0,1);return o; }
float4 pixel(int2 position) { position=clamp(position,int2(0,0),int2(dimensions)-1); float4 p=image.Load(int3(position,0)); if(alphaMode==0)p.a=1; else if(alphaMode==1)p.rgb*=p.a; return p; }
float4 ps(Vertex input):SV_Target {
 float2 uv=input.uv; if(mirror!=0)uv.x=1-uv.x;
 if(rotation==90)uv=float2(uv.y,1-uv.x);else if(rotation==180)uv=1-uv;else if(rotation==270)uv=float2(1-uv.y,uv.x);
 float2 at=(crop.xy+uv*crop.zw)*dimensions-.5; at=clamp(at,0,dimensions-1);int2 base=int2(floor(at));float2 f=frac(at);
 float4 p=lerp(lerp(pixel(base),pixel(base+int2(1,0)),f.x),lerp(pixel(base+int2(0,1)),pixel(base+1),f.x),f.y);
 return p*opacity;
}
)hlsl";
struct Constants {
  float crop[4], dimensions[2];
  uint32_t rotation, mirror;
  float opacity;
  uint32_t alpha;
  float padding[2]{};
};
static_assert(sizeof(Constants) == 48);
struct Slot {
  std::atomic<bool> used{};
  ComPtr<ID3D11Texture2D> bgra, nv12;
  ComPtr<ID3D11RenderTargetView> target;
  ComPtr<ID3D11UnorderedAccessView> compute_target;
  ComPtr<ID3D11VideoProcessorInputView> input;
  ComPtr<ID3D11VideoProcessorOutputView> output;
  ComPtr<ID3D11Query> done;
  uint64_t bytes{};
  ~Slot() { live_bytes -= bytes; }
};
struct Surface final : capture::Surface {
  std::shared_ptr<Slot> slot;
  ComPtr<ID3D11Device> device;
  uint64_t luid;
  Surface(std::shared_ptr<Slot> value, ID3D11Device *owner, uint64_t adapter)
      : slot(std::move(value)), device(owner), luid(adapter) {}
  ~Surface() override { slot->used = false; }
  void *gpu_texture() const noexcept override { return slot->nv12.Get(); }
  void *gpu_device() const noexcept override { return device.Get(); }
  uint64_t adapter_luid() const noexcept override { return luid; }
};
struct Converter {
  uint32_t width{}, height{};
  capture::Frame::Color color{capture::Frame::Color::Bt709};
  ComPtr<ID3D11Texture2D> texture;
  ComPtr<ID3D11ShaderResourceView> shader_view;
  ComPtr<ID3D11VideoProcessorEnumerator> enumeration;
  ComPtr<ID3D11VideoProcessor> processor;
  ComPtr<ID3D11VideoProcessorOutputView> output;
  uint64_t bytes{};
  ~Converter() { live_bytes -= bytes; }
};
class Gpu final : public Renderer {
  uint32_t width_, height_;
  uint64_t luid_, generation_{++generations}, frames_{}, pressure_{},
      revision_{}, constant_bytes_{};
  ComPtr<ID3D11Device> device_;
  ComPtr<ID3D11DeviceContext> context_;
  ComPtr<ID3D11VideoDevice> video_;
  ComPtr<ID3D11VideoContext1> video_context_;
  ComPtr<ID3D11VertexShader> vertex_;
  ComPtr<ID3D11PixelShader> pixel_;
  std::unique_ptr<ComputeComposition> compute_;
  bool compute_selected_{};
  HRESULT compute_probe_result_{E_NOTIMPL};
  ComPtr<ID3D11Buffer> constants_;
  ComPtr<ID3D11BlendState> blend_;
  ComPtr<ID3D11RasterizerState> raster_;
  ComPtr<ID3D11VideoProcessorEnumerator> enumeration_;
  ComPtr<ID3D11VideoProcessor> processor_;
  std::array<std::unique_ptr<Converter>, 3> converters_;
  std::vector<std::shared_ptr<Slot>> slots_;
  struct Pending {
    std::shared_ptr<Slot> slot;
    capture::Frame frame;
    std::vector<std::shared_ptr<const capture::Surface>> sources;
    std::vector<ComPtr<ID3D11VideoProcessorInputView>> views;
    std::vector<ComPtr<ID3D11ShaderResourceView>> direct;
  };
  std::deque<Pending> pending_;
  bool closed_{};
  void raster() {
    ComPtr<ID3DBlob> vs, ps, errors;
    checked(D3DCompile(shader, sizeof(shader) - 1, "NessStudio.Scene", nullptr,
                       nullptr, "vs", "vs_5_0", D3DCOMPILE_ENABLE_STRICTNESS, 0,
                       &vs, &errors),
            "Scene vertex shader compilation failed");
    checked(D3DCompile(shader, sizeof(shader) - 1, "NessStudio.Scene", nullptr,
                       nullptr, "ps", "ps_5_0", D3DCOMPILE_ENABLE_STRICTNESS, 0,
                       &ps, &errors),
            "Scene pixel shader compilation failed");
    checked(device_->CreateVertexShader(vs->GetBufferPointer(),
                                        vs->GetBufferSize(), nullptr, &vertex_),
            "Scene vertex shader creation failed");
    checked(device_->CreatePixelShader(ps->GetBufferPointer(),
                                       ps->GetBufferSize(), nullptr, &pixel_),
            "Scene pixel shader creation failed");
    D3D11_BUFFER_DESC buffer{};
    buffer.ByteWidth = sizeof(Constants);
    buffer.Usage = D3D11_USAGE_DYNAMIC;
    buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    buffer.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    checked(device_->CreateBuffer(&buffer, nullptr, &constants_),
            "Scene constant buffer creation failed");
    D3D11_BLEND_DESC blend{};
    auto &target = blend.RenderTarget[0];
    target.BlendEnable = TRUE;
    target.SrcBlend = D3D11_BLEND_ONE;
    target.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    target.BlendOp = D3D11_BLEND_OP_ADD;
    target.SrcBlendAlpha = D3D11_BLEND_ONE;
    target.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    target.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    target.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    checked(device_->CreateBlendState(&blend, &blend_),
            "Scene premultiplied blend state failed");
    D3D11_RASTERIZER_DESC raster{};
    raster.FillMode = D3D11_FILL_SOLID;
    raster.CullMode = D3D11_CULL_NONE;
    raster.DepthClipEnable = TRUE;
    checked(device_->CreateRasterizerState(&raster, &raster_),
            "Scene rasterizer creation failed");
  }
  void processor(uint32_t width, uint32_t height,
                 ComPtr<ID3D11VideoProcessorEnumerator> &enumeration,
                 ComPtr<ID3D11VideoProcessor> &output) {
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC desc{};
    desc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    desc.InputFrameRate = {30, 1};
    desc.OutputFrameRate = {30, 1};
    desc.InputWidth = width;
    desc.InputHeight = height;
    desc.OutputWidth = width;
    desc.OutputHeight = height;
    desc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
    checked(video_->CreateVideoProcessorEnumerator(&desc, &enumeration),
            "Scene video processor enumeration failed");
    checked(video_->CreateVideoProcessor(enumeration.Get(), 0, &output),
            "Scene video processor creation failed");
    video_context_->VideoProcessorSetStreamFrameFormat(
        output.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    video_context_->VideoProcessorSetStreamAutoProcessingMode(output.Get(), 0,
                                                              FALSE);
    video_context_->VideoProcessorSetOutputAlphaFillMode(
        output.Get(), D3D11_VIDEO_PROCESSOR_ALPHA_FILL_MODE_OPAQUE, 0);
    RECT area{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    video_context_->VideoProcessorSetStreamSourceRect(output.Get(), 0, TRUE,
                                                      &area);
    video_context_->VideoProcessorSetStreamDestRect(output.Get(), 0, TRUE,
                                                    &area);
    video_context_->VideoProcessorSetOutputTargetRect(output.Get(), TRUE,
                                                      &area);
  }
  ID3D11ShaderResourceView *input(const Input &input, Pending &work) {
    const auto &frame = input.frame;
    if (frame.surface->gpu_device() != device_.Get() ||
        frame.surface->adapter_luid() != luid_)
      throw capture::Error(
          NSS_E_UNAVAILABLE, NSS_STAGE_PROCESS, 0,
          "Scene forbids implicit cross-device/cross-adapter texture copies");
    work.sources.push_back(frame.surface);
    if (frame.format == capture::Frame::Format::Bgra) {
      ComPtr<ID3D11ShaderResourceView> view;
      checked(device_->CreateShaderResourceView(
                  static_cast<ID3D11Texture2D *>(frame.surface->gpu_texture()),
                  nullptr, &view),
              "Scene BGRA shader view failed");
      work.direct.push_back(view);
      return view.Get();
    }
    auto &cached = converters_[input.source];
    if (!cached || cached->width != frame.width ||
        cached->height != frame.height || cached->color != frame.color) {
      auto value = std::make_unique<Converter>();
      value->width = frame.width;
      value->height = frame.height;
      value->color = frame.color;
      processor(frame.width, frame.height, value->enumeration,
                value->processor);
      D3D11_TEXTURE2D_DESC desc{};
      desc.Width = frame.width;
      desc.Height = frame.height;
      desc.MipLevels = desc.ArraySize = 1;
      desc.SampleDesc.Count = 1;
      desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
      desc.Usage = D3D11_USAGE_DEFAULT;
      desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
      checked(device_->CreateTexture2D(&desc, nullptr, &value->texture),
              "Scene GPU input conversion texture failed");
      checked(device_->CreateShaderResourceView(value->texture.Get(), nullptr,
                                                &value->shader_view),
              "Scene GPU input shader view failed");
      D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC output{};
      output.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
      checked(video_->CreateVideoProcessorOutputView(value->texture.Get(),
                                                     value->enumeration.Get(),
                                                     &output, &value->output),
              "Scene RGB conversion target failed");
      video_context_->VideoProcessorSetStreamColorSpace1(
          value->processor.Get(), 0,
          frame.color == capture::Frame::Color::Bt601
              ? DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P601
              : DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709);
      video_context_->VideoProcessorSetOutputColorSpace1(
          value->processor.Get(), DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
      video_context_->VideoProcessorSetOutputShaderUsage(value->processor.Get(),
                                                         TRUE);
      value->bytes = uint64_t(frame.width) * frame.height * 4;
      live_bytes += value->bytes;
      cached = std::move(value);
    }
    ComPtr<ID3D11VideoProcessorInputView> view;
    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC desc{};
    desc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    checked(video_->CreateVideoProcessorInputView(
                static_cast<ID3D11Texture2D *>(frame.surface->gpu_texture()),
                cached->enumeration.Get(), &desc, &view),
            "Scene NV12 input view failed");
    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.pInputSurface = view.Get();
    checked(video_context_->VideoProcessorBlt(
                cached->processor.Get(), cached->output.Get(), 0, 1, &stream),
            "Scene NV12 to RGB conversion failed");
    work.views.push_back(view);
    return cached->shader_view.Get();
  }
  void compose(const Snapshot& scene, std::span<const Input> sources,
                Pending& work, ID3D11UnorderedAccessView* target) {
    ComputeComposition::State state{};
    if (scene.layers.size() > state.layers.size())
      throw capture::Error(NSS_E_ARGUMENT, NSS_STAGE_PROCESS, 0, "Compute scene exceeds its layer budget");
    state.width = width_; state.height = height_;
    for (size_t channel = 0; channel < 3; ++channel)
      state.background[channel] = scene.background[channel] / 255.0f;
    state.background[3] = 1;
    std::array<ID3D11ShaderResourceView*, 2> views{};
    for (const auto& layer : scene.layers) {
      if (!layer.visible || layer.opacity == 0) continue;
      if (layer.source < 1 || layer.source > views.size())
        throw capture::Error(NSS_E_ARGUMENT, NSS_STAGE_PROCESS, 0, "Invalid compute scene source");
      const auto found = std::find_if(sources.begin(), sources.end(),
          [&](const auto& source) { return source.source == layer.source; });
      if (found == sources.end()) continue;
      auto& view = views[static_cast<size_t>(layer.source - 1)];
      if (!view) view = input(*found, work);
      const auto placement = place(scene, layer, found->frame.width, found->frame.height, found->frame.rotation);
      const auto& destination = placement.destination;
      auto& value = state.layers[state.count++];
      value = {{float(placement.crop.x), float(placement.crop.y), float(placement.crop.width), float(placement.crop.height)},
               {float(found->frame.width), float(found->frame.height)}, placement.rotation, placement.mirror ? 1U : 0U,
               float(layer.opacity), found->frame.format == capture::Frame::Format::Nv12 ? 0U : uint32_t(found->frame.alpha),
               static_cast<uint32_t>(layer.source), 0,
               {float(destination.x), float(destination.y), float(destination.width), float(destination.height)}};
    }
    compute_->dispatch(context_.Get(), state, views, target);
    constant_bytes_ += sizeof(state);
  }
  std::optional<capture::Frame> complete() {
    if (pending_.empty())
      return {};
    auto &first = pending_.front();
    BOOL done{};
    const auto result =
        context_->GetData(first.slot->done.Get(), &done, sizeof(done),
                          D3D11_ASYNC_GETDATA_DONOTFLUSH);
    checked(result, "Scene GPU completion query failed");
    if (result == S_FALSE || !done)
      return {};
    auto output = std::move(first.frame);
    output.end_hns = qpc_hns();
    pending_.pop_front();
    return output;
  }

public:
  Gpu(const Snapshot &scene, const capture::Frame &reference, GpuKernel kernel)
      : width_(scene.width), height_(scene.height),
        luid_(reference.surface->adapter_luid()),
        device_(static_cast<ID3D11Device *>(reference.surface->gpu_device())) {
    if (!device_)
      throw capture::Error(NSS_E_ARGUMENT, NSS_STAGE_PROCESS, 0,
                           "Scene GPU reference is not a device texture");
    device_->GetImmediateContext(&context_);
    checked(device_.As(&video_), "Scene D3D11 video device unavailable");
    checked(context_.As(&video_context_),
            "Scene D3D11 video context unavailable");
    processor(width_, height_, enumeration_, processor_);
    video_context_->VideoProcessorSetStreamColorSpace1(
        processor_.Get(), 0, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
    video_context_->VideoProcessorSetOutputColorSpace1(
        processor_.Get(), DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709);
    if (kernel != GpuKernel::Automatic && kernel != GpuKernel::Raster && kernel != GpuKernel::Compute)
      throw capture::Error(NSS_E_ARGUMENT, NSS_STAGE_PROCESS, 0, "Unknown GPU scene kernel");
    bool use_compute = kernel != GpuKernel::Raster;
    for (size_t index = 0; index < 6; ++index) {
      auto slot = std::make_shared<Slot>();
      D3D11_TEXTURE2D_DESC desc{};
      desc.Width = width_;
      desc.Height = height_;
      desc.MipLevels = desc.ArraySize = 1;
      desc.SampleDesc.Count = 1;
      desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
      desc.Usage = D3D11_USAGE_DEFAULT;
      desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE |
          (use_compute ? D3D11_BIND_UNORDERED_ACCESS : 0);
      auto result = device_->CreateTexture2D(&desc, nullptr, &slot->bgra);
      if (use_compute) {
        if (SUCCEEDED(result)) result = device_->CreateUnorderedAccessView(slot->bgra.Get(), nullptr, &slot->compute_target);
        compute_probe_result_ = result;
        if (FAILED(result)) {
          checked(device_->GetDeviceRemovedReason(), "Scene device removed during compute capability probe");
          if (kernel == GpuKernel::Compute ||
              (result != E_INVALIDARG && result != E_NOINTERFACE && result != DXGI_ERROR_UNSUPPORTED))
            checked(result, "Scene compute target unavailable");
          slot->compute_target.Reset(); slot->bgra.Reset();
          use_compute = false;
          desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
          result = device_->CreateTexture2D(&desc, nullptr, &slot->bgra);
        }
      }
      checked(result, "Scene BGRA output pool allocation failed");
      checked(device_->CreateRenderTargetView(slot->bgra.Get(), nullptr,
                                              &slot->target),
              "Scene render target view failed");
      desc.Format = DXGI_FORMAT_NV12;
      desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
      checked(device_->CreateTexture2D(&desc, nullptr, &slot->nv12),
              "Scene NV12 output pool allocation failed");
      D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC input{};
      input.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
      checked(video_->CreateVideoProcessorInputView(
                  slot->bgra.Get(), enumeration_.Get(), &input, &slot->input),
              "Scene program color input failed");
      D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC output{};
      output.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
      checked(video_->CreateVideoProcessorOutputView(
                  slot->nv12.Get(), enumeration_.Get(), &output, &slot->output),
              "Scene program NV12 target failed");
      D3D11_QUERY_DESC query{D3D11_QUERY_EVENT, 0};
      checked(device_->CreateQuery(&query, &slot->done),
              "Scene completion query allocation failed");
      slot->bytes = uint64_t(width_) * height_ * 11 / 2;
      live_bytes += slot->bytes;
      slots_.push_back(std::move(slot));
    }
    compute_selected_ = use_compute;
    if (use_compute) compute_ = std::make_unique<ComputeComposition>(device_.Get());
    else {
      for (auto& slot : slots_) slot->compute_target.Reset();
      raster();
    }
  }
  ~Gpu() override {
    try {
      close();
    } catch (...) {
    }
  }
  std::optional<capture::Frame>
  render(const Snapshot &scene, std::span<const Input> sources,
         std::optional<int64_t> presentation) override {
    if (closed_)
      return {};
    checked(device_->GetDeviceRemovedReason(), "Scene GPU device removed");
    auto ready = complete();
    std::shared_ptr<Slot> slot;
    for (auto &candidate : slots_) {
      bool expected = false;
      if (candidate->used.compare_exchange_strong(expected, true)) {
        slot = candidate;
        break;
      }
    }
    if (!slot) {
      ++pressure_;
      return ready;
    }
    Pending work;
    work.slot = slot;
    const auto begin = qpc_hns();
    try {
      if (compute_) compose(scene, sources, work, slot->compute_target.Get());
      else {
        const float background[]{scene.background[0] / 255.0f,
                                 scene.background[1] / 255.0f,
                                 scene.background[2] / 255.0f, 1};
        context_->ClearRenderTargetView(slot->target.Get(), background);
        context_->IASetInputLayout(nullptr);
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context_->VSSetShader(vertex_.Get(), nullptr, 0);
        context_->PSSetShader(pixel_.Get(), nullptr, 0);
        context_->RSSetState(raster_.Get());
        const float factor[4]{};
        context_->OMSetBlendState(blend_.Get(), factor, UINT_MAX);
        auto *render_target = slot->target.Get();
        context_->OMSetRenderTargets(1, &render_target, nullptr);
        for (const auto &layer : scene.layers) {
          if (!layer.visible || layer.opacity == 0)
            continue;
          const auto found = std::find_if(
              sources.begin(), sources.end(),
              [&](const auto &source) { return source.source == layer.source; });
          if (found == sources.end())
            continue;
          auto *view = input(*found, work);
          const auto placement =
              place(scene, layer, found->frame.width, found->frame.height,
                    found->frame.rotation);
          const auto &dest = placement.destination;
          Constants value{{static_cast<float>(placement.crop.x),
                           static_cast<float>(placement.crop.y),
                           static_cast<float>(placement.crop.width),
                           static_cast<float>(placement.crop.height)},
                          {static_cast<float>(found->frame.width),
                           static_cast<float>(found->frame.height)},
                          placement.rotation,
                          placement.mirror ? 1U : 0U,
                          static_cast<float>(layer.opacity),
                          found->frame.format == capture::Frame::Format::Nv12
                              ? 0U
                              : static_cast<uint32_t>(found->frame.alpha)};
          D3D11_MAPPED_SUBRESOURCE mapping{};
          checked(context_->Map(constants_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0,
                                &mapping),
                  "Scene constants update failed");
          std::memcpy(mapping.pData, &value, sizeof(value));
          context_->Unmap(constants_.Get(), 0);
          constant_bytes_ += sizeof(value);
          auto *constants = constants_.Get();
          context_->PSSetConstantBuffers(0, 1, &constants);
          context_->PSSetShaderResources(0, 1, &view);
          D3D11_VIEWPORT viewport{static_cast<float>(dest.x),
                                  static_cast<float>(dest.y),
                                  static_cast<float>(dest.width),
                                  static_cast<float>(dest.height),
                                  0,
                                  1};
          context_->RSSetViewports(1, &viewport);
          context_->Draw(3, 0);
          ID3D11ShaderResourceView *clear = nullptr;
          context_->PSSetShaderResources(0, 1, &clear);
        }
        context_->OMSetRenderTargets(0, nullptr, nullptr);
      }
      D3D11_VIDEO_PROCESSOR_STREAM stream{};
      stream.Enable = TRUE;
      stream.pInputSurface = slot->input.Get();
      checked(video_context_->VideoProcessorBlt(
                  processor_.Get(), slot->output.Get(), 0, 1, &stream),
              "Scene final RGB to NV12 conversion failed");
      context_->End(slot->done.Get());
      context_->Flush();
      work.frame = {std::make_shared<Surface>(slot, device_.Get(), luid_),
                    ++frames_,
                    generation_,
                    begin,
                    begin,
                    begin,
                    width_,
                    height_,
                    width_,
                    true};
      work.frame.presentation_hns = presentation;
      pending_.push_back(std::move(work));
      revision_ = scene.revision;
    } catch (...) {
      // Commands already queued can still reference source textures. Retain
      // those leases behind the completion query even on a failed render.
      context_->End(slot->done.Get());
      context_->Flush();
      work.frame = {std::make_shared<Surface>(slot, device_.Get(), luid_)};
      pending_.push_back(std::move(work));
      throw;
    }
    return ready;
  }
  std::optional<capture::Frame> take_ready() override { return complete(); }
  void close() override {
    if (closed_)
      return;
    closed_ = true;
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!pending_.empty()) {
      (void)complete();
      if (std::chrono::steady_clock::now() > end)
        throw capture::Error(NSS_E_TIMEOUT, NSS_STAGE_PROCESS, 0,
                             "Scene GPU work did not drain");
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    // All queued work is complete. Unbind, release the renderer's references,
    // then flush D3D11's deferred destruction while the context is still alive.
    // Output leases retain their own shared slots/device independently.
    context_->ClearState();
    converters_ = {};
    slots_.clear();
    processor_.Reset();
    enumeration_.Reset();
    raster_.Reset();
    blend_.Reset();
    constants_.Reset();
    compute_.reset();
    pixel_.Reset();
    vertex_.Reset();
    video_context_.Reset();
    video_.Reset();
    context_->Flush();
  }
  Json metrics() const override {
    uint64_t pool{}, scratch{};
    for (const auto &slot : slots_)
      pool += slot->bytes;
    for (const auto &source : converters_)
      if (source)
        scratch += source->bytes;
    return {{"implementationId",
             "D3D11 shared scene premultiplied shader blend to NV12"},
            {"compositionKernel", compute_selected_ ? "compute" : "raster"},
            {"computeCompositionSelected", compute_selected_},
            {"computePlaneViewProbeResult", compute_probe_result_},
            {"hardware", true},
            {"software", false},
            {"adapterLuid", luid_},
            {"frames", frames_},
            {"appliedRevision", revision_},
            {"poolPressure", pressure_},
            {"poolSlots", 6},
            {"poolBytes", pool},
            {"scratchBytes", scratch},
            {"pendingGpuFrames", pending_.size()},
            {"shaderConstantUploadBytes", constant_bytes_},
            {"gpuToCpuReadbackBytes", 0},
            {"sourceCaptureOpens", 0}};
  }
};
} // namespace
std::unique_ptr<Renderer> make_gpu(const Snapshot &scene,
                                   const capture::Frame &reference, GpuKernel kernel) {
  return std::make_unique<Gpu>(scene, reference, kernel);
}
} // namespace nss::scene
