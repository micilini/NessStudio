#include "capture/capture.hpp"
#include "cpu_features.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <iostream>
#include <vector>

using namespace nss::capture;
static void require(bool value, const char *message) {
  if (!value) throw std::runtime_error(message);
}
static uint32_t random_value(uint32_t &state) {
  state = state * 1664525U + 1013904223U;
  return state;
}
static void check_image(uint32_t sw, uint32_t sh, uint32_t stride,
                         Rect crop, Rect viewport, uint32_t width, uint32_t height,
                         size_t alignment, uint32_t &random, bool general) {
  const auto input_bytes = size_t(sh - 1) * stride + size_t(sw) * 4;
  std::vector<uint8_t> input(alignment + input_bytes);
  for (auto &v : input) v = static_cast<uint8_t>(random_value(random) >> 24);
  const auto original = input;
  const BgraView source{input.data() + alignment, sw, sh, stride, input_bytes};
  const auto output_bytes = size_t(width) * height * 3 / 2;
  std::vector<uint8_t> scalar(output_bytes), guarded(output_bytes + alignment + 31, 0xa5);
  process_cpu(source, crop, viewport, width, height, scalar, {}, CpuPixelKernel::Scalar);
  process_cpu(source, crop, viewport, width, height,
               {guarded.data() + alignment, output_bytes});
  require(std::equal(scalar.begin(), scalar.end(), guarded.begin() + alignment),
          "Automatic SIMD conversion changed a scalar NV12 byte");
  if (nss::cpu::has_avx2(nss::cpu::capabilities())) {
    std::vector<uint8_t> precise(output_bytes);
    process_cpu(source, crop, viewport, width, height, precise, {}, CpuPixelKernel::Avx2Double);
    require(precise == scalar, "Original double SIMD kernel differs from scalar conversion");
  }
  require(std::all_of(guarded.begin(), guarded.begin() + alignment, [](uint8_t v) { return v == 0xa5; }) &&
            std::all_of(guarded.begin() + alignment + output_bytes, guarded.end(), [](uint8_t v) { return v == 0xa5; }),
          "SIMD conversion overwrote output guards");
  if (general) {
    const std::array<uint8_t, 16> transparent{};
    const Layer layer{{transparent.data(), 2, 2, 8, transparent.size()},
                       {0, 0, static_cast<int32_t>(width), static_cast<int32_t>(height)}, 0, false};
    std::vector<uint8_t> reference(output_bytes);
    process_cpu(source, crop, viewport, width, height, reference, {&layer, 1});
    require(reference == scalar, "Pixel kernel differs from the independent general compositor");
  }
  require(input == original, "Pixel conversion modified the borrowed source");
}
static Json equivalence() {
  uint32_t random = 0x1ab079U;
  size_t cases{};
  for (size_t alignment : {size_t{0}, size_t{1}, size_t{7}, size_t{15}, size_t{31}}) {
    for (int i = 0; i < 120; ++i) {
      const auto sw = 1 + random_value(random) % 127, sh = 1 + random_value(random) % 97;
      const auto x = random_value(random) % sw, y = random_value(random) % sh;
      const auto cw = 1 + random_value(random) % (sw - x), ch = 1 + random_value(random) % (sh - y);
      const auto vw = 2 * (1 + random_value(random) % 48), vh = 2 * (1 + random_value(random) % 36);
      const auto padding = 2 * (random_value(random) % 3);
      check_image(sw, sh, sw * 4 + random_value(random) % 19,
                    {static_cast<int32_t>(x), static_cast<int32_t>(y), static_cast<int32_t>(cw), static_cast<int32_t>(ch)},
                    {static_cast<int32_t>(padding), static_cast<int32_t>(padding), static_cast<int32_t>(vw), static_cast<int32_t>(vh)},
                    vw + 2 * padding, vh + 2 * padding, alignment, random, i % 9 == 0);
      ++cases;
    }
    for (uint32_t width : {2U, 6U, 34U, 1920U, 4096U}) {
      check_image(width, 4, width * 4, {0, 0, static_cast<int32_t>(width), 4},
                    {0, 0, static_cast<int32_t>(width), 4}, width, 4, alignment, random, true);
      ++cases;
    }
    check_image(16384, 2, 65536, {16383, 1, 1, 1}, {0, 0, 4096, 4}, 4096, 4, alignment, random, true);
    check_image(1, 16384, 4, {0, 16380, 1, 4}, {0, 0, 2, 4}, 2, 4, alignment, random, true);
    cases += 2;
  }
  // Feature dispatch must reject AVX2 if either the CPU or the operating
  // system does not provide the required vector register state.
  const nss::cpu::Capabilities complete{true, true, true, true, 6};
  require(nss::cpu::has_avx2(complete), "Complete AVX2 capability set rejected");
  for (int field = 0; field < 5; ++field) {
    auto missing = complete;
    if (field == 0) missing.sse2 = false;
    if (field == 1) missing.avx = false;
    if (field == 2) missing.osxsave = false;
    if (field == 3) missing.avx2 = false;
    if (field == 4) missing.xcr0 = 2;
    require(!nss::cpu::has_avx2(missing), "Incomplete CPU/OS vector state accepted");
  }
  return {{"status", "PASS"}, {"pixelCases", cases},
            {"automaticKernel", nss::cpu::has_avx2(nss::cpu::capabilities()) ? "avx2" : "scalar"},
            {"byteExact", true}, {"unalignedInputsOutputs", true}, {"sourcePreserved", true}};
}
static Json benchmark() {
  const std::array<std::array<uint32_t, 4>, 3> shapes{{{1280, 720, 1280, 720},
                                                     {1920, 1200, 1280, 720},
                                                     {1920, 1200, 1920, 1080}}};
  Json rows = Json::array();
  uint32_t random = 77;
  for (const auto &shape : shapes) {
    const auto sw = shape[0], sh = shape[1], width = shape[2], height = shape[3];
    std::vector<uint8_t> input(size_t(sw) * sh * 4), scalar(size_t(width) * height * 3 / 2), optimized(scalar.size());
    for (auto &v : input) v = static_cast<uint8_t>(random_value(random) >> 24);
    const BgraView source{input.data(), sw, sh, sw * 4, input.size()};
    const Rect crop{0, 0, static_cast<int32_t>(sw), static_cast<int32_t>(sh)};
    const auto viewport = fit_viewport(sw, sh, width, height);
    Json pairs = Json::array();
    for (int pair = -3; pair < 12; ++pair) {
      std::array<double, 2> cost{};
      for (int at = 0; at < 2; ++at) {
        const auto index = (pair & 1) ? 1 - at : at;
        auto &destination = index ? optimized : scalar;
        const auto start = std::chrono::steady_clock::now();
        process_cpu(source, crop, viewport, width, height, destination, {},
                     index ? CpuPixelKernel::Automatic : CpuPixelKernel::Scalar);
        cost[static_cast<size_t>(index)] = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
      }
      require(scalar == optimized, "Benchmark pixels differ");
      if (pair >= 0) pairs.push_back({{"scalarMs", cost[0]}, {"automaticMs", cost[1]}});
    }
    rows.push_back({{"sourceWidth", sw}, {"sourceHeight", sh}, {"outputWidth", width}, {"outputHeight", height}, {"pairs", pairs}});
  }
  return {{"status", "PASS"}, {"scope", "Offline CPU pixel conversion equivalence"},
            {"automaticKernel", nss::cpu::has_avx2(nss::cpu::capabilities()) ? "avx2" : "scalar"}, {"cases", rows}};
}
int main(int argc, char **argv) {
  try {
    const std::string scope = argc > 1 ? argv[1] : "equivalence";
    if (scope != "equivalence" && scope != "benchmark") return 2;
    std::cout << (scope == "benchmark" ? benchmark() : equivalence()).dump() << '\n';
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
