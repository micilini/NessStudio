#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "capture/nv12_compute.hpp"
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <vector>

using namespace nss::capture;
using Microsoft::WRL::ComPtr;
static void require(bool good, const char* message) {
  if (!good) throw std::runtime_error(message);
}
static void hr(HRESULT result) {
  checked(result, NSS_STAGE_PROCESS, "NV12 compute qualification failed");
}
static std::vector<uint8_t> readback(ID3D11Device* device, ID3D11DeviceContext* context,
                                    ID3D11Texture2D* texture) {
  D3D11_TEXTURE2D_DESC description{};
  texture->GetDesc(&description);
  description.Usage = D3D11_USAGE_STAGING;
  description.BindFlags = 0;
  description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ComPtr<ID3D11Texture2D> staging;
  hr(device->CreateTexture2D(&description, nullptr, &staging));
  context->CopyResource(staging.Get(), texture);
  D3D11_MAPPED_SUBRESOURCE mapped{};
  hr(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
  std::vector<uint8_t> result(size_t(description.Width) * description.Height * 3 / 2);
  for (uint32_t row = 0; row < description.Height * 3 / 2; ++row)
    std::memcpy(result.data() + size_t(row) * description.Width,
        static_cast<const uint8_t*>(mapped.pData) + size_t(row) * mapped.RowPitch, description.Width);
  context->Unmap(staging.Get(), 0);
  return result;
}
static std::vector<uint8_t> packed_readback(ID3D11Device* device, ID3D11DeviceContext* context,
                                           Nv12Compute& compute, ID3D11ShaderResourceView* source,
                                           uint32_t width, uint32_t height) {
  const auto stride=(width+3)&~3U;
  D3D11_BUFFER_DESC desc{};desc.ByteWidth=stride*(height+height/2);
  desc.Usage=D3D11_USAGE_DEFAULT;desc.BindFlags=D3D11_BIND_UNORDERED_ACCESS;
  ComPtr<ID3D11Buffer> output,staging;
  hr(device->CreateBuffer(&desc,nullptr,&output));
  D3D11_UNORDERED_ACCESS_VIEW_DESC view{};view.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
  view.ViewDimension=D3D11_UAV_DIMENSION_BUFFER;view.Buffer.NumElements=desc.ByteWidth/4;
  ComPtr<ID3D11UnorderedAccessView> writable;
  hr(device->CreateUnorderedAccessView(output.Get(),&view,&writable));
  desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
  hr(device->CreateBuffer(&desc,nullptr,&staging));
  compute.prepare_packed(device);
  compute.dispatch_packed(context,source,writable.Get());
  context->CopyResource(staging.Get(),output.Get());
  D3D11_MAPPED_SUBRESOURCE map{};
  hr(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&map));
  std::vector<uint8_t> result(size_t(width)*(height+height/2));
  for(uint32_t row=0;row<height+height/2;++row)
    std::memcpy(result.data()+size_t(row)*width,static_cast<const uint8_t*>(map.pData)+size_t(row)*stride,width);
  context->Unmap(staging.Get(),0);
  return result;
}
int main() {
  try {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    hr(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
        &device, nullptr, &context));
    // Source size, output size and viewport. Include odd source padding,
    // sub-dispatch-sized targets, nonzero borders and both scaling branches.
    const std::array<std::array<uint32_t, 8>, 8> shapes{{
      {1,1,2,2,0,0,2,2}, {3,3,6,6,2,2,2,2}, {18,10,18,10,0,0,18,10},
      {35,21,38,26,2,2,34,22}, {47,29,28,20,2,2,24,16},
      {1920,1200,1280,720,64,0,1152,720},
      {1920,1200,1920,1080,96,0,1728,1080},
      {1280,720,1280,720,0,0,1280,720}}};
    uint32_t random = 719031;
    uint64_t samples{}, differences{}, cases{};
    double squared{}, maximum{};
    for (const auto& shape : shapes) {
      const auto [sw, sh, width, height, vx, vy, vw, vh] = shape;
      const auto tw = (sw + 1) & ~1U, th = (sh + 1) & ~1U;
      const Rect viewport{int32_t(vx),int32_t(vy),int32_t(vw),int32_t(vh)};
      Nv12Shader raster(device.Get(),sw,sh,tw,th,width,height,viewport);
      Nv12Compute compute(device.Get(),sw,sh,tw,th,width,height,viewport);
      for (int pattern = 0; pattern < 3; ++pattern) {
        std::vector<uint8_t> pixels(size_t(tw) * th * 4);
        for (size_t at = 0; at < pixels.size(); ++at) {
          random = random * 1664525U + 1013904223U;
          pixels[at] = pattern == 0 ? uint8_t(random >> 24) :
              (pattern == 1 ? uint8_t((at / 4) & 1 ? 255 : 0) : uint8_t(127 + ((at / 4) % 3)));
        }
        D3D11_TEXTURE2D_DESC description{};
        description.Width=tw; description.Height=th;
        description.MipLevels=description.ArraySize=description.SampleDesc.Count=1;
        description.Format=DXGI_FORMAT_B8G8R8A8_UNORM;
        description.Usage=D3D11_USAGE_DEFAULT;
        description.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA data{pixels.data(),tw * 4,0};
        ComPtr<ID3D11Texture2D> input, output;
        hr(device->CreateTexture2D(&description,&data,&input));
        description.Width=width; description.Height=height;
        description.Format=DXGI_FORMAT_NV12;
        description.BindFlags=D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS;
        hr(device->CreateTexture2D(&description,nullptr,&output));
        Nv12Shader::Views raster_views;
        Nv12Compute::Views compute_views;
        hr(Nv12Shader::views(device.Get(),input.Get(),output.Get(),raster_views));
        hr(Nv12Compute::views(device.Get(),input.Get(),output.Get(),compute_views));
        raster.draw(context.Get(),raster_views);
        const auto expected=readback(device.Get(),context.Get(),output.Get());
        compute.dispatch(context.Get(),compute_views);
        const auto actual=readback(device.Get(),context.Get(),output.Get());
        const auto packed=packed_readback(device.Get(),context.Get(),compute,compute_views.source.Get(),width,height);
        require(packed==actual,"Packed NV12 bytes differ from the original compute output");
        for (size_t at=0;at<actual.size();++at) {
          const auto difference=double(actual[at])-expected[at];
          squared+=difference*difference;
          maximum=(std::max)(maximum,std::abs(difference));
          differences+=difference!=0;
        }
        samples+=actual.size(); ++cases;
        context->ClearState();
      }
    }
    const auto mean_squared=squared/double(samples);
    std::cout<<Json{{"status",maximum<=1 && mean_squared<=0.65?"PASS":"FAIL"},
        {"cases",cases},{"samples",samples},{"differentBytes",differences},
        {"maximumByteDifference",maximum},{"meanSquaredDifference",mean_squared},
        {"qualificationReadbacksOnly",true},{"pixelShaderReference",true},
        {"productionComputeKernel",true},{"packedBufferBytesExactlyEqual",true}}.dump()<<'\n';
    require(maximum<=1 && mean_squared<=0.65,"Compute NV12 exceeded the declared quantization tolerance");
    return 0;
  } catch (const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
