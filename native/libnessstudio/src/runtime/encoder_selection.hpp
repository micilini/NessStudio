#pragma once
#include <array>
#include <ness_muxer_v2.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

namespace nss {
// Called only during Prepare, before opening output files. Each failed probe
// must be an unavailable capability; allocation/configuration/driver failures
// are causal errors and must never be hidden by a different encoder.
template <class Observe>
int32_t select_encoder(ness_encoder_config_v2 &config,
                       const ness_gpu_device_v2 *device, bool automatic,
                       bool allow_software, ness_encoder_v2 **encoder,
                       ness_error_v2 &error, Observe observe) {
  auto attempt = [&](uint32_t backend) {
    config.backend = backend;
    error = {};
    error.size = sizeof(error);
    error.version = 2;
    const auto result = config.policy == NESS_V2_POLICY_REQUIRE_GPU
        ? ness_encoder_create_gpu_v2(&config, device, encoder, &error)
        : ness_encoder_create_v2(&config, encoder, &error);
    observe(backend, result, error);
    return result;
  };
  if (config.policy != NESS_V2_POLICY_REQUIRE_GPU || !automatic)
    return attempt(config.backend);
  // Probe the encoder belonging to the capture device. In particular, an AMD
  // vendor mismatch must not replace the useful NVENC error on an NVIDIA GPU.
  uint32_t vendor = 0;
  if (device && device->device) {
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgi;
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC description{};
    if (SUCCEEDED(static_cast<ID3D11Device *>(device->device)->QueryInterface(IID_PPV_ARGS(&dxgi))) &&
        SUCCEEDED(dxgi->GetAdapter(&adapter)) && SUCCEEDED(adapter->GetDesc(&description)))
      vendor = description.VendorId;
  }
  const auto native_backend = vendor == 0x8086 ? NESS_V2_BACKEND_QSV_D3D11
      : vendor == 0x10de ? NESS_V2_BACKEND_NVENC_D3D11
      : vendor == 0x1002 ? NESS_V2_BACKEND_AMF_D3D11 : 0u;
  for (const auto backend : std::array<uint32_t, 3>{
           NESS_V2_BACKEND_QSV_D3D11, NESS_V2_BACKEND_NVENC_D3D11,
           NESS_V2_BACKEND_AMF_D3D11}) {
    if (native_backend && backend != native_backend) continue;
    const auto result = attempt(backend);
    if (result != NESS_V2_E_UNAVAILABLE)
      return result;
  }
  if (!allow_software)
    return NESS_V2_E_UNAVAILABLE;
  config.policy = NESS_V2_POLICY_SOFTWARE;
  return attempt(NESS_V2_BACKEND_X264);
}
} // namespace nss
