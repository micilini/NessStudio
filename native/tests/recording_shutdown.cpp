// Exercise the actual track shutdown implementation with an unrelated retained
// GPU lease. No GPU, physical capture or test controls enter the shipped DLL.
#include "core.hpp"
#include "../libnessstudio/src/runtime/recording.cpp"
#include <iostream>

int main() {
  try {
    ness_input_lease_v2 other_track;
    other_track.value = std::make_shared<ness2::InputCompletion>();
    nss::Plan plan;
    plan.capture.slots = 3;
    nss::Recording recording(std::move(plan));
    const auto result = recording.shutdown(nss::ShutdownStage::Packets);
    if (result.error || result.pending)
      throw std::runtime_error("A track waited for another track's retained GPU input lease");
    auto stats = nss::fresh<ness_gpu_runtime_stats_v2>();
    if (ness_gpu_runtime_stats_get_v2(&stats, nullptr) != NESS_V2_OK ||
        stats.input_leases == 0 || stats.completion_states == 0)
      throw std::runtime_error("Shutdown stole or released the other track's GPU resources");
    std::cout << "{\"status\":\"PASS\",\"unrelatedGpuLeasePreserved\":true}\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
