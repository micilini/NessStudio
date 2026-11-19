#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "job.hpp"
#include "capture/capture.hpp"
#include "project_export.hpp"
#include "runtime/journal.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <utility>
#include <windows.h>

namespace nss::jobs {
namespace {
using capture::Error;
std::wstring wide(const std::string &s) {
  if (s.empty() || s.size() > 32760 || s.find('\0') != std::string::npos)
    throw std::invalid_argument("Invalid job path");
  const auto n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(),
                                     static_cast<int>(s.size()), nullptr, 0);
  if (!n)
    throw std::invalid_argument("Invalid job UTF8");
  std::wstring r(static_cast<size_t>(n), 0);
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(),
                      static_cast<int>(s.size()), r.data(), n);
  return r;
}
std::string utf8(const std::wstring &s) {
  const auto n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s.data(),
                                     static_cast<int>(s.size()), nullptr, 0,
                                     nullptr, nullptr);
  if (!n)
    throw std::invalid_argument("Invalid job UTF16");
  std::string r(static_cast<size_t>(n), 0);
  WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s.data(),
                      static_cast<int>(s.size()), r.data(), n, nullptr,
                      nullptr);
  return r;
}
std::filesystem::path path(const std::string &s) {
  return std::filesystem::path(wide(s));
}
std::string diagnostic_text(const std::string &raw) {
  if (raw.empty())
    return {};
  const auto n = MultiByteToWideChar(CP_UTF8, 0, raw.data(),
                                     static_cast<int>(raw.size()), nullptr, 0);
  if (!n)
    return "[unavailable diagnostic encoding]";
  std::wstring decoded(static_cast<size_t>(n), 0);
  MultiByteToWideChar(CP_UTF8, 0, raw.data(), static_cast<int>(raw.size()),
                      decoded.data(), n);
  auto result = utf8(decoded);
  if (result.size() > 65536) {
    auto at = result.size() - 65536;
    while (at < result.size() &&
           (static_cast<unsigned char>(result[at]) & 0xc0) == 0x80)
      ++at;
    result.erase(0, at);
  }
  return result;
}
// CommandLineToArgvW / CRT quoting: backslashes preceding quotes or the closing
// quote double.
std::wstring quote(const std::wstring &s) {
  std::wstring r = L"\"";
  size_t slashes{};
  for (const auto c : s) {
    if (c == L'\\') {
      ++slashes;
      continue;
    }
    if (c == L'\"') {
      r.append(slashes * 2 + 1, L'\\');
      r += c;
    } else {
      r.append(slashes, L'\\');
      r += c;
    }
    slashes = 0;
  }
  r.append(slashes * 2, L'\\');
  r += L'\"';
  return r;
}
struct Handle {
  HANDLE h{};
  explicit Handle(HANDLE value = nullptr) : h(value) {}
  ~Handle() {
    if (h && h != INVALID_HANDLE_VALUE)
      CloseHandle(h);
  }
  Handle(const Handle &) = delete;
  Handle &operator=(const Handle &) = delete;
};
Json read_json(const std::filesystem::path &p) {
  if (!p.is_absolute() || !std::filesystem::is_regular_file(p) ||
      std::filesystem::file_size(p) > NSS_MAX_JSON_BYTES)
    throw std::invalid_argument(
        "Manifest requires a bounded absolute regular file");
  std::ifstream f(p, std::ios::binary);
  std::string bytes(NSS_MAX_JSON_BYTES + 1, '\0');
  f.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  bytes.resize(static_cast<size_t>(f.gcount()));
  if (bytes.size() > NSS_MAX_JSON_BYTES)
    throw std::invalid_argument("Manifest byte budget exceeded");
  return Json::parse(bytes, [](int depth, Json::parse_event_t, Json &) {
    if (depth > 64)
      throw std::invalid_argument("Manifest nesting budget exceeded");
    return true;
  });
}
void write_json(const std::filesystem::path &p, const Json &value) {
  const auto bytes = value.dump(2) + "\n";
  Handle file(CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                          FILE_ATTRIBUTE_NORMAL, nullptr));
  DWORD written{};
  if (file.h == INVALID_HANDLE_VALUE ||
      !WriteFile(file.h, bytes.data(), static_cast<DWORD>(bytes.size()),
                 &written, nullptr) ||
      written != bytes.size() || !FlushFileBuffers(file.h))
    throw Error(NSS_E_IO, NSS_STAGE_STORAGE, HRESULT_FROM_WIN32(GetLastError()),
                "Export receipt write failed");
  const auto h = std::exchange(file.h, nullptr);
  if (!CloseHandle(h))
    throw Error(NSS_E_IO, NSS_STAGE_STORAGE, HRESULT_FROM_WIN32(GetLastError()),
                "Export receipt close failed");
}
void new_path(const std::filesystem::path &p) {
  if (!p.is_absolute() || !std::filesystem::is_directory(p.parent_path()) ||
      std::filesystem::exists(p))
    throw std::invalid_argument("Job outputs require new absolute paths");
}
std::pair<uint32_t,uint32_t> png_dimensions(const std::filesystem::path &p) {
  std::ifstream input(p,std::ios::binary);std::array<unsigned char,33> header{};
  input.read(reinterpret_cast<char *>(header.data()),static_cast<std::streamsize>(header.size()));
  constexpr std::array<unsigned char,8> signature{137,80,78,71,13,10,26,10};
  if(input.gcount()!=static_cast<std::streamsize>(header.size()) ||
     !std::equal(signature.begin(),signature.end(),header.begin()) ||
     header[8] || header[9] || header[10] || header[11]!=13 || std::memcmp(header.data()+12,"IHDR",4))
    throw Error(NSS_E_IO,NSS_STAGE_JOB,0,"Thumbnail does not contain a valid PNG header");
  auto number=[&](size_t at) {return (uint32_t(header[at])<<24)|(uint32_t(header[at+1])<<16)|
    (uint32_t(header[at+2])<<8)|header[at+3];};
  const auto width=number(16),height=number(20);
  if(!width || !height || width>640 || height>360)
    throw Error(NSS_E_IO,NSS_STAGE_JOB,0,"Thumbnail exceeds its image dimensions budget");
  return {width,height};
}
} // namespace
struct Job::Impl {
  Json request, manifest;
  std::shared_ptr<std::atomic<bool>> budget;
  mutable std::mutex mutex;
  std::condition_variable changed;
  std::thread worker;
  std::deque<nss_event_v1> events;
  std::atomic<bool> cancelled{};
  bool polling{}, interrupted{}, started{}, released{}, completion_claimed{};
  uint32_t state{NSS_JOB_CREATED};
  uint64_t sequence{}, id{}, next_command{1}, start_id{}, cancel_id{};
  int64_t duration{}, progress{};
  int64_t process_time{};
  uint64_t process_frames{};
  Fault fault;
  DWORD exit_code{STILL_ACTIVE}, validation_exit_code{STILL_ACTIVE};
  std::string log, validation_log, codec_log;
  std::filesystem::path output, partial, receipt, executable, video;
  std::string profile;
  int64_t thumbnail_seek{},thumbnail_effective_seek{};
  uint32_t thumbnail_width{},thumbnail_height{};
  Json thumbnail_attempts=Json::array();
  std::vector<std::filesystem::path> audio;
  std::optional<ProjectExportPlan> project_plan;
  std::filesystem::path journal_path;
  std::unique_ptr<Journal> journal;
  Json secondary = Json::array();
  explicit Impl(Json value, std::shared_ptr<std::atomic<bool>> b)
      : request(std::move(value)), budget(std::move(b)) {
    profile = request.value("profile", "");
    if (profile == "thumbnail-png") {
      if(!request.is_object() || request.size()!=7 || request.at("version")!=1 || request.at("type")!="thumbnail")
        throw std::invalid_argument("Thumbnail requires its versioned local-file description");
      for(const auto &field:request.items())
        if(field.key()!="version" && field.key()!="type" && field.key()!="profile" &&
           field.key()!="inputPath" && field.key()!="outputPath" && field.key()!="ffmpegPath" && field.key()!="seekHns")
          throw std::invalid_argument("Unknown thumbnail field");
      if(!request.at("seekHns").is_number_integer()) throw std::invalid_argument("Thumbnail seek must use integer HNS");
      thumbnail_seek=request.at("seekHns").get<int64_t>();
      if(thumbnail_seek<0 || thumbnail_seek>maximum_session_hns)
        throw std::invalid_argument("Thumbnail seek exceeds its domain");
      video=path(request.at("inputPath"));
      if(!video.is_absolute() || !std::filesystem::is_regular_file(video) || !std::filesystem::file_size(video))
        throw std::invalid_argument("Thumbnail requires an existing nonempty local video asset");
      duration=hns_per_second; // A bounded job progress unit, not a video duration.
    } else if (profile == "project-export") {
      if (!request.is_object() || request.size() != 6 ||
          request.at("version") != 1 || request.at("type") != "export")
        throw std::invalid_argument(
            "Project export requires its versioned description");
      for (const auto &field : request.items())
        if (field.key() != "version" && field.key() != "type" &&
            field.key() != "profile" && field.key() != "project" &&
            field.key() != "ffmpegPath" && field.key() != "outputPath")
          throw std::invalid_argument("Unknown project export field");
      project_plan = project_export(request.at("project"));
      duration = project_plan->duration;
    } else {
      if (!request.is_object() || request.size() != 6 ||
          request.at("version") != 1 ||
          (profile != "av-basic" && profile != "camera-remux" &&
           profile != "audio-basic"))
        throw std::invalid_argument("Job requires version 1, av-basic, "
                                    "sessionManifestPath, ffmpegPath, "
                                    "outputPath and profile");
      for (const auto &v : request.items())
        if (v.key() != "version" && v.key() != "profile" &&
            v.key() != "sessionManifestPath" && v.key() != "ffmpegPath" &&
            v.key() != "outputPath" && v.key() != "type")
          throw std::invalid_argument("Unknown export job field");
      if (request.at("type") != "export")
        throw std::invalid_argument("Unsupported job type");
      manifest = read_json(path(request.at("sessionManifestPath")));
      if (manifest.at("version") != 1 || manifest.at("mediaComplete") != true ||
          manifest.at("finalized") != true ||
          (profile == "audio-basic"
               ? manifest.at("kind") != "audio-only"
               : manifest.at("codec") != "h264" ||
                     manifest.at("container") != "matroska"))
        throw std::invalid_argument(
            "Export requires a complete H264 recording manifest");
      duration = manifest.at("durationHns").get<int64_t>();
      if (duration < 1 || duration > maximum_session_hns)
        throw std::invalid_argument("Invalid asset duration");
      if (profile != "audio-basic") {
        video = path(manifest.at("outputPath"));
        if (!video.is_absolute() || !std::filesystem::is_regular_file(video) ||
            std::filesystem::file_size(video) !=
                manifest.at("bytes").get<uint64_t>())
          throw std::invalid_argument("Video asset is incomplete or changed");
      }
      const auto tracks = manifest.at("audio");
      if (!tracks.is_array() || tracks.size() > 2 ||
          (profile != "camera-remux" && tracks.empty()))
        throw std::invalid_argument(
            "av-basic requires one or two finalized audio assets");
      for (const auto &track : tracks) {
        if (track.at("finalized") != true || track.at("outputRate") != 48000 ||
            track.at("outputChannels") != 2 ||
            track.at("outputSamples").get<uint64_t>() !=
                scale_floor(static_cast<uint64_t>(duration), 48000,
                            hns_per_second))
          throw std::invalid_argument(
              "Audio asset alignment/format is invalid");
        const auto p = path(track.at("path"));
        if (!p.is_absolute() || !std::filesystem::is_regular_file(p) ||
            std::filesystem::file_size(p) !=
                80 + track.at("outputSamples").get<uint64_t>() * 4)
          throw std::invalid_argument("Audio asset is incomplete or changed");
        std::ifstream f(p, std::ios::binary);
        std::array<char, 80> header{};
        f.read(header.data(), 80);
        if (f.gcount() != 80 || std::memcmp(header.data(), "RF64", 4) ||
            std::memcmp(header.data() + 72, "data", 4))
          throw std::invalid_argument("Invalid RF64 audio asset");
        auto u16 = [&](size_t at) {
          uint16_t v{};
          std::memcpy(&v, header.data() + at, 2);
          return v;
        };
        auto u32 = [&](size_t at) {
          uint32_t v{};
          std::memcpy(&v, header.data() + at, 4);
          return v;
        };
        auto u64 = [&](size_t at) {
          uint64_t v{};
          std::memcpy(&v, header.data() + at, 8);
          return v;
        };
        const auto samples = track.at("outputSamples").get<uint64_t>();
        if (std::memcmp(header.data() + 12, "ds64", 4) || u32(16) != 28 ||
            u64(20) != 72 + samples * 4 || u64(28) != samples * 4 ||
            u64(36) != samples || std::memcmp(header.data() + 48, "fmt ", 4) ||
            u32(52) != 16 || u16(56) != 1 || u16(58) != 2 || u32(60) != 48000 ||
            u32(64) != 192000 || u16(68) != 4 || u16(70) != 16)
          throw std::invalid_argument(
              "RF64 header does not describe finalized PCM16 stereo 48kHz");
        audio.push_back(p);
      }
      if (profile == "camera-remux")
        audio.clear();
    }
    executable = path(request.at("ffmpegPath"));
    DWORD type{};
    if (!executable.is_absolute() ||
        !GetBinaryTypeW(executable.c_str(), &type) || type != SCS_64BIT_BINARY)
      throw std::invalid_argument(
          "Export requires an explicit packaged x64 FFmpeg executable");
    output = path(request.at("outputPath"));
    const auto extension = profile=="thumbnail-png" ? L".png" :
        project_plan ? (project_plan->container == "matroska" ? L".mkv"
                        : project_plan->container == "mp4"    ? L".mp4"
                        : project_plan->container == "mp3"    ? L".mp3"
                                                              : L".wav")
                     : (profile == "camera-remux" ? L".mp4" : L".mkv");
    if (output.extension() != extension)
      throw std::invalid_argument("Job output extension does not match the requested profile");
    partial = output.native() + L".partial" + extension;
    receipt = output.native() + (profile=="thumbnail-png" ? L".thumbnail.json" : L".export.json");
    new_path(output);
    new_path(partial);
    new_path(receipt);
    new_path(receipt.native() + L".pending");
    static std::atomic<uint64_t> ids{1};
    id = ids++;
    journal_path = Journal::job_path(output, id);
  }
  void checkpoint(uint32_t state_override = 0) {
    if (!journal)
      journal = std::make_unique<Journal>(journal_path, id, "export");
    Json value;
    {
      std::lock_guard lock(mutex);
      const auto saved_state = state_override ? state_override : state;
      value = {{"version", 1},
               {"state", saved_state},
               {"requested", request},
               {"outputPath", utf8(output.native())},
               {"outputBytes", saved_state == NSS_JOB_COMPLETED
                                   ? Json(std::filesystem::file_size(output))
                                   : Json(nullptr)},
               {"partialPath", utf8(partial.native())},
               {"receiptPath", utf8(receipt.native())},
               {"activeTimeHns", progress},
               {"mediaComplete", saved_state == NSS_JOB_COMPLETED},
               {"cleanupPending", saved_state != NSS_JOB_COMPLETED &&
                                      saved_state != NSS_JOB_FAILED &&
                                      saved_state != NSS_JOB_CANCELLED},
               {"firstError", fault ? Json{{"code", fault.code},
                                           {"stage", fault.stage},
                                           {"hresult", fault.hresult},
                                           {"message", fault.message}}
                                    : Json(nullptr)},
               {"secondaryErrors", secondary}};
    }
    try {
      journal->store(std::move(value));
    } catch (const JournalError &error) {
      throw Error(NSS_E_IO, NSS_STAGE_STORAGE, error.native, error.what());
    }
  }
  void preserve_terminal_checkpoint(uint32_t state_override = 0) noexcept {
    try {
      checkpoint(state_override);
    } catch (const std::exception &error) {
      try {
        std::lock_guard lock(mutex);
        if (secondary.size() < 8)
          secondary.push_back({{"code", NSS_E_IO},
                               {"stage", NSS_STAGE_STORAGE},
                               {"hresult", dynamic_cast<const Error *>(&error)
                                               ? dynamic_cast<const Error *>(&error)->native
                                               : 0},
                               {"message", error.what()}});
      } catch (...) {
      }
    }
  }
  void emit(uint64_t command, nss_result status) {
    nss_event_v1 e{};
    e.size = sizeof(e);
    e.version = 1;
    e.sequence = ++sequence;
    e.object_id = id;
    e.command_id = command;
    e.kind = command || !fault ? NSS_EVENT_RESULT : NSS_EVENT_FAULT;
    e.state = state;
    e.status = status;
    e.active_time_hns = progress;
    e.stage = status < 0 ? fault.stage : 0;
    e.hresult = status < 0 ? fault.hresult : 0;
    const auto message =
        status < 0 ? fault.message.substr(0, 255) : std::string{};
    std::memcpy(e.message_utf8, message.data(), message.size());
    // Only state transitions/results enter this bounded queue; progress is a
    // snapshot.
    if (events.size() == 16)
      events.pop_front();
    events.push_back(e);
    changed.notify_all();
  }
  void change(uint32_t value) {
    {
      std::lock_guard lock(mutex);
      if (cancelled)
        throw Error(NSS_E_CANCELLED, NSS_STAGE_JOB, 0,
                    "Export cancelled before state transition");
      state = value;
      emit(0, NSS_OK);
    }
    checkpoint();
  }
  void release_budget() {
    if (!released) {
      *budget = false;
      released = true;
    }
  }
  DWORD process(const std::vector<std::wstring> &args,
                std::string &destination) {
    if (cancelled)
      throw Error(NSS_E_CANCELLED, NSS_STAGE_JOB, 0,
                  "Export cancelled before child creation");
    process_time = 0;
    process_frames = 0;
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE read{}, write{};
    if (!CreatePipe(&read, &write, &sa, 0))
      throw Error(NSS_E_IO, NSS_STAGE_JOB, HRESULT_FROM_WIN32(GetLastError()),
                  "Export pipe allocation failed");
    Handle reader(read), writer(write);
    SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0);
    Handle input(CreateFileW(L"NUL", GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                             OPEN_EXISTING, 0, nullptr));
    Handle group(CreateJobObjectW(nullptr, nullptr));
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
    limits.BasicLimitInformation.ActiveProcessLimit = 1;
    if (!group.h ||
        !SetInformationJobObject(group.h, JobObjectExtendedLimitInformation,
                                 &limits, sizeof(limits)))
      throw Error(NSS_E_IO, NSS_STAGE_JOB, HRESULT_FROM_WIN32(GetLastError()),
                  "Export process supervision unavailable");
    std::wstring command = quote(executable.native());
    for (const auto &arg : args) {
      command += L' ';
      command += quote(arg);
    }
    if (command.size() > 32760)
      throw std::invalid_argument("FFmpeg argv exceeds Windows budget");
    SIZE_T bytes{};
    InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
    std::vector<uint8_t> storage(bytes);
    auto *attributes =
        reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &bytes))
      throw Error(NSS_E_IO, NSS_STAGE_JOB, HRESULT_FROM_WIN32(GetLastError()),
                  "Export handle list unavailable");
    struct AttributeOwner {
      LPPROC_THREAD_ATTRIBUTE_LIST p;
      ~AttributeOwner() { DeleteProcThreadAttributeList(p); }
    } attributes_owner{attributes};
    const std::array<HANDLE, 2> handles{write, input.h};
    if (!UpdateProcThreadAttribute(attributes, 0,
                                   PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                   const_cast<HANDLE *>(handles.data()),
                                   sizeof(handles), nullptr, nullptr))
      throw Error(NSS_E_IO, NSS_STAGE_JOB, HRESULT_FROM_WIN32(GetLastError()),
                  "Export handle inheritance failed");
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.lpAttributeList = attributes;
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = input.h;
    startup.StartupInfo.hStdOutput = write;
    startup.StartupInfo.hStdError = write;
    PROCESS_INFORMATION child{};
    if (!CreateProcessW(
            executable.c_str(), command.data(), nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT,
            nullptr, output.parent_path().c_str(), &startup.StartupInfo,
            &child))
      throw Error(NSS_E_IO, NSS_STAGE_JOB, HRESULT_FROM_WIN32(GetLastError()),
                  "FFmpeg process creation failed");
    Handle proc(child.hProcess), thread(child.hThread);
    ++children;
    struct ChildOwner {
      HANDLE process;
      ~ChildOwner() {
        if (WaitForSingleObject(process, 0) != WAIT_OBJECT_0) {
          TerminateProcess(process, ERROR_CANCELLED);
          WaitForSingleObject(process, 5000);
        }
        --children;
      }
    } child_owner{proc.h};
    if (!AssignProcessToJobObject(group.h, proc.h)) {
      TerminateProcess(proc.h, 1);
      WaitForSingleObject(proc.h, 5000);
      throw Error(NSS_E_IO, NSS_STAGE_JOB, HRESULT_FROM_WIN32(GetLastError()),
                  "Cannot supervise FFmpeg");
    }
    if (ResumeThread(thread.h) == DWORD(-1))
      throw Error(NSS_E_IO, NSS_STAGE_JOB, HRESULT_FROM_WIN32(GetLastError()),
                  "Cannot start supervised FFmpeg");
    CloseHandle(std::exchange(writer.h, nullptr));
    const auto deadline =
        GetTickCount64() +
        static_cast<uint64_t>(profile=="thumbnail-png" ? 10000 : (std::max)(int64_t{60000}, duration / 10000 * 4));
    std::string line;
    bool killed{};
    for (;;) {
      if (cancelled || GetTickCount64() > deadline) {
        TerminateJobObject(group.h, ERROR_CANCELLED);
        killed = true;
      }
      DWORD available{};
      if (PeekNamedPipe(read, nullptr, 0, nullptr, &available, nullptr) &&
          available) {
        std::array<char, 4096> buffer{};
        DWORD count{};
        if (ReadFile(read, buffer.data(),
                     (std::min)(available, static_cast<DWORD>(buffer.size())),
                     &count, nullptr)) {
          {
            std::lock_guard lock(mutex);
            destination.append(buffer.data(), count);
            if (destination.size() > 65536)
              destination.erase(0, destination.size() - 65536);
          }
          for (DWORD i = 0; i < count; ++i) {
            if (buffer[i] == '\n') {
              if (line.starts_with("out_time_us=")) {
                try {
                  const auto micros = std::stoll(line.substr(12));
                  if (micros < 0 || micros > maximum_session_hns / 10 + 1000000)
                    throw std::out_of_range("FFmpeg progress time budget");
                  process_time = micros * 10;
                  std::lock_guard lock(mutex);
                  progress =
                      (std::max)(progress, (std::min)(duration, process_time));
                } catch (const std::exception &) {
                }
              }
              if (line.starts_with("frame=")) {
                try {
                  process_frames = std::stoull(line.substr(6));
                } catch (const std::exception &) {
                }
              }
              line.clear();
            } else if (line.size() < 4096)
              line += buffer[i];
          }
        }
        continue;
      }
      if (WaitForSingleObject(proc.h, 5) == WAIT_OBJECT_0) {
        DWORD pending{};
        if (PeekNamedPipe(read, nullptr, 0, nullptr, &pending, nullptr) &&
            pending)
          continue;
        break;
      }
    }
    DWORD code{};
    if (!GetExitCodeProcess(proc.h, &code))
      throw Error(NSS_E_IO, NSS_STAGE_JOB, HRESULT_FROM_WIN32(GetLastError()),
                  "FFmpeg exit status unavailable");
    if (killed && !cancelled)
      throw Error(NSS_E_TIMEOUT, NSS_STAGE_JOB, 0, "FFmpeg deadline exceeded");
    return code;
  }
  void run() noexcept {
    ++workers;
    try {
      checkpoint();
      Handle reservation(CreateFileW(partial.c_str(), GENERIC_WRITE, 0, nullptr,
                                     CREATE_NEW, FILE_ATTRIBUTE_NORMAL,
                                     nullptr));
      if (reservation.h == INVALID_HANDLE_VALUE)
        throw Error(NSS_E_IO, NSS_STAGE_STORAGE,
                    HRESULT_FROM_WIN32(GetLastError()),
                    "Cannot reserve export partial");
      CloseHandle(std::exchange(reservation.h, nullptr));
      std::vector<std::wstring> args{L"-nostdin", L"-hide_banner", L"-loglevel",
                                     L"info",     L"-y",           L"-hwaccel",
                                     L"none"};
      const bool thumbnail=profile=="thumbnail-png";
      auto thumbnail_arguments=[&](int64_t seek) {
        std::wostringstream seconds;seconds.imbue(std::locale::classic());
        seconds<<std::fixed<<std::setprecision(7)<<double(seek)/hns_per_second;
        return std::vector<std::wstring>{L"-nostdin",L"-hide_banner",L"-v",L"error",L"-y",
          L"-hwaccel",L"none",L"-threads",L"2",L"-protocol_whitelist",L"file,pipe",
          L"-ss",seconds.str(),L"-i",video.native(),L"-map",L"0:v:0",L"-an",L"-sn",L"-dn",
          L"-vf",L"scale=w='min(640,iw)':h='min(360,ih)':force_original_aspect_ratio=decrease",
          L"-frames:v",L"1",L"-c:v",L"png",L"-threads:v",L"1",L"-map_metadata",L"-1",
          L"-progress",L"pipe:1",L"-f",L"image2",L"-update",L"1",partial.native()};
      };
      if(thumbnail) {
        thumbnail_effective_seek=thumbnail_seek;args=thumbnail_arguments(thumbnail_seek);
      } else if (project_plan) {
        args = project_plan->arguments;
        args.push_back(partial.native());
      } else {
        if (profile != "audio-basic")
          args.insert(args.end(), {L"-i", video.native()});
        for (const auto &a : audio) {
          args.push_back(L"-i");
          args.push_back(a.native());
        }
        if (profile != "audio-basic")
          args.insert(args.end(), {L"-map", L"0:v:0"});
        if (audio.size() == 1)
          args.insert(args.end(),
                      {L"-map", profile == "audio-basic" ? L"0:a:0" : L"1:a:0",
                       L"-af", L"volume=0.5"});
        else if (audio.size() == 2)
          args.insert(
              args.end(),
              {L"-filter_complex",
               (profile == "audio-basic" ? L"[0:a:0][1:a:0]"
                                         : L"[1:a:0][2:a:0]") +
                   std::wstring(
                       L"amix=inputs=2:duration=longest:dropout_"
                       L"transition=0:weights='0.5 0.5':normalize=0[a]"),
               L"-map", L"[a]"});
        std::wostringstream seconds;
        seconds.imbue(std::locale::classic());
        seconds << std::fixed << std::setprecision(7)
                << double(duration) / hns_per_second;
        if (profile == "camera-remux")
          args.insert(args.end(),
                      {L"-c:v", L"copy", L"-an", L"-movflags", L"+faststart"});
        else
          args.insert(args.end(), {L"-c:v", L"copy", L"-c:a", L"pcm_s16le",
                                   L"-ar", L"48000", L"-ac", L"2"});
        args.insert(args.end(),
                    {L"-t", seconds.str(), L"-map_metadata", L"-1",
                     L"-progress", L"pipe:1", L"-f",
                     profile == "camera-remux" ? L"mp4" : L"matroska",
                     partial.native()});
      }
      auto result = process(args, log);
      if(thumbnail) {
        auto bytes=std::filesystem::is_regular_file(partial)?std::filesystem::file_size(partial):0;
        thumbnail_attempts.push_back({{"seekHns",thumbnail_effective_seek},{"exitCode",result},{"bytes",bytes}});
        if(!result && !cancelled && !bytes && thumbnail_seek>0) {
          thumbnail_effective_seek=0;result=process(thumbnail_arguments(0),log);
          bytes=std::filesystem::is_regular_file(partial)?std::filesystem::file_size(partial):0;
          thumbnail_attempts.push_back({{"seekHns",0},{"exitCode",result},{"bytes",bytes}});
        }
      }
      {
        std::lock_guard lock(mutex);
        exit_code = result;
      }
      if (cancelled)
        throw Error(NSS_E_CANCELLED, NSS_STAGE_JOB, 0,
                    "Export cancelled; assets preserved");
      if (result || !std::filesystem::is_regular_file(partial) ||
          std::filesystem::file_size(partial) < (thumbnail ? 33u : 128u))
        throw Error(NSS_E_IO, NSS_STAGE_JOB, 0,
                    "FFmpeg failed; partial is not complete");
      change(NSS_JOB_FINALIZING);
      if(thumbnail) {
        const auto dimensions=png_dimensions(partial);thumbnail_width=dimensions.first;thumbnail_height=dimensions.second;
      }
      const bool has_video =
          project_plan ? project_plan->video : profile != "audio-basic";
      const bool has_audio =
          project_plan ? project_plan->audio : profile != "camera-remux" && !thumbnail;
      const std::string expected_audio =
          project_plan ? project_plan->audio_codec : "pcm_s16le";
      const uint32_t expected_rate =
          project_plan ? project_plan->sample_rate : 48000U;
      std::vector<std::wstring> codec_args{L"-nostdin", L"-v", L"error", L"-i",
                                           partial.native()};
      if (has_video)
        codec_args.insert(codec_args.end(), {L"-map", L"0:v:0"});
      if (has_audio)
        codec_args.insert(codec_args.end(), {L"-map", L"0:a:0"});
      codec_args.insert(codec_args.end(),
                        {L"-c", L"copy", L"-frames:v", L"0", L"-frames:a", L"0",
                         L"-f", L"framehash", L"-"});
      const auto codec_result = process(codec_args, codec_log);
      if (cancelled)
        throw Error(NSS_E_CANCELLED, NSS_STAGE_JOB, 0,
                    "Export cancelled during codec validation");
      const std::string audio_index = has_video ? "1" : "0";
      if (codec_result ||
          (has_video &&
           codec_log.find(thumbnail ? "#codec_id 0: png" : "#codec_id 0: h264") == std::string::npos) ||
          (has_audio && (codec_log.find("#codec_id " + audio_index + ": " +
                                        expected_audio) == std::string::npos ||
                         codec_log.find("#sample_rate " + audio_index + ": " +
                                        std::to_string(expected_rate)) ==
                             std::string::npos ||
                         codec_log.find("#channel_layout_name " + audio_index +
                                        ": stereo") == std::string::npos)))
        throw Error(
            NSS_E_IO, NSS_STAGE_JOB, 0,
            "Actual final A/V codecs/rate/channels differ from av-basic");
      if (project_plan && has_video &&
          codec_log.find(
              "#dimensions 0: " + std::to_string(project_plan->width) + "x" +
              std::to_string(project_plan->height)) == std::string::npos)
        throw Error(
            NSS_E_IO, NSS_STAGE_JOB, 0,
            "Actual exported video dimensions differ from the selected canvas");
      // A second supervised process fully decodes both required streams before
      // promotion.
      std::vector<std::wstring> validation_args{
          L"-nostdin", L"-hide_banner", L"-v", L"error",        L"-xerror",
          L"-hwaccel", L"none",         L"-i", partial.native()};
      if (has_video)
        validation_args.insert(validation_args.end(), {L"-map", L"0:v:0"});
      if (has_audio)
        validation_args.insert(validation_args.end(), {L"-map", L"0:a:0"});
      validation_args.insert(validation_args.end(),
                             {L"-progress", L"pipe:1", L"-f", L"null", L"-"});
      const auto validation = process(validation_args, validation_log);
      if (cancelled)
        throw Error(NSS_E_CANCELLED, NSS_STAGE_JOB, 0,
                    "Export cancelled during validation");
      {
        std::lock_guard lock(mutex);
        validation_exit_code = validation;
      }
      if (validation)
        throw Error(NSS_E_IO, NSS_STAGE_JOB, 0,
                    "Final A/V full decode validation failed");
      const bool bad_frames = thumbnail ? process_frames!=1 :
          project_plan
              ? (has_video &&
                 (process_frames == 0 ||
                  (!project_plan->legacy_duration &&
                   std::abs(static_cast<int64_t>(process_frames) -
                            static_cast<int64_t>(std::llround(
                                double(duration) * project_plan->fps_num /
                                project_plan->fps_den / hns_per_second))) > 2)))
              : (has_video &&
                 process_frames != manifest.at("frames").get<uint64_t>());
      const int64_t duration_tolerance =
          project_plan
              ? (project_plan->legacy_duration ? hns_per_second : 1000000)
              : 400000;
      if (bad_frames || (!thumbnail && std::abs(process_time - duration) > duration_tolerance))
        throw Error(NSS_E_IO, NSS_STAGE_JOB, 0,
                    "Final A/V frame count/duration validation failed");
      {
        std::lock_guard lock(mutex);
        if (cancelled)
          throw Error(NSS_E_CANCELLED, NSS_STAGE_JOB, 0,
                      "Export cancelled before publication");
        completion_claimed = true;
      }
      Json record{{"version", 1},
                  {"mediaComplete", true},
                  {"profile", profile},
                  {"outputPath", utf8(output.native())},
                  {"durationHns", duration},
                  {"videoCodec", profile == "audio-basic" ? Json(nullptr)
                                                          : Json("h264-copy")},
                  {"audioCodec", profile == "camera-remux" ? Json(nullptr)
                                                           : Json("pcm_s16le")},
                  {"audioSampleRate", profile == "camera-remux" ? 0 : 48000},
                  {"audioChannels", profile == "camera-remux" ? 0 : 2},
                  {"gainPerSource", profile == "camera-remux" ? 0.0 : 0.5},
                  {"normalization", false},
                  {"hardwareAcceleration", false},
                  {"fullDecodeValidated", true},
                  {"codecsRateChannelsValidated", true},
                  {"sessionManifestPath",
                   request.value("sessionManifestPath", Json(nullptr))},
                  {"ffmpegPath", request.at("ffmpegPath")},
                  {"sourceAssetsPreserved", true}};
      if (project_plan) {
        record["effective"] = project_plan->effective();
        record["requestedDurationHns"] = duration;
        record["durationHns"] = process_time;
        record["decodedVideoFrames"] = has_video ? process_frames : 0;
        record["decodedDurationHns"] = process_time;
        record["videoCodec"] =
            has_video
                ? Json(project_plan->reencode ? "h264-encode" : "h264-copy")
                : Json(nullptr);
        record["audioCodec"] = has_audio ? Json(expected_audio) : Json(nullptr);
        record["audioSampleRate"] = has_audio ? expected_rate : 0;
        record["audioChannels"] = has_audio ? 2 : 0;
        record["gainPerSource"] = project_plan->gain;
        record["project"] = request.at("project");
      }
      if(thumbnail) {
        record["durationHns"]=nullptr;record["videoCodec"]="png";record["audioCodec"]=nullptr;
        record["audioSampleRate"]=0;record["audioChannels"]=0;record["gainPerSource"]=0;
        record["inputPath"]=utf8(video.native());record["width"]=thumbnail_width;record["height"]=thumbnail_height;
        record["requestedSeekHns"]=thumbnail_seek;record["effectiveSeekHns"]=thumbnail_effective_seek;
        record["seekAttempts"]=thumbnail_attempts;record["decodedFrames"]=process_frames;
      }
      const std::filesystem::path pending = receipt.native() + L".pending";
      write_json(pending, record);
      if (!MoveFileExW(partial.c_str(), output.c_str(), MOVEFILE_WRITE_THROUGH))
        throw Error(NSS_E_IO, NSS_STAGE_STORAGE,
                    HRESULT_FROM_WIN32(GetLastError()),
                    "Final A/V publication failed");
      if (!MoveFileExW(pending.c_str(), receipt.c_str(),
                       MOVEFILE_WRITE_THROUGH))
        throw Error(NSS_E_IO, NSS_STAGE_STORAGE,
                    HRESULT_FROM_WIN32(GetLastError()),
                    "Export receipt publication failed");
      checkpoint(NSS_JOB_COMPLETED);
      {
        std::lock_guard lock(mutex);
        progress = duration;
        state = NSS_JOB_COMPLETED;
        emit(start_id, NSS_OK);
        release_budget();
      }
    } catch (const Error &e) {
      const auto terminal = e.code == NSS_E_CANCELLED ? NSS_JOB_CANCELLED : NSS_JOB_FAILED;
      {
        std::lock_guard lock(mutex);
        fault = {e.code, e.stage, e.native, e.what()};
        completion_claimed = true;
      }
      // A terminal event permits managed consumers to read/consolidate these
      // documents and release the job. Publish it only after the final durable
      // checkpoint, just as the successful completion path already does.
      preserve_terminal_checkpoint(terminal);
      {
        std::lock_guard lock(mutex);
        state = terminal;
        emit(start_id, e.code);
        if (cancel_id)
          emit(cancel_id, state == NSS_JOB_CANCELLED ? NSS_OK : e.code);
        release_budget();
      }
    } catch (const std::exception &e) {
      {
        std::lock_guard lock(mutex);
        fault = {NSS_E_INTERNAL, NSS_STAGE_JOB, 0, e.what()};
        completion_claimed = true;
      }
      preserve_terminal_checkpoint(NSS_JOB_FAILED);
      {
        std::lock_guard lock(mutex);
        state = NSS_JOB_FAILED;
        emit(start_id, fault.code);
        release_budget();
      }
    }
    --workers;
  }
};
Job::Job(Json request, std::shared_ptr<std::atomic<bool>> budget)
    : impl_(std::make_unique<Impl>(std::move(request), std::move(budget))) {
  if (live_jobs.fetch_add(1) >= 64) {
    --live_jobs;
    throw Error(NSS_E_BUSY, NSS_STAGE_JOB, 0, "Job object budget exhausted");
  }
}
Job::~Job() {
  abandon();
  if (impl_->worker.joinable())
    impl_->worker.join();
  impl_->release_budget();
  --live_jobs;
}
nss_result Job::start(uint64_t &id) {
  std::lock_guard lock(impl_->mutex);
  if (impl_->state != NSS_JOB_CREATED)
    return NSS_E_STATE;
  impl_->started = true;
  impl_->state = NSS_JOB_RUNNING;
  id = impl_->start_id = impl_->next_command++;
  impl_->emit(0, NSS_OK);
  try {
    impl_->worker = std::thread([this] { impl_->run(); });
  } catch (...) {
    impl_->state = NSS_JOB_CREATED;
    impl_->started = false;
    id = 0;
    throw;
  }
  return NSS_ACCEPTED;
}
nss_result Job::cancel(uint64_t &id) {
  std::lock_guard lock(impl_->mutex);
  if (impl_->state == NSS_JOB_CANCELLED) {
    id = impl_->cancel_id;
    return NSS_ALREADY;
  }
  if (impl_->state == NSS_JOB_COMPLETED || impl_->state == NSS_JOB_FAILED ||
      impl_->completion_claimed)
    return NSS_E_STATE;
  if (impl_->cancel_id) {
    id = impl_->cancel_id;
    return NSS_ALREADY;
  }
  id = impl_->cancel_id = impl_->next_command++;
  impl_->cancelled = true;
  if (!impl_->started) {
    impl_->state = NSS_JOB_CANCELLED;
    impl_->emit(id, NSS_OK);
    impl_->release_budget();
  } else {
    impl_->state = NSS_JOB_CANCELLING;
    impl_->emit(0, NSS_OK);
  }
  return NSS_ACCEPTED;
}
Json Job::snapshot() const {
  std::lock_guard lock(impl_->mutex);
  const bool thumbnail = impl_->profile == "thumbnail-png";
  const bool terminal = impl_->state == NSS_JOB_COMPLETED ||
                        impl_->state == NSS_JOB_CANCELLED ||
                        impl_->state == NSS_JOB_FAILED;
  Json stage =
      thumbnail ? Json{{"videoPolicy", "software decode to one PNG"},
                       {"audioCodec", "absent"}, {"sampleRate", 0}, {"channels", 0},
                       {"maxWidth", 640}, {"maxHeight", 360}}
      : impl_->project_plan
          ? impl_->project_plan->effective()
          : Json{{"videoPolicy",
                  impl_->profile == "audio-basic" ? "absent" : "H264 copy"},
                 {"audioCodec",
                  impl_->profile == "camera-remux" ? "absent" : "pcm_s16le"},
                 {"sampleRate", impl_->profile == "camera-remux" ? 0 : 48000},
                 {"channels", impl_->profile == "camera-remux" ? 0 : 2}};
  stage["implementationId"] = "FFmpeg " + impl_->profile;
  stage["software"] = true;
  stage["hardware"] = false;
  std::error_code output_error, partial_error;
  const bool output_exists =
      std::filesystem::is_regular_file(impl_->output, output_error);
  const bool partial_exists =
      std::filesystem::is_regular_file(impl_->partial, partial_error);
  return {
      {"version", 1},
      {"objectId", impl_->id},
      {"state", impl_->state},
      {"lastSequence", impl_->sequence},
      {"pendingCommandId", impl_->started && !terminal ? impl_->start_id : 0},
      {"operationResult", terminal ? (impl_->fault ? impl_->fault.code : NSS_OK)
                                   : (impl_->started ? NSS_ACCEPTED : NSS_OK)},
      {"cleanupPending", impl_->started && !terminal},
      {"secondaryErrors", impl_->secondary},
      {"journalPath", utf8(impl_->journal_path.native())},
      {"requested", impl_->request},
      {"effectiveStages", Json::array({stage})},
      {"metrics",
       {{"usefulDurationHns", thumbnail ? Json(nullptr) : Json(impl_->duration)},
        {"progressHns", thumbnail ? Json(nullptr) : Json(impl_->progress)},
        {"gainPerSource",
         thumbnail ? 0.0 : impl_->project_plan ? impl_->project_plan->gain : 0.5},
        {"diagnosticByteLimit", 65536}}},
      {"assets",
       impl_->state == NSS_JOB_COMPLETED
           ? Json::array({{{"path", utf8(impl_->output.native())},
                           {"container",
                            thumbnail ? "png" : impl_->project_plan ? impl_->project_plan->container
                            : impl_->profile == "camera-remux" ? "mp4"
                                                               : "matroska"},
                           {"finalized", true}}})
           : Json::array()},
      {"mediaComplete", impl_->state == NSS_JOB_COMPLETED},
      {"activeTimeHns", thumbnail ? Json(nullptr) : Json(impl_->progress)},
      {"durationHns", thumbnail ? Json(nullptr) : Json(impl_->duration)},
      {"actualMediaDurationHns", !thumbnail && impl_->state == NSS_JOB_COMPLETED
                                     ? Json(impl_->process_time)
                                     : Json(nullptr)},
      {"decodedVideoFrames", impl_->state == NSS_JOB_COMPLETED
                                 ? Json(impl_->process_frames)
                                 : Json(nullptr)},
      {"outputPath", utf8(impl_->output.native())},
      {"partialPath", utf8(impl_->partial.native())},
      {"outputExists", output_exists},
      {"partialExists", partial_exists},
      {"outputRequiresRecovery",
       output_exists && impl_->state != NSS_JOB_COMPLETED},
      {"exitCode", impl_->exit_code},
      {"validationExitCode", impl_->validation_exit_code},
      {"codecProbe", diagnostic_text(impl_->codec_log)},
      {"stderrTail", diagnostic_text(impl_->log)},
      {"validationStderrTail", diagnostic_text(impl_->validation_log)},
      {"liveNativeJobChildren", children.load()},
      {"diagnosticByteLimit", 65536},
      {"firstError", impl_->fault ? Json{{"code", impl_->fault.code},
                                         {"stage", impl_->fault.stage},
                                         {"hresult", impl_->fault.hresult},
                                         {"message", impl_->fault.message}}
                                  : Json(nullptr)}};
}
nss_result Job::poll(uint32_t timeout, nss_event_v1 &out) {
  if (timeout > 60000)
    return NSS_E_ARGUMENT;
  std::unique_lock lock(impl_->mutex);
  if (impl_->polling)
    return NSS_E_BUSY;
  impl_->polling = true;
  impl_->changed.wait_for(lock, std::chrono::milliseconds(timeout), [&] {
    return !impl_->events.empty() || impl_->interrupted;
  });
  impl_->polling = false;
  if (impl_->interrupted) {
    impl_->interrupted = false;
    return NSS_E_CANCELLED;
  }
  if (impl_->events.empty())
    return NSS_TIMEOUT;
  out = impl_->events.front();
  impl_->events.pop_front();
  return NSS_OK;
}
void Job::interrupt() noexcept {
  std::lock_guard lock(impl_->mutex);
  impl_->interrupted = true;
  impl_->changed.notify_all();
}
void Job::abandon() noexcept {
  std::lock_guard lock(impl_->mutex);
  if (!impl_->completion_claimed)
    impl_->cancelled = true;
  impl_->interrupted = true;
  impl_->changed.notify_all();
}
} // namespace nss::jobs
