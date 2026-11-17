#pragma once
#include "scene.hpp"
#include <cstring>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

namespace nss::scene {
// Original two-layer compositor. Each thread owns one output pixel, so the
// complete ordered blend needs no UAV loads, atomics or intermediate texture.
class ComputeComposition {
  template<class T> using ComPtr = Microsoft::WRL::ComPtr<T>;
  ComPtr<ID3D11ComputeShader> shader_;
  ComPtr<ID3D11Buffer> constants_;
  static void checked(HRESULT result, const char* operation) {
    capture::checked(result, NSS_STAGE_PROCESS, operation);
  }
  static constexpr char code_[] = R"hlsl(
struct LayerState {
  float4 crop;
  float2 dimensions; uint rotation; uint mirror;
  float opacity; uint alphaMode; uint source; uint reserved;
  float4 destination;
};
cbuffer SceneState : register(b0) {
  float4 background; uint2 outputSize; uint layerCount; uint unused;
  LayerState layers[2];
};
Texture2D<float4> firstSource : register(t0);
Texture2D<float4> secondSource : register(t1);
RWTexture2D<float4> outputImage : register(u0);
float4 pixel(LayerState layer, int2 position) {
  position = clamp(position, int2(0, 0), int2(layer.dimensions) - 1);
  float4 value;
  if (layer.source == 1) value = firstSource.Load(int3(position, 0));
  else value = secondSource.Load(int3(position, 0));
  if (layer.alphaMode == 0) value.a = 1;
  else if (layer.alphaMode == 1) value.rgb *= value.a;
  return value;
}
float4 sampleLayer(LayerState layer, float2 center) {
  float2 uv = (center - layer.destination.xy) / layer.destination.zw;
  if (layer.mirror != 0) uv.x = 1 - uv.x;
  if (layer.rotation == 90) uv = float2(uv.y, 1 - uv.x);
  else if (layer.rotation == 180) uv = 1 - uv;
  else if (layer.rotation == 270) uv = float2(1 - uv.y, uv.x);
  float2 at = (layer.crop.xy + uv * layer.crop.zw) * layer.dimensions - .5;
  at = clamp(at, 0, layer.dimensions - 1);
  int2 base = int2(floor(at)); float2 fraction = frac(at);
  return lerp(lerp(pixel(layer, base), pixel(layer, base + int2(1, 0)), fraction.x),
              lerp(pixel(layer, base + int2(0, 1)), pixel(layer, base + 1), fraction.x),
              fraction.y) * layer.opacity;
}
[numthreads(16, 16, 1)]
void compose(uint3 dispatchId : SV_DispatchThreadID) {
  if (any(dispatchId.xy >= outputSize)) return;
  float2 center = float2(dispatchId.xy) + .5;
  float4 color = background;
  for (uint index = 0; index < layerCount; ++index) {
    LayerState layer = layers[index];
    if (any(center < layer.destination.xy) ||
        any(center >= layer.destination.xy + layer.destination.zw)) continue;
    float4 foreground = sampleLayer(layer, center);
    // The raster path stores to an 8-bit UNORM target after every layer.
    // Preserve that intermediate quantization instead of accumulating an
    // unbounded-precision blend and quantizing only the final result.
    color = round(saturate(foreground + color * (1 - foreground.a)) * 255) / 255;
  }
  outputImage[dispatchId.xy] = color;
}
)hlsl";
public:
  struct LayerState {
    float crop[4], dimensions[2];
    uint32_t rotation{}, mirror{};
    float opacity{};
    uint32_t alpha{}, source{}, reserved{};
    float destination[4];
  };
  struct State {
    float background[4];
    uint32_t width{}, height{}, count{}, reserved{};
    std::array<LayerState, 2> layers{};
  };
  static_assert(sizeof(LayerState) == 64);
  static_assert(sizeof(State) == 160);
  explicit ComputeComposition(ID3D11Device* device) {
    ComPtr<ID3DBlob> code, diagnostics;
    checked(D3DCompile(code_, sizeof(code_) - 1, "NessStudio.Scene.Compute", nullptr,
        nullptr, "compose", "cs_5_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, &code, &diagnostics),
        "Scene compute compilation failed");
    checked(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(),
        nullptr, &shader_), "Scene compute shader creation failed");
    D3D11_BUFFER_DESC description{};
    description.ByteWidth = sizeof(State);
    description.Usage = D3D11_USAGE_DYNAMIC;
    description.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    checked(device->CreateBuffer(&description, nullptr, &constants_), "Scene compute constants allocation failed");
  }
  void dispatch(ID3D11DeviceContext* context, const State& state,
                 const std::array<ID3D11ShaderResourceView*, 2>& inputs,
                 ID3D11UnorderedAccessView* target) const {
    if (state.count > state.layers.size() || !state.width || !state.height)
      throw capture::Error(NSS_E_ARGUMENT, NSS_STAGE_PROCESS, 0, "Invalid compute scene bounds");
    D3D11_MAPPED_SUBRESOURCE mapped{};
    checked(context->Map(constants_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped),
            "Scene compute constants update failed");
    std::memcpy(mapped.pData, &state, sizeof(state));
    context->Unmap(constants_.Get(), 0);
    context->CSSetShader(shader_.Get(), nullptr, 0);
    context->CSSetConstantBuffers(0, 1, constants_.GetAddressOf());
    context->CSSetShaderResources(0, static_cast<UINT>(inputs.size()), inputs.data());
    context->CSSetUnorderedAccessViews(0, 1, &target, nullptr);
    context->Dispatch((state.width + 15) / 16, (state.height + 15) / 16, 1);
    ID3D11ShaderResourceView* empty[2]{};
    ID3D11UnorderedAccessView* no_target{};
    context->CSSetShaderResources(0, 2, empty);
    context->CSSetUnorderedAccessViews(0, 1, &no_target, nullptr);
  }
};
} // namespace nss::scene
