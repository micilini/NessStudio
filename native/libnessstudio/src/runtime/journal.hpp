#pragma once
#include "json.hpp"
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>

namespace nss {
class JournalError final : public std::runtime_error {
public:
  int32_t native;
  JournalError(const char *message, int32_t error)
      : std::runtime_error(message), native(error) {}
};

// One worker owns each journal. Files are reserved before media I/O, flushed,
// and replaced using owned, unique checkpoint files in the same directory.
class Journal final {
  std::filesystem::path path_;
  uint64_t object_id_{}, process_started_{}, sequence_{};
  std::string operation_;
  bool reserved_{};

public:
  Journal(std::filesystem::path path, uint64_t object_id,
          std::string operation);
  void store(nlohmann::json snapshot);
  static std::filesystem::path recording_path(const nlohmann::json &request,
                                              uint64_t object_id);
  static std::filesystem::path job_path(const std::filesystem::path &output,
                                        uint64_t object_id);
};
} // namespace nss
