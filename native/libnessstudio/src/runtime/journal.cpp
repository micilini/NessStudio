#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "journal.hpp"
#include <atomic>
#include <windows.h>

namespace nss {
namespace {
uint64_t value(FILETIME time) {
  return (uint64_t(time.dwHighDateTime) << 32) | time.dwLowDateTime;
}
[[noreturn]] void failed(const char *message, DWORD error) {
  if (!error)
    error = ERROR_WRITE_FAULT;
  throw JournalError(message, static_cast<int32_t>(HRESULT_FROM_WIN32(error)));
}
std::wstring windows_path(const std::filesystem::path &path) {
  auto normalized = path.lexically_normal();
  normalized.make_preferred();
  const auto &text = normalized.native();
  if (text.starts_with(L"\\\\?\\"))
    return text;
  if (text.starts_with(L"\\\\"))
    return L"\\\\?\\UNC\\" + text.substr(2);
  return L"\\\\?\\" + text;
}
std::filesystem::path from_utf8(const std::string &text) {
  if (text.empty() || text.size() > 32760 ||
      text.find('\0') != std::string::npos)
    throw JournalError("Invalid journal path budget", E_INVALIDARG);
  const int count =
      MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                          static_cast<int>(text.size()), nullptr, 0);
  if (!count)
    failed("Invalid UTF8 journal path", GetLastError());
  std::wstring result(static_cast<size_t>(count), L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                      static_cast<int>(text.size()), result.data(), count);
  return std::filesystem::path(result);
}
} // namespace
Journal::Journal(std::filesystem::path path, uint64_t object_id,
                 std::string operation)
    : path_(std::move(path)), object_id_(object_id),
      operation_(std::move(operation)) {
  if (!path_.is_absolute())
    throw JournalError("Journal requires an absolute path", E_INVALIDARG);
  FILETIME created{}, exited{}, kernel{}, user{};
  if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user))
    failed("Cannot identify journal owner", GetLastError());
  process_started_ = value(created);
}
std::filesystem::path Journal::recording_path(const nlohmann::json &request,
                                              uint64_t object_id) {
  if (request.value("previewOnly", false) || request.contains("testFixture"))
    return {};
  const char *field =
      request.value("recordingMode", std::string{}) == "Program" &&
              request.contains("programOutputPath")
          ? "programOutputPath"
      : request.contains("outputPath") ? "outputPath"
                                       : "manifestPath";
  if (!request.contains(field))
    return {};
  auto output = from_utf8(request.at(field).get<std::string>());
  return output.native() + L".session-" + std::to_wstring(object_id) + L".json";
}
std::filesystem::path Journal::job_path(const std::filesystem::path &output,
                                        uint64_t object_id) {
  return output.native() + L".job-" + std::to_wstring(object_id) + L".json";
}
void Journal::store(nlohmann::json snapshot) {
  FILETIME now{};
  GetSystemTimeAsFileTime(&now);
  nlohmann::json document{{"version", 1},
                          {"kind", "nessstudio.operation-journal"},
                          {"operation", operation_},
                          {"objectId", object_id_},
                          {"processId", GetCurrentProcessId()},
                          {"processStartedFileTimeUtc", process_started_},
                          {"updatedFileTimeUtc", value(now)},
                          {"sequence", sequence_ + 1},
                          {"sourceAssetsPreserved", true},
                          {"snapshot", std::move(snapshot)}};
  const std::string text = document.dump(2) + "\n";
  if (text.size() > 2 * 1024 * 1024)
    throw JournalError("Journal exceeds its 2 MiB budget", E_INVALIDARG);
  if (!reserved_) {
    const HANDLE reservation =
        CreateFileW(windows_path(path_).c_str(), GENERIC_WRITE, 0, nullptr,
                    CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (reservation == INVALID_HANDLE_VALUE)
      failed("Cannot reserve operation journal; existing files were preserved",
             GetLastError());
    const bool closed = CloseHandle(reservation) != FALSE;
    if (!closed)
      failed("Cannot close journal reservation", GetLastError());
    reserved_ = true;
  }
  static std::atomic<uint64_t> nonce{};
  const auto pending = std::filesystem::path(path_.native() + L".pending-" +
                                             std::to_wstring(process_started_) +
                                             L"-" + std::to_wstring(++nonce));
  const HANDLE file =
      CreateFileW(windows_path(pending).c_str(), GENERIC_WRITE, 0, nullptr,
                  CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE)
    failed("Cannot reserve journal checkpoint", GetLastError());
  DWORD written{};
  const bool saved =
      WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written,
                nullptr) &&
      written == text.size() && FlushFileBuffers(file);
  const DWORD error = GetLastError();
  const bool closed = CloseHandle(file) != FALSE;
  if (!saved)
    failed(
        "Cannot flush journal checkpoint; the previous checkpoint is preserved",
        error);
  if (!closed)
    failed("Cannot close journal checkpoint", GetLastError());
  if (!MoveFileExW(windows_path(pending).c_str(), windows_path(path_).c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    failed("Cannot publish journal checkpoint; the previous checkpoint and "
           "owned pending file are preserved",
           GetLastError());
  ++sequence_;
}
} // namespace nss
