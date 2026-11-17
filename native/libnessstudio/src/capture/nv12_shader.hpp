#pragma once
#include "capture.hpp"
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

namespace nss::capture {
// Original SDR conversion. The owner supplies an already cropped BGRA texture
// and an owned NV12 render target. No CPU readback or frame allocation occurs
// in draw(). Views and immutable geometry belong to one pool generation.
class Nv12Shader {
  friend class Nv12Compute;
  template <class T> using ComPtr = Microsoft::WRL::ComPtr<T>;
  ComPtr<ID3D11VertexShader> vertex_;
  ComPtr<ID3D11PixelShader> luma_, chroma_;
  ComPtr<ID3D11Buffer> constants_;
  ComPtr<ID3D11SamplerState> sampler_;
  ComPtr<ID3D11RasterizerState> raster_;
  ComPtr<ID3D11BlendState> blend_;
  ComPtr<ID3D11DepthStencilState> depth_;
  uint32_t width_{}, height_{};
  static constexpr char code_[] = R"hlsl(
cbuffer Geometry : register(b0) {
  float2 sourceSize; float2 textureSize; float4 viewport;
};
Texture2D<float4> sourceImage : register(t0);
SamplerState linearClamp : register(s0);
float4 vertex(uint index : SV_VertexID) : SV_Position {
  float2 corner = float2(index == 1 ? 3 : -1, index == 2 ? -3 : 1);
  return float4(corner, 0, 1);
}
float3 color(float2 center) {
  float2 local = center - viewport.xy;
  if (any(local < 0) || any(local >= viewport.zw)) return 0;
  float2 at = clamp(local * sourceSize / viewport.zw, .5, sourceSize - .5);
  float2 footprint = max(sourceSize / viewport.zw, 1);
  if (all(footprint <= 1))
    return sourceImage.SampleLevel(linearClamp, at / textureSize, 0).rgb;
  // A reducing bilinear filter widens its triangular support in source
  // pixels. Sampling only the central four texels aliases fine detail.
  // For at most 2:1 reduction, four taps per axis suffice; pair adjacent
  // weights into four hardware bilinear reads rather than sixteen loads.
  float2 pixel = at - .5;
  float2 base = floor(pixel) - 1;
  float4 x = max(1 - abs(base.x + float4(0, 1, 2, 3) - pixel.x) / footprint.x, 0);
  float4 y = max(1 - abs(base.y + float4(0, 1, 2, 3) - pixel.y) / footprint.y, 0);
  x /= dot(x, float4(1, 1, 1, 1)); y /= dot(y, float4(1, 1, 1, 1));
  float2 xw = float2(x.x + x.y, x.z + x.w);
  float2 yw = float2(y.x + y.y, y.z + y.w);
  float2 xs = base.x + float2(x.y / max(xw.x, .00001), 2 + x.w / max(xw.y, .00001)) + .5;
  float2 ys = base.y + float2(y.y / max(yw.x, .00001), 2 + y.w / max(yw.y, .00001)) + .5;
  xs = clamp(xs, .5, sourceSize.x - .5) / textureSize.x;
  ys = clamp(ys, .5, sourceSize.y - .5) / textureSize.y;
  return sourceImage.SampleLevel(linearClamp, float2(xs.x, ys.x), 0).rgb * xw.x * yw.x
       + sourceImage.SampleLevel(linearClamp, float2(xs.y, ys.x), 0).rgb * xw.y * yw.x
       + sourceImage.SampleLevel(linearClamp, float2(xs.x, ys.y), 0).rgb * xw.x * yw.y
       + sourceImage.SampleLevel(linearClamp, float2(xs.y, ys.y), 0).rgb * xw.y * yw.y;
}
float yPlane(float4 position : SV_Position) : SV_Target {
  return 16.0 / 255 + dot(color(position.xy), float3(.182586, .614231, .062007));
}
float2 uvPlane(float4 position : SV_Position) : SV_Target {
  float2 topLeft = position.xy * 2 - .5;
  float3 rgb = (color(topLeft) + color(topLeft + float2(1, 0)) +
                color(topLeft + float2(0, 1)) + color(topLeft + 1)) * .25;
  return 128.0 / 255 + float2(dot(rgb, float3(-.100644, -.338572, .439216)),
                             dot(rgb, float3(.439216, -.398942, -.040274)));
}
RWTexture2D<float> outputY : register(u0);
RWTexture2D<float2> outputUV : register(u1);
Buffer<float4> horizontalFilter : register(t1);
Buffer<float4> verticalFilter : register(t2);
RWBuffer<float4> horizontalFilterOutput : register(u2);
RWBuffer<float4> verticalFilterOutput : register(u3);
// Geometry is fixed for a pool generation. Keep the original triangular
// weights and paired bilinear positions, computed on the same GPU once.
float4 filterAxis(float center, float origin, float viewportSize,
                  float sourceExtent, float textureExtent) {
  float at = clamp((center - origin) * sourceExtent / viewportSize, .5, sourceExtent - .5);
  float footprint = max(sourceExtent / viewportSize, 1);
  float pixel = at - .5;
  float base = floor(pixel) - 1;
  float4 weights = max(1 - abs(base + float4(0, 1, 2, 3) - pixel) / footprint, 0);
  weights /= dot(weights, float4(1, 1, 1, 1));
  float2 paired = float2(weights.x + weights.y, weights.z + weights.w);
  float2 positions = base + float2(weights.y / max(paired.x, .00001),
                                  2 + weights.w / max(paired.y, .00001)) + .5;
  positions = clamp(positions, .5, sourceExtent - .5) / textureExtent;
  return float4(positions, paired);
}
[numthreads(64, 1, 1)]
void prepareFilters(uint3 dispatchId : SV_DispatchThreadID) {
  uint horizontalCount, verticalCount;
  horizontalFilterOutput.GetDimensions(horizontalCount);
  verticalFilterOutput.GetDimensions(verticalCount);
  if (dispatchId.x < horizontalCount)
    horizontalFilterOutput[dispatchId.x] = filterAxis(float(dispatchId.x) + .5,
        viewport.x, viewport.z, sourceSize.x, textureSize.x);
  if (dispatchId.x < verticalCount)
    verticalFilterOutput[dispatchId.x] = filterAxis(float(dispatchId.x) + .5,
        viewport.y, viewport.w, sourceSize.y, textureSize.y);
}
float3 filteredColor(float4 horizontal, float4 vertical) {
  return sourceImage.SampleLevel(linearClamp, float2(horizontal.x, vertical.x), 0).rgb * horizontal.z * vertical.z
       + sourceImage.SampleLevel(linearClamp, float2(horizontal.y, vertical.x), 0).rgb * horizontal.w * vertical.z
       + sourceImage.SampleLevel(linearClamp, float2(horizontal.x, vertical.y), 0).rgb * horizontal.z * vertical.w
       + sourceImage.SampleLevel(linearClamp, float2(horizontal.y, vertical.y), 0).rgb * horizontal.w * vertical.w;
}
float luma(float3 rgb) {
  return 16.0 / 255 + dot(rgb, float3(.182586, .614231, .062007));
}
[numthreads(8, 8, 1)]
void compute(uint3 dispatchId : SV_DispatchThreadID) {
  uint width, height; outputY.GetDimensions(width, height);
  uint2 at = dispatchId.xy * 2;
  if (at.x >= width || at.y >= height) return;
  float2 center = float2(at) + .5;
  float3 a = 0, b = 0, c = 0, d = 0;
  // The validated viewport is even: an entire 2x2 tile is inside or outside.
  float2 local = center - viewport.xy;
  if (all(local >= 0) && all(local < viewport.zw)) {
    if (any(sourceSize > viewport.zw)) {
      float4 x0 = horizontalFilter[at.x], x1 = horizontalFilter[at.x + 1];
      float4 y0 = verticalFilter[at.y], y1 = verticalFilter[at.y + 1];
      a = filteredColor(x0, y0); b = filteredColor(x1, y0);
      c = filteredColor(x0, y1); d = filteredColor(x1, y1);
    } else {
      a = color(center); b = color(center + float2(1, 0));
      c = color(center + float2(0, 1)); d = color(center + 1);
    }
  }
  outputY[at] = luma(a);
  outputY[at + uint2(1, 0)] = luma(b);
  outputY[at + uint2(0, 1)] = luma(c);
  outputY[at + 1] = luma(d);
  float3 rgb = (a + b + c + d) * .25;
  outputUV[dispatchId.xy] = 128.0 / 255 +
      float2(dot(rgb, float3(-.100644, -.338572, .439216)),
             dot(rgb, float3(.439216, -.398942, -.040274)));
}
// Four adjacent NV12 bytes per RGBA8_UNORM store. The hardware uses
// the same quantization as our planar NV12 views; no integer re-rounding.
cbuffer LinearOutput : register(b1) {
  uint outputWidth; uint outputHeight; uint outputStride; uint uvWordOffset;
};
RWBuffer<float4> outputBytes : register(u4);
float3 packedColor(uint2 at) {
  float2 center = float2(at) + .5;
  float2 local = center - viewport.xy;
  if (any(local < 0) || any(local >= viewport.zw)) return 0;
  if (any(sourceSize > viewport.zw))
    return filteredColor(horizontalFilter[at.x], verticalFilter[at.y]);
  return color(center);
}
float2 chromaBytes(float3 rgb) {
  float2 uv = 128.0 / 255 +
    float2(dot(rgb, float3(-.100644, -.338572, .439216)),
           dot(rgb, float3(.439216, -.398942, -.040274)));
  return uv;
}
groupshared float4 packedLuma[64];
groupshared float2 packedChroma[64];
[numthreads(16, 4, 1)]
void computePacked(uint3 id : SV_DispatchThreadID, uint lane : SV_GroupIndex) {
  uint2 at=id.xy*2;
  float2 center=float2(at)+.5;
  float2 local=center-viewport.xy;
  float3 a=0,b=0,c=0,d=0;
  // Every lane must reach the group barrier, including padded dispatch lanes.
  if (at.x<outputWidth && at.y<outputHeight && all(local>=0) && all(local<viewport.zw)) {
    if (any(sourceSize>viewport.zw)) {
      float4 x0=horizontalFilter[at.x],x1=horizontalFilter[at.x+1];
      float4 y0=verticalFilter[at.y],y1=verticalFilter[at.y+1];
      a=filteredColor(x0,y0);b=filteredColor(x1,y0);
      c=filteredColor(x0,y1);d=filteredColor(x1,y1);
    } else {
      a=color(center);b=color(center+float2(1,0));
      c=color(center+float2(0,1));d=color(center+1);
    }
  }
  packedLuma[lane]=float4(luma(a),luma(b),luma(c),luma(d));
  packedChroma[lane]=chromaBytes((a+b+c+d)*.25);
  GroupMemoryBarrierWithGroupSync();
  if ((lane&1)==0 && at.x<outputWidth && at.y<outputHeight) {
    float4 left=packedLuma[lane],right=packedLuma[lane+1];
    outputBytes[(at.y*outputStride+at.x)/4]=float4(left.xy,right.xy);
    outputBytes[((at.y+1)*outputStride+at.x)/4]=float4(left.zw,right.zw);
    outputBytes[uvWordOffset+(at.y/2*outputStride+at.x)/4]=float4(packedChroma[lane],packedChroma[lane+1]);
  }
}
)hlsl";
  static ComPtr<ID3DBlob> compile(const char *entry, const char *target) {
    ComPtr<ID3DBlob> result, diagnostics;
    checked(D3DCompile(code_, sizeof(code_) - 1, "NessStudio.SdrNv12", nullptr,
                       nullptr, entry, target,
                       D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3,
                       0, &result, &diagnostics), NSS_STAGE_PROCESS,
            "Native SDR conversion shader compilation failed");
    return result;
  }
public:
  struct Views {
    ComPtr<ID3D11ShaderResourceView> source;
    ComPtr<ID3D11RenderTargetView> y, uv;
  };
  Nv12Shader(ID3D11Device *device, uint32_t source_width, uint32_t source_height,
             uint32_t texture_width, uint32_t texture_height,
             uint32_t width, uint32_t height, Rect viewport)
      : width_(width), height_(height) {
    if (!device || !source_width || !source_height ||
        source_width > texture_width || source_height > texture_height ||
        !width || !height || ((width | height) & 1) ||
        viewport.x < 0 || viewport.y < 0 || viewport.width <= 0 || viewport.height <= 0 ||
        int64_t(viewport.x) + viewport.width > width ||
        int64_t(viewport.y) + viewport.height > height ||
        uint64_t(source_width) > uint64_t(viewport.width) * 2 ||
        uint64_t(source_height) > uint64_t(viewport.height) * 2 ||
        ((viewport.x | viewport.y | viewport.width | viewport.height) & 1))
      throw Error(NSS_E_ARGUMENT, NSS_STAGE_PROCESS, 0, "Invalid SDR shader geometry");
    const auto vs = compile("vertex", "vs_5_0"),
               ys = compile("yPlane", "ps_5_0"),
               uvs = compile("uvPlane", "ps_5_0");
    checked(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &vertex_),
            NSS_STAGE_PROCESS, "SDR vertex shader creation failed");
    checked(device->CreatePixelShader(ys->GetBufferPointer(), ys->GetBufferSize(), nullptr, &luma_),
            NSS_STAGE_PROCESS, "SDR luma shader creation failed");
    checked(device->CreatePixelShader(uvs->GetBufferPointer(), uvs->GetBufferSize(), nullptr, &chroma_),
            NSS_STAGE_PROCESS, "SDR chroma shader creation failed");
    const float geometry[] = {float(source_width), float(source_height),
      float(texture_width), float(texture_height), float(viewport.x), float(viewport.y),
      float(viewport.width), float(viewport.height)};
    D3D11_BUFFER_DESC buffer{};
    buffer.ByteWidth = sizeof(geometry);
    buffer.Usage = D3D11_USAGE_IMMUTABLE;
    buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    const D3D11_SUBRESOURCE_DATA initial{geometry, 0, 0};
    checked(device->CreateBuffer(&buffer, &initial, &constants_), NSS_STAGE_PROCESS,
            "SDR shader geometry allocation failed");
    D3D11_SAMPLER_DESC sampling{};
    sampling.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampling.AddressU = sampling.AddressV = sampling.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampling.MaxLOD = D3D11_FLOAT32_MAX;
    sampling.MaxAnisotropy = 1;
    sampling.ComparisonFunc = D3D11_COMPARISON_NEVER;
    checked(device->CreateSamplerState(&sampling, &sampler_), NSS_STAGE_PROCESS,
            "SDR shader sampler creation failed");
    D3D11_RASTERIZER_DESC raster{};
    raster.FillMode = D3D11_FILL_SOLID;
    raster.CullMode = D3D11_CULL_NONE;
    raster.DepthClipEnable = TRUE;
    checked(device->CreateRasterizerState(&raster, &raster_), NSS_STAGE_PROCESS,
            "SDR shader rasterizer creation failed");
    D3D11_BLEND_DESC blend{};
    blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    checked(device->CreateBlendState(&blend, &blend_), NSS_STAGE_PROCESS,
            "SDR shader blend state creation failed");
    D3D11_DEPTH_STENCIL_DESC depth{};
    depth.DepthFunc = D3D11_COMPARISON_ALWAYS;
    checked(device->CreateDepthStencilState(&depth, &depth_), NSS_STAGE_PROCESS,
            "SDR shader depth state creation failed");
  }
  static HRESULT views(ID3D11Device *device, ID3D11Texture2D *bgra,
                        ID3D11Texture2D *nv12, Views &output) noexcept {
    // Direct3D 11.1 selects the NV12 plane from R8/R8G8 view formats. Probe
    // actual views; interface/feature-level presence alone is insufficient.
    Views candidate;
    auto result = device->CreateShaderResourceView(bgra, nullptr, &candidate.source);
    D3D11_RENDER_TARGET_VIEW_DESC desc{};
    desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    desc.Format = DXGI_FORMAT_R8_UNORM;
    if (SUCCEEDED(result)) result = device->CreateRenderTargetView(nv12, &desc, &candidate.y);
    desc.Format = DXGI_FORMAT_R8G8_UNORM;
    if (SUCCEEDED(result)) result = device->CreateRenderTargetView(nv12, &desc, &candidate.uv);
    if (SUCCEEDED(result)) output = std::move(candidate);
    return result;
  }
  void draw(ID3D11DeviceContext *context, const Views &views) const {
    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(vertex_.Get(), nullptr, 0);
    context->GSSetShader(nullptr, nullptr, 0);
    context->HSSetShader(nullptr, nullptr, 0);
    context->DSSetShader(nullptr, nullptr, 0);
    context->RSSetState(raster_.Get());
    context->OMSetBlendState(blend_.Get(), nullptr, 0xffffffff);
    context->OMSetDepthStencilState(depth_.Get(), 0);
    context->PSSetConstantBuffers(0, 1, constants_.GetAddressOf());
    context->PSSetSamplers(0, 1, sampler_.GetAddressOf());
    context->PSSetShaderResources(0, 1, views.source.GetAddressOf());
    D3D11_VIEWPORT viewport{0, 0, float(width_), float(height_), 0, 1};
    context->RSSetViewports(1, &viewport);
    context->PSSetShader(luma_.Get(), nullptr, 0);
    context->OMSetRenderTargets(1, views.y.GetAddressOf(), nullptr);
    context->Draw(3, 0);
    viewport.Width /= 2;
    viewport.Height /= 2;
    context->RSSetViewports(1, &viewport);
    context->PSSetShader(chroma_.Get(), nullptr, 0);
    context->OMSetRenderTargets(1, views.uv.GetAddressOf(), nullptr);
    context->Draw(3, 0);
    // Do not keep textures alive through immediate-context bindings or leave
    // SRV/write hazards for subsequent capture/preview operations.
    ID3D11ShaderResourceView *empty{};
    context->PSSetShaderResources(0, 1, &empty);
    context->OMSetRenderTargets(0, nullptr, nullptr);
  }
};
} // namespace nss::capture
