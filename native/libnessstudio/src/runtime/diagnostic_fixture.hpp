#pragma once
#include "capture/capture.hpp"
#include <algorithm>
#include <filesystem>
#include <memory>
#include <ness_muxer_v2.h>
#include <string>
#include <vector>

namespace nss {
// The public diagnostic uses the same versioned CPU encoder/muxer contract as
// recording. It opens no capture source or graphics device.
inline uint64_t software_fixture(const std::string &filename) {
  auto fresh = []<class T>() {
    T value{};
    value.size = sizeof(value);
    value.version = 2;
    return value;
  };
  auto detail = fresh.template operator()<ness_error_v2>();
  auto checked = [&](int32_t result) {
    if (result >= 0)
      return;
    const auto code = result == NESS_V2_E_UNAVAILABLE ? NSS_E_UNAVAILABLE
                      : result == NESS_V2_E_IO ? NSS_E_IO
                      : result == NESS_V2_E_ARGUMENT ? NSS_E_ARGUMENT
                      : NSS_E_INTERNAL;
    throw capture::Error(code,
                         detail.stage >= NESS_V2_STAGE_OUTPUT ? NSS_STAGE_OUTPUT
                                                             : NSS_STAGE_ENCODER,
                         detail.native_code,
                         reinterpret_cast<const char *>(detail.message_utf8));
  };
  constexpr uint32_t width = 320, height = 240, count = 60, fps = 30;
  auto config = fresh.template operator()<ness_encoder_config_v2>();
  config.backend = NESS_V2_BACKEND_X264;
  config.policy = NESS_V2_POLICY_SOFTWARE;
  config.width = width;
  config.height = height;
  config.fps_num = fps;
  config.fps_den = 1;
  config.bitrate_bps = 800000;
  config.rate_control = NESS_V2_CBR;
  config.profile = NESS_V2_PROFILE_BASELINE;
  config.color = NESS_V2_BT709_LIMITED;
  config.max_pending_frames = 4;
  config.max_packet_bytes = 1024 * 1024;
  ness_encoder_v2 *raw_encoder{};
  checked(ness_encoder_create_v2(&config, &raw_encoder, &detail));
  std::unique_ptr<ness_encoder_v2, decltype(&ness_encoder_release_v2)> encoder(
      raw_encoder, ness_encoder_release_v2);
  ness_codec_v2 *raw_codec{};
  checked(ness_encoder_description_v2(encoder.get(), &raw_codec, &detail));
  std::unique_ptr<ness_codec_v2, decltype(&ness_codec_release_v2)> codec(
      raw_codec, ness_codec_release_v2);
  if (!codec)
    throw capture::Error(NSS_E_INTERNAL, NSS_STAGE_ENCODER, 0,
                         "x264 diagnostic did not provide its codec description");
  auto mux_config = fresh.template operator()<ness_muxer_config_v2>();
  mux_config.track_id = 1;
  mux_config.max_buffered_packets = 4;
  mux_config.max_buffered_bytes = 1024 * 1024;
  mux_config.output_path_utf8 = {
      reinterpret_cast<const uint8_t *>(filename.data()), filename.size()};
  ness_mkv_v2 *raw_muxer{};
  checked(ness_mkv_open_v2(&mux_config, codec.get(), &raw_muxer, &detail));
  std::unique_ptr<ness_mkv_v2, decltype(&ness_mkv_release_v2)> muxer(
      raw_muxer, ness_mkv_release_v2);
  uint64_t packets{};
  auto receive = [&] {
    for (uint32_t budget = 0; budget <= count; ++budget) {
      ness_packet_v2 *raw_packet{};
      const auto result = ness_encoder_receive_v2(encoder.get(), &raw_packet, &detail);
      std::unique_ptr<ness_packet_v2, decltype(&ness_packet_release_v2)> packet(
          raw_packet, ness_packet_release_v2);
      checked(result);
      if (result == NESS_V2_AGAIN || result == NESS_V2_END)
        return result;
      if (!packet || packets >= count)
        throw capture::Error(NSS_E_INTERNAL, NSS_STAGE_ENCODER, 0,
                             "x264 diagnostic returned an invalid packet count");
      auto view = fresh.template operator()<ness_packet_view_v2>();
      checked(ness_packet_view_get_v2(packet.get(), &view, &detail));
      const auto pts = static_cast<int64_t>(packets * 10000000 / fps);
      const auto next = static_cast<int64_t>((packets + 1) * 10000000 / fps);
      if (view.frame_id != packets + 1 || view.pts_hns != pts ||
          view.dts_hns != pts || view.duration_hns != next - pts)
        throw capture::Error(NSS_E_INTERNAL, NSS_STAGE_ENCODER, 0,
                             "x264 diagnostic changed input identity or timestamps");
      checked(ness_mkv_write_packet_v2(muxer.get(), packet.get(), &detail));
      ++packets;
    }
    throw capture::Error(NSS_E_INTERNAL, NSS_STAGE_ENCODER, 0,
                         "x264 diagnostic exceeded its bounded receive budget");
  };
  std::vector<uint8_t> pixels(width * height * 3 / 2, 128);
  for (uint64_t index = 0; index < count; ++index) {
    std::fill_n(pixels.data(), width * height, static_cast<uint8_t>(16 + index * 3));
    auto frame = fresh.template operator()<ness_cpu_frame_v2>();
    frame.format = NESS_V2_NV12;
    frame.width = width;
    frame.height = height;
    frame.frame_id = index + 1;
    frame.pts_hns = static_cast<int64_t>(index * 10000000 / fps);
    frame.duration_hns = static_cast<int64_t>((index + 1) * 10000000 / fps) - frame.pts_hns;
    frame.planes[0] = {pixels.data(), width * height, width, 0};
    frame.planes[1] = {pixels.data() + width * height, width * height / 2, width, 0};
    checked(ness_encoder_submit_cpu_v2(encoder.get(), &frame, &detail));
    (void)receive();
  }
  checked(ness_encoder_drain_v2(encoder.get(), &detail));
  if (receive() != NESS_V2_END || packets != count)
    throw capture::Error(NSS_E_INTERNAL, NSS_STAGE_ENCODER, 0,
                         "x264 diagnostic did not drain all expected inputs");
  checked(ness_mkv_finalize_v2(muxer.get(), &detail));
  return packets;
}
} // namespace nss
