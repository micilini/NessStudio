#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "scene/scene.hpp"
#include <cmath>
#include <cstring>
#include <d3d11.h>
#include <dxgi.h>
#include <iostream>
#include <thread>
#include <windows.h>
#include <wrl/client.h>

using namespace nss;
using Json = nlohmann::json;
using Microsoft::WRL::ComPtr;
namespace {
void require(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
template <class Action> void rejects(Action action) {
  try {
    action();
  } catch (const capture::Error &) {
    return;
  }
  throw std::runtime_error("Invalid scene was accepted");
}
scene::Snapshot snapshot() {
  return scene::parse(Json::parse(
      R"({"version":1,"width":320,"height":180,"layout":"Custom","layers":[{"sourceId":1},{"sourceId":2,"opacity":0.5}]})"));
}
struct Ram final : capture::Surface {
  std::vector<std::byte> bytes;
  Ram() : bytes(320 * 180 * 4) {
    for (uint32_t y = 0; y < 180; ++y)
      for (uint32_t x = 0; x < 320; ++x) {
        auto *p = reinterpret_cast<uint8_t *>(bytes.data()) + (y * 320 + x) * 4;
        p[0] = x < 160 ? 32 : 190;
        p[1] = y < 90 ? 60 : 210;
        p[2] = x < 160 ? 210 : 40;
        p[3] = x < 160 ? 128 : 255;
      }
  }
  std::span<const std::byte> cpu_pixels() const override { return bytes; }
};
capture::Frame input() {
  capture::Frame frame{
      std::make_shared<Ram>(), 1, 1, 1, 2, 1, 320, 180, 1280, true};
  frame.format = capture::Frame::Format::Bgra;
  frame.alpha = capture::Frame::Alpha::Straight;
  return frame;
}
void geometry() {
  auto value = snapshot();
  const auto layer = value.layers.front();
  auto result = scene::place(value, layer, 1920, 1200);
  require(result.destination.width == 288 && result.destination.height == 180 &&
              result.destination.x == 16,
          "Contain aspect ratio failed");
  auto rotated = layer;
  rotated.rotation = 90;
  result = scene::place(value, rotated, 320, 180);
  require(result.destination.width == 102 && result.destination.height == 180,
          "Rotated aspect ratio failed");
  result.rotation = 90;
  result.mirror = false;
  result.crop = {};
  auto uv = scene::source_uv(result, .25, .75);
  require(uv[0] == .75 && uv[1] == .75, "Clockwise inverse orientation failed");
  result.mirror = true;
  uv = scene::source_uv(result, .25, .75);
  require(uv[0] == .75 && uv[1] == .25, "Output-space mirror failed");
  auto cover = layer;
  cover.cover = true;
  result = scene::place(value, cover, 640, 180);
  require(std::abs(result.crop.width - .5) < .00001 &&
              std::abs(result.crop.x - .25) < .00001,
          "Cover source crop failed");
  for (const auto *key : {"width", "height", "fpsNum"}) {
    auto bad = scene::json(value);
    bad[key] = 1;
    rejects([&] { scene::parse(bad); });
  }
  auto bad = scene::json(value);
  bad["layers"][0]["rotation"] = 45;
  rejects([&] { scene::parse(bad); });
  bad = scene::json(value);
  bad["layers"][0]["crop"]["x"] = .5;
  rejects([&] { scene::parse(bad); });
  bad = scene::json(value);
  bad["layers"][0]["opacity"] = 1.1;
  rejects([&] { scene::parse(bad); });
  bad["unexpected"] = true;
  rejects([&] { scene::parse(bad); });
}
void policy() {
  auto value = snapshot();
  auto layer = value.layers.back();
  std::optional<capture::Frame> frame = input();
  const auto now = 10000000LL;
  require(scene::select(layer, frame, now, true) == scene::Selection::Frame,
          "HoldLast lost a late source");
  layer.missing = scene::Missing::Background;
  require(scene::select(layer, frame, now, true) ==
              scene::Selection::Background,
          "Background did not hide a late layer");
  layer.missing = scene::Missing::Fail;
  require(scene::select(layer, frame, now, true) == scene::Selection::Fail,
          "Fail freshness was ignored");
  require(scene::select(layer, {}, now, true) == scene::Selection::Wait,
          "First mandatory content was fabricated");
  auto renderer = scene::make_cpu(value);
  std::array<scene::Input, 1> sources{{{1, input()}}};
  auto output = renderer->render(value, sources);
  require(output.has_value(),
          "Missing camera stopped independent screen composition");
  value.layers[1].visible = false;
  auto hidden = renderer->render(value, sources);
  require(hidden.has_value(), "Hidden source stopped composition");
}
void updates() {
  auto initial = snapshot();
  scene::Store store(initial);
  auto changed = scene::json(initial);
  changed["layers"][1]["mirror"] = true;
  require(store.update(changed) == 2, "Scene revision was not acknowledged");
  require(store.get()->layers[1].mirror, "Scene snapshot was not replaced");
  auto bad = changed;
  bad["width"] = 640;
  rejects([&] { store.update(bad); });
  bad = changed;
  bad["layers"][1]["required"] = false;
  rejects([&] { store.update(bad); });
  std::atomic<bool> done{};
  std::thread writer([&] {
    for (int i = 0; i < 500; ++i) {
      changed["layers"][1]["mirror"] = (i & 1) != 0;
      store.update(changed);
    }
    done = true;
  });
  uint64_t revision{};
  while (!done) {
    auto current = store.get();
    require(current->width == 320 && current->revision >= revision,
            "Torn scene snapshot observed");
    revision = current->revision;
  }
  writer.join();
  require(store.get()->revision == 502, "Concurrent scene updates were lost");
}
void cpu() {
  auto value = snapshot();
  auto renderer = scene::make_cpu(value);
  std::array<scene::Input, 2> sources{{{1, input()}, {2, input()}}};
  auto first = renderer->render(value, sources);
  require(first.has_value(), "CPU output missing");
  const auto memory = first->surface->cpu_pixels();
  std::vector<std::byte> saved(memory.begin(), memory.end());
  require(
      uint8_t(memory[0]) >= 63 && uint8_t(memory[0]) <= 65,
      "Straight alpha/opacity did not produce the BT709 limited golden luma");
  auto second = renderer->render(value, sources),
       third = renderer->render(value, sources);
  auto fourth = renderer->render(value, sources),
       fifth = renderer->render(value, sources),
       sixth = renderer->render(value, sources);
  require(second && third && fourth && fifth && sixth &&
              !renderer->render(value, sources),
          "CPU output pool is not bounded at six leases");
  require(std::equal(saved.begin(), saved.end(), memory.begin()),
          "Held output lease was overwritten");
  second.reset();
  value.layers[1].rotation = 180;
  value.layers[1].mirror = true;
  value.layers[1].crop = {.25, 0, .5, 1};
  auto next = renderer->render(value, sources);
  require(next.has_value(), "CPU pool failed to recover");
  std::vector<uint8_t> decoded(320 * 180 * 4);
  scene::decode_bgra(*first, decoded);
  require(decoded[2] > decoded[0] + 50,
          "Straight-alpha red quadrant did not survive CPU NV12 output");
  require(renderer->metrics().at("gpuToCpuReadbackBytes") == 0,
          "CPU renderer claimed GPU work");
  renderer->close();
}
struct Texture final : capture::Surface {
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11Texture2D> texture;
  uint64_t luid{};
  void *gpu_texture() const noexcept override { return texture.Get(); }
  void *gpu_device() const noexcept override { return device.Get(); }
  uint64_t adapter_luid() const noexcept override { return luid; }
};
void gpu(scene::GpuKernel kernel = scene::GpuKernel::Automatic, bool compare_raster = false) {
  auto value = snapshot();
  auto ram = input();
  auto surface = std::make_shared<Texture>();
  ComPtr<ID3D11DeviceContext> context;
  D3D_FEATURE_LEVEL level{};
  capture::checked(
      D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                        D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                        D3D11_SDK_VERSION, &surface->device, &level, &context),
      NSS_STAGE_PROCESS, "Qualification hardware device unavailable");
  ComPtr<IDXGIDevice> dxgi;
  surface->device.As(&dxgi);
  ComPtr<IDXGIAdapter> adapter;
  dxgi->GetAdapter(&adapter);
  DXGI_ADAPTER_DESC info{};
  adapter->GetDesc(&info);
  surface->luid = (uint64_t(uint32_t(info.AdapterLuid.HighPart)) << 32) |
                  info.AdapterLuid.LowPart;
  D3D11_TEXTURE2D_DESC desc{};
  desc.Width = 320;
  desc.Height = 180;
  desc.MipLevels = desc.ArraySize = 1;
  desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
  auto bytes = ram.surface->cpu_pixels();
  D3D11_SUBRESOURCE_DATA data{bytes.data(), 1280, 0};
  capture::checked(
      surface->device->CreateTexture2D(&desc, &data, &surface->texture),
      NSS_STAGE_PROCESS, "Qualification source texture creation failed");
  auto frame = ram;
  frame.surface = surface;
  auto renderer = scene::make_gpu(value, frame, kernel);
  auto cpu = scene::make_cpu(value);
  double maximum_error{}, squared{}, raster_squared{}, raster_maximum{};
  uint64_t samples{};
  for (uint32_t variant = 0; variant < 12; ++variant) {
    const auto rotation = (variant % 4) * 90;
    if (variant == 4) {
      auto memory = std::dynamic_pointer_cast<const Ram>(ram.surface);
      require(bool(memory), "Premultiplied qualification lost its original BGRA source");
      auto premultiplied = std::make_shared<Ram>();
      premultiplied->bytes = memory->bytes;
      auto* pixels = reinterpret_cast<uint8_t*>(premultiplied->bytes.data());
      for (size_t at = 0; at < premultiplied->bytes.size(); at += 4)
        for (size_t channel = 0; channel < 3; ++channel)
          pixels[at + channel] = static_cast<uint8_t>((uint32_t(pixels[at + channel]) * pixels[at + 3] + 127) / 255);
      ram.surface = premultiplied;
      ram.alpha = capture::Frame::Alpha::Premultiplied;
      context->UpdateSubresource(surface->texture.Get(), 0, nullptr, pixels, 1280, 0);
      frame = ram; frame.surface = surface;
    }
    if (variant == 8) {
      auto opaque = value;
      opaque.layers.resize(1);
      opaque.layers[0].opacity = 1;
      opaque.layers[0].rotation = 0;
      opaque.layers[0].mirror = false;
      opaque.layers[0].crop = {};
      opaque.layers[0].destination = {};
      auto converter = scene::make_cpu(opaque);
      std::array<scene::Input, 1> source{{{1, ram}}};
      ram = *converter->render(opaque, source);
      auto nv12_surface = std::make_shared<Texture>();
      nv12_surface->device = surface->device;
      nv12_surface->luid = surface->luid;
      D3D11_TEXTURE2D_DESC nv12_desc{};
      nv12_desc.Width = 320;
      nv12_desc.Height = 180;
      nv12_desc.MipLevels = nv12_desc.ArraySize = 1;
      nv12_desc.Format = DXGI_FORMAT_NV12;
      nv12_desc.SampleDesc.Count = 1;
      nv12_desc.Usage = D3D11_USAGE_DEFAULT;
      nv12_desc.BindFlags =
          D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
      const auto pixels = ram.surface->cpu_pixels();
      D3D11_SUBRESOURCE_DATA nv12_data{pixels.data(), 320, 0};
      capture::checked(surface->device->CreateTexture2D(&nv12_desc, &nv12_data,
                                                        &nv12_surface->texture),
                       NSS_STAGE_PROCESS,
                       "Qualification NV12 texture upload failed");
      frame = ram;
      frame.surface = nv12_surface;
      renderer = scene::make_gpu(value, frame, kernel);
    }

    value.layers[1].rotation = rotation;
    value.layers[1].mirror = (rotation == 90 || rotation == 180);
    value.layers[1].crop = {.1, .1, .8, .8};
    value.layers[1].destination = {.5, 0, .5, 1};
    value.layers[0].opacity = variant < 8 ? .8 : 1;
    value.layers[1].opacity = variant < 8 ? .5 : 1;
    std::array<scene::Input, 2> gpu_inputs{{{1, frame}, {2, frame}}},
        cpu_inputs{{{1, ram}, {2, ram}}};
    auto expected = cpu->render(value, cpu_inputs);
    std::optional<capture::Frame> output;
    const auto until = qpc_hns() + 3 * hns_per_second;
    while (!(output = renderer->render(value, gpu_inputs))) {
      require(qpc_hns() < until, "GPU composition did not complete");
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    auto *texture =
        static_cast<ID3D11Texture2D *>(output->surface->gpu_texture());
    texture->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> readback;
    capture::checked(
        surface->device->CreateTexture2D(&desc, nullptr, &readback),
        NSS_STAGE_PROCESS, "Qualification-only readback allocation failed");
    context->CopyResource(readback.Get(), texture);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    capture::checked(
        context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped),
        NSS_STAGE_PROCESS, "Qualification-only readback failed");
    auto reference = expected->surface->cpu_pixels();
    std::vector<uint8_t> composed(compare_raster ? reference.size() : 0);
    for (uint32_t y = 0; y < 270; ++y)
      for (uint32_t x = 0; x < 320; ++x) {
        const auto delta = double(static_cast<uint8_t *>(
                               mapped.pData)[size_t(y) * mapped.RowPitch + x]) -
                           double(uint8_t(reference[size_t(y) * 320 + x]));
        if (compare_raster) composed[size_t(y) * 320 + x] =
            static_cast<uint8_t*>(mapped.pData)[size_t(y) * mapped.RowPitch + x];
        maximum_error = (std::max)(maximum_error, std::abs(delta));
        squared += delta * delta;
        ++samples;
      }
    context->Unmap(readback.Get(), 0);
    if (compare_raster) {
      require(renderer->metrics().at("computeCompositionSelected") == true,
              "Compute equivalence did not execute the compute path");
      auto raster = scene::make_gpu(value, frame, scene::GpuKernel::Raster);
      std::optional<capture::Frame> raster_output;
      const auto deadline = qpc_hns() + 3 * hns_per_second;
      while (!(raster_output = raster->render(value, gpu_inputs))) {
        require(qpc_hns() < deadline, "Raster reference did not complete");
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
      }
      context->CopyResource(readback.Get(), static_cast<ID3D11Texture2D*>(raster_output->surface->gpu_texture()));
      capture::checked(context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped),
                       NSS_STAGE_PROCESS, "Raster comparison readback failed");
      for (uint32_t y = 0; y < 270; ++y)
        for (uint32_t x = 0; x < 320; ++x) {
          const auto delta = double(composed[size_t(y) * 320 + x]) -
              double(static_cast<uint8_t*>(mapped.pData)[size_t(y) * mapped.RowPitch + x]);
          raster_squared += delta * delta;
          raster_maximum = (std::max)(raster_maximum, std::abs(delta));
        }
      context->Unmap(readback.Get(), 0);
      raster->close();
    }
    // Drain old revisions before submitting the next orientation.
    renderer->close();
    renderer = scene::make_gpu(value, frame, kernel);
  }
  const auto psnr = 10 * std::log10(255. * 255. / (squared / double(samples)));
  std::cout << Json{{"gpuCpuPsnr", psnr},
                    {"maximumNv12Difference", maximum_error},
                    {"samples", samples},
                    {"qualificationReadbacksOnly", true},
                    {"bgraAlphaAndOpaqueNv12Compared", true},
                    {"premultipliedInputCompared", true},
                    {"computeRasterCompared", compare_raster},
                    {"computeRasterMaximumDifference", raster_maximum},
                    {"computeRasterMeanSquaredDifference", raster_squared / double(samples)}}
                   .dump()
            << "\n";
  require(psnr > 35, "GPU/CPU composition colors, alpha or geometry diverged");
  if (compare_raster)
    require(raster_maximum <= 2 && raster_squared / double(samples) <= 0.65,
            "Compute/raster composition exceeded the declared quantization tolerance");
  renderer->close();
}
} // namespace
int main(int argc, char **argv) {
  try {
    const std::string scope = argc > 1 ? argv[1] : "";
    if (scope == "geometry")
      geometry();
    else if (scope == "missing")
      policy();
    else if (scope == "updates")
      updates();
    else if (scope == "cpu")
      cpu();
    else if (scope == "gpu")
      gpu();
    else if (scope == "gpu-raster")
      gpu(scene::GpuKernel::Raster);
    else if (scope == "gpu-compute-equivalence")
      gpu(scene::GpuKernel::Compute, true);
    else
      throw std::runtime_error("Unknown scene scope");
    require(scene::live_renderers == 0 && scene::live_bytes == 0,
            "Scene resources survived ownership scope");
    std::cout << "PASS scene-" << scope << "\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << "\n";
    return 1;
  }
}
