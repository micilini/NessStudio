#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "project_export.hpp"
#include "scene/scene.hpp"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <locale>
#include <sstream>
#include <windows.h>

namespace nss::jobs {
namespace {
std::wstring wide(const std::string &text) {
  if (text.empty() || text.size() > 32760 ||
      text.find('\0') != std::string::npos)
    throw std::invalid_argument("Invalid export text/path budget");
  const int count =
      MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                          static_cast<int>(text.size()), nullptr, 0);
  if (!count)
    throw std::invalid_argument("Invalid export UTF8");
  std::wstring result(static_cast<size_t>(count), 0);
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                      static_cast<int>(text.size()), result.data(), count);
  return result;
}
std::string number(double value) {
  std::ostringstream out;
  out.imbue(std::locale::classic());
  out << std::fixed << std::setprecision(7) << value;
  return out.str();
}
void option(std::vector<std::wstring> &args, const std::string &name,
            const std::string &value) {
  args.push_back(wide(name));
  args.push_back(wide(value));
}
void known(const Json &value, std::initializer_list<const char *> names) {
  if (!value.is_object())
    throw std::invalid_argument("Export metadata must be an object");
  for (const auto &field : value.items())
    if (std::none_of(names.begin(), names.end(),
                     [&](const char *name) { return field.key() == name; }))
      throw std::invalid_argument("Unknown export metadata field: " +
                                  field.key());
}
struct Input {
  std::string role;
  std::filesystem::path path;
  Json video, audio;
  int index{};
};
} // namespace
Json ProjectExportPlan::effective() const {
  return {
      {"videoPolicy",
       video ? (reencode ? "H264 software encode" : "H264 copy") : "absent"},
      {"videoCodec", video ? Json("h264") : Json(nullptr)},
      {"audioCodec", audio ? Json(audio_codec) : Json(nullptr)},
      {"sampleRate", audio ? sample_rate : 0},
      {"channels", audio ? 2 : 0},
      {"width", width},
      {"height", height},
      {"fps", video && reencode ? Json(fps) : Json(nullptr)},
      {"fpsNum", video && reencode ? Json(fps_num) : Json(nullptr)},
      {"fpsDen", video && reencode ? Json(fps_den) : Json(nullptr)},
      {"frameRatePolicy",
       video ? (reencode ? "explicit rational scene/export cadence"
                         : "source stream preserved")
             : "absent"},
      {"container", container},
      {"gainPerSource", gain},
      {"normalization", false},
      {"hardwareAcceleration", false},
      {"sourceAssetsPreserved", true}};
}
ProjectExportPlan project_export(const Json &project) {
  known(project, {"version", "layout", "container", "quality", "durationHns",
                  "fps", "inputs", "scene", "legacyDuration"});
  if (project.at("version") != 1)
    throw std::invalid_argument("Unsupported project export version");
  ProjectExportPlan plan;
  plan.duration = project.at("durationHns").get<int64_t>();
  plan.fps = project.at("fps").get<uint32_t>();
  plan.fps_num = plan.fps;
  plan.legacy_duration = project.value("legacyDuration", false);
  if (plan.duration <= 0 || plan.duration > maximum_session_hns ||
      plan.fps < 1 || plan.fps > 120)
    throw std::invalid_argument("Export duration/cadence exceeds the budget");
  const std::string layout = project.at("layout"),
                    quality = project.at("quality"),
                    container = project.at("container");
  if (layout != "NoVideo" && layout != "ScreenOnly" && layout != "WebcamOnly" &&
      layout != "ScreenWebcamPictureInPicture" &&
      layout != "ScreenWebcamSideBySide" && layout != "RecordedProgram")
    throw std::invalid_argument("Unknown export layout");
  if (container != "Mp4" && container != "Mkv" && container != "Mp3" &&
      container != "Wav")
    throw std::invalid_argument("Unknown export container");
  if (quality != "Fast" && quality != "Balanced" && quality != "HighQuality" &&
      quality != "Audio128Kbps" && quality != "Audio192Kbps" &&
      quality != "Audio320Kbps" && quality != "Wav44100Hz16Bit" &&
      quality != "Wav48000Hz16Bit" && quality != "Wav48000Hz24Bit")
    throw std::invalid_argument("Unknown export quality");
  plan.container = container == "Mp4"   ? "mp4"
                   : container == "Mkv" ? "matroska"
                   : container == "Mp3" ? "mp3"
                                        : "wav";
  const auto &definitions = project.at("inputs");
  if (!definitions.is_array() || definitions.empty() || definitions.size() > 5)
    throw std::invalid_argument("Export requires one to five selected assets");
  std::vector<Input> inputs;
  int screen = -1, camera = -1, program = -1;
  std::vector<int> audio;
  for (const auto &definition : definitions) {
    known(definition, {"role", "path", "video", "audio"});
    Input input;
    input.role = definition.at("role");
    input.path = std::filesystem::path(wide(definition.at("path")));
    input.index = static_cast<int>(inputs.size());
    input.video = definition.value("video", Json(nullptr));
    input.audio = definition.value("audio", Json(nullptr));
    if (input.role != "screen" && input.role != "camera" &&
        input.role != "program" && input.role != "microphone" &&
        input.role != "system")
      throw std::invalid_argument("Unknown export asset role");
    if (std::any_of(inputs.begin(), inputs.end(), [&](const Input &old) {
          return old.role == input.role || old.path == input.path;
        }))
      throw std::invalid_argument("Duplicate export input identity");
    if (!input.path.is_absolute() ||
        !std::filesystem::is_regular_file(input.path) ||
        std::filesystem::file_size(input.path) == 0)
      throw std::invalid_argument("Export input is absent or empty");
    if (input.role == "screen")
      screen = input.index;
    else if (input.role == "camera")
      camera = input.index;
    else if (input.role == "program")
      program = input.index;
    else
      audio.push_back(input.index);
    if (input.video.is_object()) {
      known(input.video, {"width", "height", "fps", "crop"});
      const int w = input.video.at("width"), h = input.video.at("height");
      if (w < 2 || h < 2 || w > 16384 || h > 16384)
        throw std::invalid_argument("Invalid export video geometry");
    } else if (input.role == "screen" || input.role == "camera" ||
               input.role == "program")
      throw std::invalid_argument("Video format metadata is required");
    if (input.audio.is_object()) {
      known(input.audio, {"offsetMs", "channels"});
      const int64_t offset = input.audio.value("offsetMs", int64_t{});
      const int channels = input.audio.value("channels", 2);
      if (offset < 0 || offset > maximum_session_hns / 10000 || channels < 1 ||
          channels > 8)
        throw std::invalid_argument("Invalid export audio alignment");
    } else if (input.role == "microphone" || input.role == "system")
      throw std::invalid_argument("Audio alignment metadata is required");
    plan.inputs.push_back(input.path);
    inputs.push_back(std::move(input));
  }
  plan.video = layout != "NoVideo";
  plan.audio = !audio.empty();
  plan.gain = audio.size() > 1 ? .5 : 1;
  if (!plan.video && !plan.audio)
    throw std::invalid_argument("No selected export stream");
  if (plan.video && (container == "Mp3" || container == "Wav"))
    throw std::invalid_argument("Audio-only containers require NoVideo");
  if ((layout == "ScreenOnly" && screen < 0) ||
      (layout == "WebcamOnly" && camera < 0) ||
      (layout == "RecordedProgram" && program < 0) ||
      ((layout == "ScreenWebcamPictureInPicture" ||
        layout == "ScreenWebcamSideBySide") &&
       (screen < 0 || camera < 0)))
    throw std::invalid_argument("Selected layout has a missing source");
  std::vector<std::string> filters;
  auto &args = plan.arguments;
  args = {L"-nostdin", L"-hide_banner", L"-loglevel", L"info", L"-y"};
  for (const auto &input : inputs) {
    args.insert(args.end(), {L"-hwaccel", L"none", L"-i", input.path.native()});
  }
  std::string video_map;
  auto stream = [](int index, const char *kind) {
    return std::to_string(index) + ":" + kind;
  };
  const bool shared_scene = plan.video && layout != "RecordedProgram" &&
                            project.contains("scene") &&
                            !project.at("scene").is_null();
  if (shared_scene) {
    auto snapshot = scene::parse(project.at("scene"));
    plan.width = snapshot.width;
    plan.height = snapshot.height;
    plan.fps_num = snapshot.fps_num;
    plan.fps_den = snapshot.fps_den;
    plan.fps = static_cast<uint32_t>(
        std::llround(double(plan.fps_num) / plan.fps_den));
    const std::string cadence =
        std::to_string(plan.fps_num) + "/" + std::to_string(plan.fps_den);
    plan.reencode = true;
    std::ostringstream color;
    color << "0x" << std::hex << std::setfill('0');
    for (auto value : snapshot.background)
      color << std::setw(2) << static_cast<int>(value);
    filters.push_back(
        "color=c=" + color.str() + ":s=" + std::to_string(plan.width) + "x" +
        std::to_string(plan.height) + ":r=" + cadence +
        ":d=" + number(double(plan.duration) / hns_per_second) + "[scene0]");
    int order{};
    const auto visible =
        std::count_if(snapshot.layers.begin(), snapshot.layers.end(),
                      [](const scene::Layer &layer) { return layer.visible; });
    if (!visible)
      throw std::invalid_argument("Export scene has no visible layers");
    for (const auto &layer : snapshot.layers) {
      if (!layer.visible)
        continue;
      const int index = layer.source == 1   ? screen
                        : layer.source == 2 ? camera
                                            : -1;
      if (index < 0)
        throw std::invalid_argument("Scene uses an unselected export source");
      const auto &format = inputs[static_cast<size_t>(index)].video;
      const uint32_t w = format.at("width"), h = format.at("height");
      const auto placement = scene::place(snapshot, layer, w, h);
      const auto &d = placement.destination;
      const int cx = std::clamp(
                    static_cast<int>(std::floor(placement.crop.x * w)) & ~1, 0,
                    static_cast<int>(w) - 2),
                cy = std::clamp(
                    static_cast<int>(std::floor(placement.crop.y * h)) & ~1, 0,
                    static_cast<int>(h) - 2);
      const int cw = std::clamp(
                    static_cast<int>(std::round(placement.crop.width * w)) & ~1,
                    2, static_cast<int>(w) - cx),
                ch = std::clamp(
                    static_cast<int>(std::round(placement.crop.height * h)) &
                        ~1,
                    2, static_cast<int>(h) - cy);
      std::string transform =
          "[" + stream(index, "v:0") + "]setpts=PTS-STARTPTS,fps=" + cadence +
          ",crop=" + std::to_string(cw) + ":" + std::to_string(ch) + ":" +
          std::to_string(cx) + ":" + std::to_string(cy);
      if (layer.rotation == 90)
        transform += ",transpose=clock";
      else if (layer.rotation == 180)
        transform += ",hflip,vflip";
      else if (layer.rotation == 270)
        transform += ",transpose=cclock";
      if (layer.mirror)
        transform += ",hflip";
      transform += ",scale=" + std::to_string(d.width) + ":" +
                   std::to_string(d.height) +
                   ":flags=bilinear,setsar=1,format=rgba";
      if (layer.opacity != 1)
        transform += ",colorchannelmixer=aa=" + number(layer.opacity);
      const std::string suffix = std::to_string(order);
      filters.push_back(transform + "[layer" + suffix + "]");
      const std::string output =
          order + 1 == visible ? "v" : "scene" + std::to_string(order + 1);
      filters.push_back("[scene" + suffix + "][layer" + suffix + "]overlay=x=" +
                        std::to_string(d.x) + ":y=" + std::to_string(d.y) +
                        ":eof_action=repeat:shortest=0[" + output + "]");
      ++order;
    }
    video_map = "[v]";
  } else if (plan.video) {
    const int index = layout == "WebcamOnly"        ? camera
                      : layout == "RecordedProgram" ? program
                                                    : screen;
    const auto &format = inputs[static_cast<size_t>(index)].video;
    plan.width = format.at("width");
    plan.height = format.at("height");
    if (layout == "ScreenWebcamPictureInPicture") {
      filters.push_back("[" + stream(screen, "v:0") +
                        "]setpts=PTS-STARTPTS,fps=" + std::to_string(plan.fps) +
                        "[screenv]");
      filters.push_back("[" + stream(camera, "v:0") +
                        "]setpts=PTS-STARTPTS,fps=" + std::to_string(plan.fps) +
                        ",scale=trunc(iw*0.25/2)*2:-2[cam]");
      filters.push_back("[screenv][cam]overlay=W-w-24:H-h-24:eof_action=repeat:"
                        "shortest=0[v]");
      video_map = "[v]";
      plan.reencode = true;
    } else if (layout == "ScreenWebcamSideBySide") {
      const uint32_t h = plan.height & ~1U;
      const auto &camera_format = inputs[static_cast<size_t>(camera)].video;
      const double camera_width = camera_format.at("width").get<double>(),
                   camera_height = camera_format.at("height").get<double>();
      plan.width = static_cast<uint32_t>(
          std::round(double(plan.width) * h / plan.height / 2) * 2 +
          std::round(camera_width * h / camera_height / 2) * 2);
      plan.height = h;
      filters.push_back("[" + stream(screen, "v:0") +
                        "]setpts=PTS-STARTPTS,fps=" + std::to_string(plan.fps) +
                        ",scale=-2:" + std::to_string(h) + "[screen]");
      filters.push_back("[" + stream(camera, "v:0") +
                        "]setpts=PTS-STARTPTS,fps=" + std::to_string(plan.fps) +
                        ",scale=-2:" + std::to_string(h) + "[cam]");
      filters.push_back("[screen][cam]hstack=inputs=2:shortest=0[v]");
      video_map = "[v]";
      plan.reencode = true;
    } else if (format.contains("crop") && !format.at("crop").is_null()) {
      const auto &crop = format.at("crop");
      known(crop, {"x", "y", "w", "h"});
      const int x = crop.at("x"), y = crop.at("y"), w = crop.at("w"),
                h = crop.at("h");
      if (x < 0 || y < 0 || w < 2 || h < 2 ||
          x + w > static_cast<int>(plan.width) ||
          y + h > static_cast<int>(plan.height) || (w & 1) || (h & 1))
        throw std::invalid_argument("Invalid legacy crop");
      filters.push_back("[" + stream(index, "v:0") +
                        "]crop=" + std::to_string(w) + ":" + std::to_string(h) +
                        ":" + std::to_string(x) + ":" + std::to_string(y) +
                        ",setpts=PTS-STARTPTS[v]");
      plan.width = static_cast<uint32_t>(w);
      plan.height = static_cast<uint32_t>(h);
      video_map = "[v]";
      plan.reencode = true;
    } else
      video_map = stream(index, "v:0");
  }
  if (plan.audio) {
    std::string labels;
    for (size_t n = 0; n < audio.size(); ++n) {
      const int index = audio[n];
      const auto &input = inputs[static_cast<size_t>(index)];
      const int64_t offset = input.audio.value("offsetMs", int64_t{});
      std::string filter =
          "[" + stream(index, "a:0") +
          "]asetpts=PTS-STARTPTS,aresample=48000:async=1:first_pts=0";
      if (offset)
        filter += ",adelay=" + std::to_string(offset) + ":all=1";
      filter += ",volume=" + number(plan.gain) + ",apad,atrim=duration=" +
                number(double(plan.duration) / hns_per_second) + "[a" +
                std::to_string(n) + "]";
      filters.push_back(filter);
      labels += "[a" + std::to_string(n) + "]";
    }
    filters.push_back(
        labels +
        (audio.size() > 1
             ? "amix=inputs=2:duration=longest:dropout_transition=0:normalize=0"
             : "anull") +
        "[a]");
  }
  if (plan.video && (plan.width > 4096 || plan.height > 2160 ||
                     uint64_t(plan.width) * plan.height > 4096ULL * 2160))
    throw std::invalid_argument(
        "Export canvas exceeds the qualified 4096x2160 budget");
  if (!filters.empty()) {
    std::string filter;
    for (const auto &part : filters) {
      if (!filter.empty())
        filter += ';';
      filter += part;
    }
    option(args, "-filter_complex", filter);
  }
  if (plan.video) {
    option(args, "-map", video_map);
    option(args, "-c:v", plan.reencode ? "libx264" : "copy");
    plan.video_codec = "h264";
    if (plan.reencode) {
      option(args, "-preset",
             quality == "Fast"          ? "veryfast"
             : quality == "HighQuality" ? "slow"
                                        : "medium");
      option(args, "-crf",
             quality == "Fast"          ? "26"
             : quality == "HighQuality" ? "18"
                                        : "23");
      option(args, "-pix_fmt", "yuv420p");
      option(args, "-colorspace", "bt709");
      option(args, "-color_primaries", "bt709");
      option(args, "-color_trc", "bt709");
      option(args, "-color_range", "tv");
    }
  } else
    args.push_back(L"-vn");
  if (plan.audio) {
    option(args, "-map", "[a]");
    plan.audio_codec =
        container == "Mp3" ? "mp3"
        : container == "Wav"
            ? (quality == "Wav48000Hz24Bit" ? "pcm_s24le" : "pcm_s16le")
            : "aac";
    option(args, "-c:a",
           plan.audio_codec == "mp3" ? "libmp3lame" : plan.audio_codec);
    plan.sample_rate =
        quality == "Wav44100Hz16Bit" && container == "Wav" ? 44100U : 48000U;
    option(args, "-ar", std::to_string(plan.sample_rate));
    option(args, "-ac", "2");
    if (container != "Wav")
      option(args, "-b:a",
             quality == "Audio128Kbps"   ? "128k"
             : quality == "Audio320Kbps" ? "320k"
                                         : "192k");
  } else
    args.push_back(L"-an");
  option(args, "-t", number(double(plan.duration) / hns_per_second));
  option(args, "-map_metadata", "-1");
  if (container == "Mp4")
    option(args, "-movflags", "+faststart");
  args.insert(args.end(),
              {L"-progress", L"pipe:1", L"-f", wide(plan.container)});
  return plan;
}
} // namespace nss::jobs
