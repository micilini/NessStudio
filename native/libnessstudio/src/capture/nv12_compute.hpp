#pragma once
#include "nv12_shader.hpp"
#include <algorithm>
#include <d3d11_3.h>

namespace nss::capture {
// Same owned input, triangular filter and NV12 format as Nv12Shader.
// One thread owns a complete 2x2 block and reuses its RGB values for both
// planes. All views are probed before selection; no per-frame allocation.
class Nv12Compute {
  template<class T> using ComPtr = Microsoft::WRL::ComPtr<T>;
  ComPtr<ID3D11ComputeShader> shader_, packed_shader_;
  ComPtr<ID3D11Buffer> packed_geometry_;
  ComPtr<ID3D11ComputeShader> filter_shader_;
  ComPtr<ID3D11Buffer> constants_;
  ComPtr<ID3D11SamplerState> sampler_;
  struct Filter {
    ComPtr<ID3D11ShaderResourceView> read;
    ComPtr<ID3D11UnorderedAccessView> write;
  } horizontal_, vertical_;
  bool filters_ready_{};
  uint32_t width_, height_;
  void prepare_filters(ID3D11DeviceContext* context) {
    if (filter_shader_ && !filters_ready_) {
      context->CSSetShader(filter_shader_.Get(), nullptr, 0);
      context->CSSetConstantBuffers(0, 1, constants_.GetAddressOf());
      ID3D11UnorderedAccessView* filters[]{horizontal_.write.Get(), vertical_.write.Get()};
      context->CSSetUnorderedAccessViews(2, 2, filters, nullptr);
      context->Dispatch(((std::max)(width_, height_) + 63) / 64, 1, 1);
      ID3D11UnorderedAccessView* no_filters[2]{};
      context->CSSetUnorderedAccessViews(2, 2, no_filters, nullptr);
      // Later reads are ordered on this same owner/immediate context. This
      // only means initialization was queued; the caller still fences NV12.
      filters_ready_ = true;
    }
  }
public:
  struct Views {
    ComPtr<ID3D11ShaderResourceView> source;
    ComPtr<ID3D11UnorderedAccessView> y, uv;
  };
  Nv12Compute(ID3D11Device* device, uint32_t source_width, uint32_t source_height,
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
      throw Error(NSS_E_ARGUMENT, NSS_STAGE_PROCESS, 0, "Invalid SDR compute geometry");
    const auto code = Nv12Shader::compile("compute", "cs_5_0");
    checked(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &shader_),
            NSS_STAGE_PROCESS, "SDR compute shader creation failed");
    const float geometry[] = {float(source_width), float(source_height),
      float(texture_width), float(texture_height), float(viewport.x), float(viewport.y),
      float(viewport.width), float(viewport.height)};
    D3D11_BUFFER_DESC buffer{};
    buffer.ByteWidth = sizeof(geometry);
    buffer.Usage = D3D11_USAGE_IMMUTABLE;
    buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    const D3D11_SUBRESOURCE_DATA initial{geometry, 0, 0};
    checked(device->CreateBuffer(&buffer, &initial, &constants_), NSS_STAGE_PROCESS,
            "SDR compute geometry allocation failed");
    D3D11_SAMPLER_DESC sampling{};
    sampling.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampling.AddressU = sampling.AddressV = sampling.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampling.MaxLOD = D3D11_FLOAT32_MAX;
    sampling.MaxAnisotropy = 1;
    sampling.ComparisonFunc = D3D11_COMPARISON_NEVER;
    checked(device->CreateSamplerState(&sampling, &sampler_), NSS_STAGE_PROCESS,
            "SDR compute sampler allocation failed");
    if (source_width > static_cast<uint32_t>(viewport.width) ||
        source_height > static_cast<uint32_t>(viewport.height)) {
      const auto filter_code = Nv12Shader::compile("prepareFilters", "cs_5_0");
      checked(device->CreateComputeShader(filter_code->GetBufferPointer(), filter_code->GetBufferSize(),
                                         nullptr, &filter_shader_), NSS_STAGE_PROCESS,
              "SDR compute filter preparation shader creation failed");
      const auto allocate = [&](uint32_t count, Filter& filter) {
        constexpr uint32_t stride = 4 * sizeof(float);
        if (uint64_t(count) * stride > UINT32_MAX)
          throw Error(NSS_E_ARGUMENT, NSS_STAGE_PROCESS, 0, "SDR filter buffer size exceeds its domain");
        D3D11_BUFFER_DESC description{};
        description.ByteWidth = count * stride;
        description.Usage = D3D11_USAGE_DEFAULT;
        description.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        ComPtr<ID3D11Buffer> coefficients;
        checked(device->CreateBuffer(&description, nullptr, &coefficients), NSS_STAGE_PROCESS,
                "SDR filter buffer allocation failed");
        D3D11_SHADER_RESOURCE_VIEW_DESC resource{};
        resource.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        resource.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        resource.Buffer.NumElements = count;
        checked(device->CreateShaderResourceView(coefficients.Get(), &resource, &filter.read), NSS_STAGE_PROCESS,
                "SDR filter read view creation failed");
        D3D11_UNORDERED_ACCESS_VIEW_DESC writable{};
        writable.Format = resource.Format;
        writable.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        writable.Buffer.NumElements = count;
        checked(device->CreateUnorderedAccessView(coefficients.Get(), &writable, &filter.write), NSS_STAGE_PROCESS,
                "SDR filter write view creation failed");
      };
      allocate(width_, horizontal_);
      allocate(height_, vertical_);
    }
  }
  static HRESULT views(ID3D11Device* device, ID3D11Texture2D* bgra,
                        ID3D11Texture2D* nv12, Views& output) noexcept {
    ComPtr<ID3D11Device3> modern;
    auto result = device->QueryInterface(IID_PPV_ARGS(&modern));
    Views candidate;
    if (SUCCEEDED(result)) result = device->CreateShaderResourceView(bgra, nullptr, &candidate.source);
    D3D11_UNORDERED_ACCESS_VIEW_DESC1 desc{};
    desc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
    desc.Format = DXGI_FORMAT_R8_UNORM;
    ComPtr<ID3D11UnorderedAccessView1> y, uv;
    if (SUCCEEDED(result)) result = modern->CreateUnorderedAccessView1(nv12, &desc, &y);
    desc.Format = DXGI_FORMAT_R8G8_UNORM;
    desc.Texture2D.PlaneSlice = 1;
    if (SUCCEEDED(result)) result = modern->CreateUnorderedAccessView1(nv12, &desc, &uv);
    if (SUCCEEDED(result)) {
      candidate.y = y; candidate.uv = uv;
      output = std::move(candidate);
    }
    return result;
  }
  void prepare_packed(ID3D11Device* device) {
    if (packed_shader_) return;
    const auto code = Nv12Shader::compile("computePacked", "cs_5_0");
    ComPtr<ID3D11ComputeShader> shader;
    checked(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &shader),
            NSS_STAGE_PROCESS, "Packed NV12 compute shader creation failed");
    const auto stride = (width_ + 3) & ~3U;
    const uint32_t geometry[]{width_, height_, stride, stride * height_ / 4};
    D3D11_BUFFER_DESC description{};
    description.ByteWidth = sizeof(geometry); description.Usage = D3D11_USAGE_IMMUTABLE;
    description.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    const D3D11_SUBRESOURCE_DATA initial{geometry, 0, 0};
    checked(device->CreateBuffer(&description, &initial, &packed_geometry_), NSS_STAGE_PROCESS,
            "Packed NV12 geometry allocation failed");
    packed_shader_ = std::move(shader);
  }
  void dispatch_packed(ID3D11DeviceContext* context, ID3D11ShaderResourceView* source,
                       ID3D11UnorderedAccessView* target) {
    if (!packed_shader_ || !source || !target)
      throw Error(NSS_E_STATE, NSS_STAGE_PROCESS, 0, "Packed NV12 dispatch was not prepared");
    prepare_filters(context);
    context->CSSetShader(packed_shader_.Get(), nullptr, 0);
    ID3D11Buffer* geometry[]{constants_.Get(), packed_geometry_.Get()};
    context->CSSetConstantBuffers(0, 2, geometry);
    context->CSSetSamplers(0, 1, sampler_.GetAddressOf());
    ID3D11ShaderResourceView* inputs[]{source, horizontal_.read.Get(), vertical_.read.Get()};
    context->CSSetShaderResources(0, 3, inputs);
    context->CSSetUnorderedAccessViews(4, 1, &target, nullptr);
    context->Dispatch((width_ + 31) / 32, (height_ + 7) / 8, 1);
    ID3D11ShaderResourceView* no_sources[3]{};
    ID3D11UnorderedAccessView* no_target{};
    context->CSSetShaderResources(0, 3, no_sources);
    context->CSSetUnorderedAccessViews(4, 1, &no_target, nullptr);
  }
  void dispatch(ID3D11DeviceContext* context, const Views& views) {
    prepare_filters(context);
    context->CSSetShader(shader_.Get(), nullptr, 0);
    context->CSSetConstantBuffers(0, 1, constants_.GetAddressOf());
    context->CSSetSamplers(0, 1, sampler_.GetAddressOf());
    ID3D11ShaderResourceView* inputs[]{views.source.Get(), horizontal_.read.Get(), vertical_.read.Get()};
    context->CSSetShaderResources(0, 3, inputs);
    ID3D11UnorderedAccessView* planes[]{views.y.Get(), views.uv.Get()};
    context->CSSetUnorderedAccessViews(0, 2, planes, nullptr);
    context->Dispatch((width_ + 15) / 16, (height_ + 15) / 16, 1);
    ID3D11ShaderResourceView* no_sources[3]{};
    ID3D11UnorderedAccessView* no_planes[2]{};
    context->CSSetShaderResources(0, 3, no_sources);
    context->CSSetUnorderedAccessViews(0, 2, no_planes, nullptr);
  }
};
} // namespace nss::capture
