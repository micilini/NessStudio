#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "runtime/journal.hpp"
#include <fstream>
#include <iostream>
#include <windows.h>

using nlohmann::json;
static json read(const std::filesystem::path &path) {
  std::ifstream stream(path);
  return json::parse(stream);
}
static void require(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
int wmain(int argc, wchar_t **argv) {
  try {
    if (argc != 3)
      return 2;
    const std::wstring scope(argv[1]);
    const auto root = std::filesystem::path(argv[2]) /
                      (scope + L"-" + std::to_wstring(GetCurrentProcessId()) +
                       L"-" + std::to_wstring(GetTickCount64()));
    std::filesystem::create_directories(root);
    const auto path = root / L"screen 'unicode-ç'.mkv.session-1.json";
    if (scope == L"identity") {
      nss::Journal journal(path, 42, "recording");
      journal.store({{"state", 4}, {"mediaComplete", false}});
      auto value = read(path);
      require(value["processId"] == GetCurrentProcessId() &&
                  value["processStartedFileTimeUtc"].get<uint64_t>() > 0 &&
                  value["objectId"] == 42 && value["sequence"] == 1,
              "Journal owner/identity was lost");
      journal.store(
          {{"state", 10}, {"mediaComplete", true}, {"cleanupPending", false}});
      value = read(path);
      require(value["sequence"] == 2 &&
                  value["snapshot"]["mediaComplete"] == true,
              "Atomic checkpoint did not advance");
      auto target = nss::Journal::recording_path(
          {{"outputPath", "C:/owned/screen.mkv"}}, 42);
      require(target.filename() == L"screen.mkv.session-42.json",
              "Recording journal is not asset-specific");
      require(nss::Journal::recording_path({{"previewOnly", true}}, 42).empty(),
              "Idle preview acquired a recording journal");
    } else if (scope == L"long-path") {
      auto directory = root;
      while (directory.native().size() < 280)
        directory /= L"owned-long-component";
      const auto extended = [](const std::filesystem::path &file) {
        auto normalized = file;
        normalized.make_preferred();
        return std::filesystem::path(L"\\\\?\\" + normalized.native());
      };
      std::filesystem::create_directories(extended(directory));
      const auto long_path = directory / L"journal.json";
      nss::Journal journal(long_path, 42, "recording");
      journal.store({{"state", 4}});
      journal.store({{"state", 10}, {"mediaComplete", true}});
      require(long_path.native().size() > MAX_PATH &&
                  read(extended(long_path))["sequence"] == 2,
              "Long-path journal checkpoint failed");
    } else if (scope == L"collision") {
      {
        std::ofstream stream(path);
        stream << "preserve this unrelated file";
      }
      bool rejected{};
      try {
        nss::Journal journal(path, 42, "recording");
        journal.store({{"state", 4}});
      } catch (const nss::JournalError &) {
        rejected = true;
      }
      std::ifstream stream(path);
      const std::string text((std::istreambuf_iterator<char>(stream)), {});
      require(rejected && text == "preserve this unrelated file",
              "Journal replaced an existing unowned file");
    } else if (scope == L"promotion") {
      nss::Journal journal(path, 42, "recording");
      journal.store({{"state", 4}});
      const auto before = root / L"original-checkpoint.json";
      std::filesystem::rename(path, before);
      std::filesystem::create_directory(path);
      bool rejected{};
      try {
        journal.store({{"state", 10}});
      } catch (const nss::JournalError &) {
        rejected = true;
      }
      size_t pending{};
      for (const auto &item : std::filesystem::directory_iterator(root))
        if (item.path().filename().native().find(L".pending-") !=
            std::wstring::npos)
          ++pending;
      require(rejected && read(before)["snapshot"]["state"] == 4 &&
                  pending == 1,
              "Failed publication lost its previous or pending checkpoint");
    } else
      return 2;
    std::cout << json{{"status", "PASS"},
                      {"method",
                       "Real owned NTFS files, flushed atomic checkpoints, "
                       "owner identity, collision and publication failure"}}
                     .dump()
              << '\n';
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
