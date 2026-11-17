#pragma once
#include "runtime/session.hpp"
#include <filesystem>

namespace nss::jobs {
struct ProjectExportPlan {
  std::vector<std::wstring> arguments;
  std::vector<std::filesystem::path> inputs;
  int64_t duration{};
  uint32_t fps{30}, fps_num{30}, fps_den{1}, width{}, height{},
      sample_rate{48000};
  bool video{}, audio{}, reencode{}, legacy_duration{};
  double gain{1};
  std::string container, video_codec, audio_codec;
  Json effective() const;
};
ProjectExportPlan project_export(const Json &project);
} // namespace nss::jobs
