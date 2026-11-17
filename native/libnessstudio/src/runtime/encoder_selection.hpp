#pragma once
#include <array>
#include <ness_muxer_v2.h>

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
  for (const auto backend : std::array<uint32_t, 3>{
           NESS_V2_BACKEND_QSV_D3D11, NESS_V2_BACKEND_NVENC_D3D11,
           NESS_V2_BACKEND_AMF_D3D11}) {
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
