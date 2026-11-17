#include "capture_fixture.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <ness_muxer_v2.h>

using namespace nss;
using namespace nss::capture;
using fixture::check;
template <class T> T fresh() {
  T v{};
  v.size = sizeof(v);
  v.version = 2;
  return v;
}
static void ok(int32_t code, const ness_error_v2 &e) {
  if (code < 0)
    throw std::runtime_error(
        std::string(reinterpret_cast<const char *>(e.message_utf8)) +
        " HRESULT=" + std::to_string(e.native_code));
}
int wmain(int argc, wchar_t **argv) {
  std::cout << std::unitbuf;
  try {
    check(argc == 2, "Expected a new absolute MKV path");
    const std::filesystem::path path(argv[1]);
    const auto output = path.u8string();
    fixture::Window window(displays().front().bounds);
    window.pattern(0, 1);
    auto config = plan(Json{{"version", 1},
                            {"cursor", false},
                            {"slots", 3},
                            {"output", {{"width", 640}, {"height", 400}}}});
    auto source = make_gpu(config);
    auto acquire = [&] {
      const auto deadline = qpc_hns() + 3 * hns_per_second;
      for (;;) {
        if (auto frame = source->poll())
          return *frame;
        check(qpc_hns() < deadline, "GPU fixture first-content timeout");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    };
    std::vector<Frame> held;
    for (unsigned i = 0; i < 3; ++i)
      held.push_back(acquire());
    auto error = fresh<ness_error_v2>();
    auto enc = fresh<ness_encoder_config_v2>();
    enc.backend = NESS_V2_BACKEND_MF_D3D11;
    enc.policy = NESS_V2_POLICY_REQUIRE_GPU;
    enc.width = 640;
    enc.height = 400;
    enc.fps_num = 30;
    enc.fps_den = 1;
    enc.profile = 66;
    enc.color = 1;
    enc.bitrate_bps = 4000000;
    enc.max_pending_frames = 1;
    enc.max_packet_bytes = 4 * 1024 * 1024;
    auto device = fresh<ness_gpu_device_v2>();
    device.device = held[0].surface->gpu_device();
    device.adapter_luid = held[0].surface->adapter_luid();
    ness_encoder_v2 *encoder{};
    auto wrong = device;
    wrong.adapter_luid ^= 1;
    check(ness_encoder_create_gpu_v2(&enc, &wrong, &encoder, &error) ==
                  NESS_V2_E_ARGUMENT &&
              !encoder,
          "Mismatched device LUID was accepted");
    ok(ness_encoder_create_gpu_v2(&enc, &device, &encoder, &error), error);
    struct Encoder {
      ness_encoder_v2 *&e;
      ~Encoder() { ness_encoder_release_v2(e); }
    } encoder_owner{encoder};
    auto identity = fresh<ness_gpu_info_v2>();
    ok(ness_encoder_gpu_info_get_v2(encoder, &identity, &error), error);
    auto mux = fresh<ness_muxer_config_v2>();
    mux.track_id = 1;
    mux.max_buffered_packets = 32;
    mux.max_buffered_bytes = 4 * 1024 * 1024;
    mux.output_path_utf8 = {reinterpret_cast<const uint8_t *>(output.data()),
                            output.size()};
    ness_mkv_v2 *writer{};
    ok(ness_mkv_open_v2(&mux, nullptr, &writer, &error), error);
    struct Writer {
      ness_mkv_v2 *w;
      ~Writer() { ness_mkv_release_v2(w); }
    } writer_owner{writer};
    bool described = false, drained = false;
    uint64_t submitted{}, written{}, pressure{}, peak_pending{};
    std::vector<ness_input_lease_v2 *> leases;
    std::vector<std::shared_ptr<const Surface>> submitted_surfaces;
    struct Leases {
      std::vector<ness_input_lease_v2 *> &values;
      ~Leases() {
        for (auto *p : values)
          ness_input_release_v2(p);
      }
    } lease_owner{leases};
    auto receive = [&] {
      for (;;) {
        ness_packet_v2 *packet{};
        const auto status = ness_encoder_receive_v2(encoder, &packet, &error);
        ok(status, error);
        if (status == NESS_V2_AGAIN)
          break;
        if (status == NESS_V2_END) {
          drained = true;
          break;
        }
        struct Packet {
          ness_packet_v2 *p;
          ~Packet() { ness_packet_release_v2(p); }
        } owned{packet};
        if (!described) {
          ness_codec_v2 *codec{};
          ok(ness_encoder_description_v2(encoder, &codec, &error), error);
          check(codec != nullptr, "First hardware AU lacks codec private");
          const auto code = ness_mkv_set_codec_v2(writer, codec, &error);
          ness_codec_release_v2(codec);
          ok(code, error);
          described = true;
        }
        ok(ness_mkv_write_packet_v2(writer, packet, &error), error);
        ++written;
      }
      for (size_t i = 0; i < leases.size(); ++i) {
        auto value = fresh<ness_completion_v2>();
        ok(ness_input_completion_get_v2(leases[i], &value, &error), error);
        if (value.ready)
          submitted_surfaces[i].reset();
      }
    };
    auto submit = [&](const Frame &frame) {
      auto input = fresh<ness_gpu_frame_v2>();
      input.resource_type = 1;
      input.format = 1;
      input.width = 640;
      input.height = 400;
      input.texture = frame.surface->gpu_texture();
      input.device = frame.surface->gpu_device();
      input.adapter_luid = frame.surface->adapter_luid();
      input.frame_id = submitted + 1;
      input.pts_hns = frame_time(submitted, 30, 1);
      input.duration_hns = frame_time(submitted + 1, 30, 1) - input.pts_hns;
      auto foreign = input;
      foreign.adapter_luid ^= 1;
      ness_input_lease_v2 *invalid{};
      check(ness_encoder_submit_gpu_v2(encoder, &foreign, &invalid, &error) ==
                    NESS_V2_E_ARGUMENT &&
                !invalid,
            "Cross-adapter texture was accepted");
      const auto deadline = qpc_hns() + 3 * hns_per_second;
      for (;;) {
        ness_input_lease_v2 *lease{};
        const auto status =
            ness_encoder_submit_gpu_v2(encoder, &input, &lease, &error);
        if (status == NESS_V2_E_BACKPRESSURE) {
          ++pressure;
          check(!lease, "Rejected input retained a caller lease");
          receive();
          check(qpc_hns() < deadline, "Hardware backpressure did not recover");
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
          continue;
        }
        ok(status, error);
        check(lease != nullptr, "GPU input has no completion lease");
        leases.push_back(lease);
        submitted_surfaces.push_back(frame.surface);
        ++submitted;
        auto stats = fresh<ness_encoder_stats_v2>();
        ok(ness_encoder_stats_get_v2(encoder, &stats, &error), error);
        peak_pending = (std::max)(peak_pending, stats.pending);
        check(stats.pending <= 1, "Configured pending budget exceeded");
        break;
      }
    };
    // A genuinely delayed consumer keeps every source texture while the screen
    // changes. Submit only after six newer painted identities: premature pool
    // reuse corrupts the MKV.
    for (unsigned number = 2; number <= 7; ++number) {
      window.pattern(0, number);
      for (unsigned retry = 0; retry < 10; ++retry) {
        (void)source->poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
      }
    }
    const auto saturated = source->metrics();
    check(saturated.at("poolPressureDrops").get<uint64_t>() > 0,
          "Retained GPU textures did not saturate the bounded pool");
    for (const auto &frame : held)
      submit(frame);
    receive();
    held.erase(held.begin() + 1, held.end());
    auto advanced = acquire();
    submit(advanced);
    // Stop while accepted input/output is still pending; all four valid frames
    // must survive.
    auto busy = fresh<ness_encoder_stats_v2>();
    ok(ness_encoder_stats_get_v2(encoder, &busy, &error), error);
    ok(ness_encoder_drain_v2(encoder, &error), error);
    const auto drain_deadline = qpc_hns() + 3 * hns_per_second;
    while (!drained) {
      receive();
      check(qpc_hns() < drain_deadline, "Async EOS did not complete");
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    check(submitted == 4 && written == 4, "Busy Stop lost an accepted frame");
    ok(ness_mkv_finalize_v2(writer, &error), error);
    auto finalized = fresh<ness_muxer_stats_v2>();
    ok(ness_mkv_stats_get_v2(writer, &finalized, &error), error);
    check(finalized.finalized && finalized.packets == 4,
          "MKV did not close completely");
    ness_encoder_release_v2(encoder);
    encoder = nullptr;
    for (auto *lease : leases) {
      auto completed = fresh<ness_completion_v2>();
      while (!completed.ready) {
        ok(ness_input_completion_get_v2(lease, &completed, &error), error);
        check(qpc_hns() < drain_deadline,
              "Tracked texture sample never returned");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    }
    for (auto *lease : leases)
      ness_input_release_v2(lease);
    leases.clear();
    submitted_surfaces.clear();
    held.clear();
    advanced.surface.reset();
    source->close();
    source.reset();
    Json report = {{"status", "PASS"},
                   {"implementationId", reinterpret_cast<const char *>(
                                            identity.implementation_utf8)},
                   {"adapterLuid", identity.adapter_luid},
                   {"submitted", submitted},
                   {"written", written},
                   {"expectedPaintedNumbers", Json::array({1, 1, 1, 7})},
                   {"width", 640},
                   {"height", 400},
                   {"sourceWidth", config.display.bounds.width},
                   {"sourceHeight", config.display.bounds.height},
                   {"poolPressure", saturated},
                   {"inputBackpressure", pressure},
                   {"pendingPeak", peak_pending},
                   {"pendingLimit", 1},
                   {"busyStopPending", busy.pending},
                   {"busyStopQueued", busy.queued_packets},
                   {"finalized", true},
                   {"allTrackedSamplesReturned", true},
                   {"livePipelines", live_pipelines.load()},
                   {"liveSurfaces", live_surfaces.load()},
                   {"liveCaptureBytes", live_capture_bytes.load()},
                   {"pixelReadbacks", 0}};
    std::cout << report.dump() << '\n';
    return 0;
  } catch (const std::exception &e) {
    std::cout << Json{{"status", "FAIL"}, {"message", e.what()}}.dump() << '\n';
    return 1;
  }
}
