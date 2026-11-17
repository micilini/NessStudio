#include "preview/preview.hpp"
#include <cstring>
#include <iostream>

namespace {
void check(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}
struct Surface final : nss::capture::Surface {
  std::vector<std::byte> pixels;
  explicit Surface(uint8_t y) : pixels(128 * 72 * 3 / 2, std::byte{128}) {
    std::fill_n(pixels.begin(), 128 * 72, std::byte{y});
  }
  std::span<const std::byte> cpu_pixels() const override { return pixels; }
};
} // namespace
int main() {
  try {
    using namespace nss;
    using preview::Json;
    std::vector<std::shared_ptr<preview::Frame>> retained;
    Json metrics;
    {
      preview::Hub hub;
      hub.bind(52, 1);
      nss_preview_config_v1 config{
          sizeof(config), 1, 1, 640, 360, 15, 1, 3, 1, 0};
      auto bad = config;
      bad.slots = 4;
      bool rejected{};
      try {
        (void)hub.attach(bad);
      } catch (const capture::Error &error) {
        rejected = error.code == NSS_E_ARGUMENT;
      }
      check(rejected, "Preview accepted an unbounded slot budget");
      auto consumer = hub.attach(config);
      for (uint64_t index = 0; index < 4; ++index) {
        auto source =
            std::make_shared<Surface>(static_cast<uint8_t>(16 + index * 32));
        capture::Frame input{source,    index + 1, 1,  qpc_hns(), qpc_hns(),
                             qpc_hns(), 128,       72, 128,       false};
        hub.offer(input);
        check(source.use_count() == 2,
              "CPU preview retained its recording source surface");
        if (index < 3) {
          auto value = consumer->acquire(false);
          check(value != nullptr, "CPU preview did not publish");
          check(value->cpu.session_id == 52 && value->cpu.source_id == 1 &&
                    value->cpu.frame_id == index + 1 &&
                    value->cpu.bytes == 128 * 72 * 4 &&
                    value->cpu.stride == 512,
                "Preview identity/bounds are incorrect");
          const auto *bytes = static_cast<const uint8_t *>(value->cpu.data);
          check(bytes[3] == 255 && bytes[value->cpu.bytes - 1] == 255,
                "CPU preview lost opaque alpha");
          retained.push_back(value);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(70));
      }
      metrics = consumer->metrics();
      check(metrics.at("dropped").get<uint64_t>() > 0,
            "Stalled presentation did not drop independently");
      const auto *black = static_cast<const uint8_t *>(retained[0]->cpu.data);
      check(black[0] == 0 && black[1] == 0 && black[2] == 0,
            "BT709 limited black was not converted correctly");
      hub.close();
      consumer.reset();
    }
    check(
        capture::live_surfaces == 0 && preview::live_channels == 0 &&
            preview::live_bytes == 3ULL * 128 * 72 * 4,
        "Presentation storage retained capture or lost independent ownership");
    check(static_cast<const uint8_t *>(retained[0]->cpu.data)[3] == 255,
          "Presentation lease died with the session");
    retained.clear();
    check(preview::live_bytes == 0,
          "Presentation pool did not return its byte budget");
    std::cout << Json{{"status", "PASS"},
                      {"boundedSlots", 3},
                      {"cpuLeaseIndependentOfCapture", true},
                      {"leaseSurvivesHub", true},
                      {"metrics", metrics}}
                     .dump()
              << '\n';
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
