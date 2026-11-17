#include "jobs/project_export.hpp"
#include <fstream>
#include <iostream>

using namespace nss;
namespace {
void require(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
template <class Action> void rejects(Action action) {
  try {
    action();
  } catch (const std::exception &) {
    return;
  }
  throw std::runtime_error("Invalid project export admitted");
}
Json video(const std::filesystem::path &root, const char *role,
           const char *name) {
  const auto path = root / name;
  std::ofstream(path, std::ios::binary) << std::string(128, 'x');
  return {{"role", role},
          {"path", path.string()},
          {"video",
           {{"width", 1280}, {"height", 720}, {"fps", 30}, {"crop", nullptr}}},
          {"audio", nullptr}};
}
Json audio(const std::filesystem::path &root, const char *role,
           const char *name, int offset = 0) {
  const auto path = root / name;
  std::ofstream(path, std::ios::binary) << std::string(128, 'x');
  return {{"role", role},
          {"path", path.string()},
          {"video", nullptr},
          {"audio", {{"offsetMs", offset}, {"channels", 2}}}};
}
Json base(const std::filesystem::path &root) {
  return {{"version", 1},
          {"layout", "ScreenOnly"},
          {"container", "Mp4"},
          {"quality", "Balanced"},
          {"durationHns", 30000000},
          {"fps", 30},
          {"legacyDuration", false},
          {"scene", nullptr},
          {"inputs", Json::array({video(root, "screen", "screen file.mkv")})}};
}
std::wstring filter(const jobs::ProjectExportPlan &plan) {
  for (size_t i = 0; i + 1 < plan.arguments.size(); ++i)
    if (plan.arguments[i] == L"-filter_complex")
      return plan.arguments[i + 1];
  return {};
}
} // namespace
int main(int argc, char **argv) {
  try {
    if (argc != 3)
      return 2;
    const std::string scope = argv[1];
    const auto root =
        std::filesystem::absolute(std::filesystem::path(argv[2]) / scope);
    std::filesystem::create_directories(root);
    auto value = base(root);
    if (scope == "modes") {
      auto plan = jobs::project_export(value);
      require(plan.video && !plan.audio && !plan.reencode &&
                  plan.container == "mp4",
              "H264 copy selection failed");
      value["layout"] = "WebcamOnly";
      rejects([&] { jobs::project_export(value); });
      value["inputs"] = Json::array({video(root, "camera", "camera'&.mp4")});
      plan = jobs::project_export(value);
      require(!plan.reencode, "Independent camera requires no transformation");
      value["layout"] = "NoVideo";
      value["container"] = "Wav";
      value["quality"] = "Wav48000Hz24Bit";
      value["inputs"] = Json::array({audio(root, "microphone", "mic.wav")});
      plan = jobs::project_export(value);
      require(!plan.video && plan.audio && plan.audio_codec == "pcm_s24le" &&
                  plan.sample_rate == 48000,
              "PCM24 route was lost");
      value["container"] = "Mp3";
      plan = jobs::project_export(value);
      require(plan.audio_codec == "mp3", "MP3 route was lost");
    } else if (scope == "bounds") {
      auto bad = value;
      bad["version"] = 2;
      rejects([&] { jobs::project_export(bad); });
      bad = value;
      bad["durationHns"] = 0;
      rejects([&] { jobs::project_export(bad); });
      bad = value;
      bad["container"] = "Wav";
      rejects([&] { jobs::project_export(bad); });
      bad = value;
      bad["shellArguments"] = "injected";
      rejects([&] { jobs::project_export(bad); });
      bad = value;
      bad["inputs"].push_back(bad["inputs"][0]);
      rejects([&] { jobs::project_export(bad); });
      bad = value;
      bad["inputs"][0]["path"] = "relative.mkv";
      rejects([&] { jobs::project_export(bad); });
      bad = value;
      bad["inputs"][0]["video"]["width"] = 20000;
      rejects([&] { jobs::project_export(bad); });
    } else if (scope == "audio") {
      value["inputs"].push_back(audio(root, "microphone", "microphone.wav"));
      value["inputs"].push_back(audio(root, "system", "system.wav", 500));
      auto plan = jobs::project_export(value);
      require(plan.audio && plan.gain == .5,
              "Two-source headroom is not bounded");
      const auto filters = filter(plan);
      require(filters.find(L"volume=0.5000000") != std::wstring::npos &&
                  filters.find(L"adelay=500:all=1") != std::wstring::npos &&
                  filters.find(L"apad,atrim=duration=3.0000000") !=
                      std::wstring::npos,
              "Alignment/silence policy is missing");
      value["inputs"].erase(2);
      plan = jobs::project_export(value);
      require(plan.gain == 1, "A single source was attenuated");
      value["inputs"][1]["audio"]["offsetMs"] = -1;
      rejects([&] { jobs::project_export(value); });
    } else if (scope == "geometry") {
      value["layout"] = "ScreenWebcamSideBySide";
      value["inputs"].push_back(video(root, "camera", "camera.mp4"));
      value["scene"] = {
          {"version", 1},
          {"width", 1280},
          {"height", 720},
          {"layout", "SideBySide"},
          {"layers",
           Json::array(
               {{{"sourceId", 1},
                 {"destination",
                  {{"x", 0}, {"y", 0}, {"width", .5}, {"height", 1}}}},
                {{"sourceId", 2},
                 {"mirror", true},
                 {"rotation", 90},
                 {"opacity", .5},
                 {"destination",
                  {{"x", .5}, {"y", 0}, {"width", .5}, {"height", 1}}}}})}};
      auto plan = jobs::project_export(value);
      auto filters = filter(plan);
      require(plan.reencode && plan.width == 1280 && plan.height == 720,
              "Canonical canvas is missing");
      require(filters.find(L"transpose=clock,hflip") != std::wstring::npos &&
                  filters.find(L"colorchannelmixer=aa=0.5000000") !=
                      std::wstring::npos,
              "Saved orientation/alpha was lost");
      require(filters.find(L"scale=640:360") != std::wstring::npos,
              "Screen aspect ratio is wrong");
      value["scene"]["fpsNum"] = 30000;
      value["scene"]["fpsDen"] = 1001;
      plan = jobs::project_export(value);
      require(plan.fps_num == 30000 && plan.fps_den == 1001 &&
                  filter(plan).find(L"fps=30000/1001") != std::wstring::npos,
              "Rational scene cadence was truncated");
      value["scene"]["width"] = 1279;
      rejects([&] { jobs::project_export(value); });
      value["scene"] = nullptr;
      value["inputs"][0]["video"]["width"] = 1920;
      value["inputs"][0]["video"]["height"] = 1200;
      plan = jobs::project_export(value);
      require(
          plan.width == 4054 && plan.height == 1200,
          "Legacy 1920x1200 + 1280x720 side-by-side compatibility was lost");
    } else
      return 2;
    std::cout << Json{{"status", "PASS"},
                      {"scope", scope},
                      {"method", "Actual native export planner; bounded "
                                 "geometry/audio/route/argument contracts, no "
                                 "fabricated media"}}
                     .dump()
              << '\n';
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
