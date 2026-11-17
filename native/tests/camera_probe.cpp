#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "camera/camera.hpp"
#include <fstream>
#include <iostream>
#include <thread>
#include <windows.h>

int main(int argc, char **argv) {
  using namespace nss;
  using Json = nlohmann::json;
  try {
    const auto inventory = camera::devices();
    if (argc == 1) {
      std::cout << inventory.dump(2) << '\n';
      return 0;
    }
    std::ifstream input(argv[1]);
    Json request;
    input >> request;
    const auto selected = camera::plan(request.at("camera"));
    camera::Source source(selected);
    const auto end =
        qpc_hns() +
        static_cast<int64_t>(request.value("durationMs", 5000U)) * 10000;
    uint64_t frames{};
    int64_t first{}, last{};
    while (qpc_hns() < end) {
      if (auto frame = source.poll()) {
        if (!frames)
          first = frame->capture_hns;
        last = frame->capture_hns;
        ++frames;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    source.close();
    const auto metrics = source.metrics();
    if (!frames)
      throw std::runtime_error("Camera did not produce any correlated frame");
    std::cout << Json{{"status", "PASS"},
                      {"inventory", inventory},
                      {"framesPolled", frames},
                      {"firstTimestampHns", first},
                      {"lastTimestampHns", last},
                      {"metrics", metrics},
                      {"liveSources", camera::live_sources.load()}}
                     .dump(2)
              << '\n';
    return 0;
  } catch (const capture::Error &error) {
    std::cout << Json{{"status", "FAIL"},
                      {"code", error.code},
                      {"stage", error.stage},
                      {"hresult", error.native},
                      {"message", error.what()}}
                     .dump(2)
              << '\n';
    return 1;
  } catch (const std::exception &error) {
    std::cout << Json{{"status", "FAIL"}, {"message", error.what()}}.dump(2)
              << '\n';
    return 1;
  }
}
