#include "dshow_source.hpp"
#include "dshow_controls.hpp"

namespace nss::camera {
using detail::CpuSurface;
Plan plan(const Json &request) {
  if (!request.is_object())
    throw capture::Error(NSS_E_ARGUMENT, NSS_STAGE_PLAN, 0,
                         "Camera controls must be an object");
  for (const auto &item : request.items())
    if (item.key() != "deviceId" && item.key() != "width" &&
        item.key() != "height" && item.key() != "fpsNum" &&
        item.key() != "fpsDen" && item.key() != "subtype" && item.key()!="controls" && item.key()!="timestampMode")
      throw capture::Error(NSS_E_ARGUMENT, NSS_STAGE_PLAN, 0,
                           "Unknown camera control field");
  Plan result;
  result.device_id = request.at("deviceId").get<std::string>();
  result.subtype = request.at("subtype").get<std::string>();
  result.controls=request.value("controls",Json::object());detail::validate_controls(result.controls);
  const auto timestamp_mode=request.value("timestampMode",std::string("DeliveryQpc"));
  if(timestamp_mode=="DeliveryQpc") result.timestamp_mode=TimestampMode::DeliveryQpc;
  else if(timestamp_mode=="StrictStreamQpc") result.timestamp_mode=TimestampMode::StrictStreamQpc;
  else throw capture::Error(NSS_E_ARGUMENT,NSS_STAGE_PLAN,0,"Unsupported camera timestamp mode");
  auto number = [&](const char *key, uint32_t maximum) {
    const auto &value = request.at(key);
    if (!value.is_number_unsigned() || value.get<uint64_t>() == 0 ||
        value.get<uint64_t>() > maximum)
      throw capture::Error(NSS_E_ARGUMENT, NSS_STAGE_PLAN, 0,
                           "Camera geometry/cadence exceeds its budget");
    return value.get<uint32_t>();
  };
  result.width = number("width", 1920);
  result.height = number("height", 1080);
  result.fps_num = number("fpsNum", 120000);
  result.fps_den = number("fpsDen", 10000);
  if (result.device_id.empty() || result.device_id.size() > 32760 ||
      result.device_id.find('\0') != std::string::npos || result.width & 1 ||
      result.height & 1 || uint64_t(result.fps_num) > 60ULL * result.fps_den ||
      (result.subtype != "NV12" && result.subtype != "YUY2" &&
       result.subtype != "MJPG" && result.subtype != "RGB32"))
    throw capture::Error(NSS_E_ARGUMENT, NSS_STAGE_PLAN, 0,
                         "Unsupported explicit camera identity/format/cadence");
  return result;
}
Json plan_json(const Plan &value) {
  return {{"deviceId", value.device_id}, {"subtype", value.subtype},
          {"width", value.width},        {"height", value.height},
          {"fpsNum", value.fps_num},     {"fpsDen", value.fps_den},{"controls",value.controls},
          {"timestampMode",value.timestamp_mode==TimestampMode::DeliveryQpc?"DeliveryQpc":"StrictStreamQpc"}};
}
namespace {
class CpuCamera final : public capture::Pipeline {
  Source source_;
  capture::Plan output_;
  FramePool frames_;
  uint64_t delivered_{}, pressure_{};

public:
  CpuCamera(const Plan &source, const capture::Plan &output)
      : source_(source), output_(output),
        frames_(output.slots, FramePool::nv12_bytes(output.width, output.height,
                                                    output.width)) {}
  std::optional<capture::Frame> poll() override {
    auto frame = source_.poll();
    if (!frame)
      return {};
    auto next = frames_.acquire();
    if (!next) {
      ++pressure_;
      return {};
    }
    const auto pixels = frame->surface->cpu_pixels();
    capture::process_cpu({reinterpret_cast<const uint8_t *>(pixels.data()),
                          frame->width, frame->height, frame->stride,
                          pixels.size()},
                         {0, 0, static_cast<int32_t>(frame->width),
                          static_cast<int32_t>(frame->height)},
                         output_.viewport, output_.width, output_.height,
                         {reinterpret_cast<uint8_t *>(next->writable().data()),
                          next->writable().size()});
    next->seal();
    ++delivered_;
    return capture::Frame{std::make_shared<CpuSurface>(std::move(*next)),
                          frame->id,
                          frame->generation,
                          frame->begin_hns,
                          frame->end_hns,
                          frame->capture_hns,
                          output_.width,
                          output_.height,
                          output_.width,
                          frame->estimated};
  }
  Json metrics() const override {
    auto result = source_.metrics();
    result["backend"] = "directshow-native-cpu";
    result["processor"] = "CPU bilinear BT709 limited NV12";
    result["delivered"] = delivered_;
    result["outputPoolPressureDrops"] = pressure_;
    result["ramOutputBytes"] =
        delivered_ * uint64_t(output_.width) * output_.height * 3 / 2;
    result["recordingReadbackBytes"] = 0;
    result["recordingReadbackCalls"] = 0;
    const auto stats = frames_.stats();
    result["outputPool"] = {
        {"bytes", stats.bytes}, {"inUse", stats.in_use}, {"peak", stats.peak}};
    return result;
  }
  capture::Pipeline::WaitHint wait_hint() const noexcept override { return {source_.wait_event()}; }
  void close() override { source_.close(); }
};
} // namespace
std::unique_ptr<capture::Pipeline> make_cpu(const Plan &source,
                                            const capture::Plan &output) {
  if (output.gpu)
    throw capture::Error(NSS_E_ARGUMENT, NSS_STAGE_PLAN, 0,
                         "CPU camera requires explicit software processing");
  return std::make_unique<CpuCamera>(source, output);
}
} // namespace nss::camera
