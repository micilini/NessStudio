#include "scene.hpp"
#include <cmath>

namespace nss::scene {
namespace {
[[noreturn]] void invalid(const char *message) {
  throw capture::Error(NSS_E_ARGUMENT, NSS_STAGE_PLAN, 0, message);
}
void keys(const Json &value, std::initializer_list<const char *> names) {
  if (!value.is_object())
    invalid("Scene objects are required");
  for (const auto &item : value.items())
    if (std::find(names.begin(), names.end(), item.key()) == names.end())
      invalid("Unknown scene field");
}
double real(const Json &value, const char *name, double fallback, double low,
            double high) {
  if (!value.contains(name))
    return fallback;
  if (!value.at(name).is_number())
    invalid("Scene coordinates/opacity must be finite numbers");
  const auto result = value.at(name).get<double>();
  if (!std::isfinite(result) || result < low || result > high)
    invalid("Scene value exceeds its normalized bounds");
  return result;
}
uint32_t integer(const Json &value, const char *name, uint32_t fallback,
                 uint32_t maximum) {
  if (!value.contains(name))
    return fallback;
  if (!(value.at(name).is_number_unsigned() ||
        (value.at(name).is_number_integer() &&
         value.at(name).get<int64_t>() >= 0)))
    invalid("Scene integer must be nonnegative");
  const auto result = value.at(name).get<uint64_t>();
  if (result > maximum)
    invalid("Scene integer exceeds its budget");
  return static_cast<uint32_t>(result);
}
bool flag(const Json &value, const char *name, bool fallback) {
  if (!value.contains(name))
    return fallback;
  if (!value.at(name).is_boolean())
    invalid("Scene flag must be boolean");
  return value.at(name).get<bool>();
}
Box box(const Json &value) {
  keys(value, {"x", "y", "width", "height"});
  Box result{real(value, "x", 0, 0, 1), real(value, "y", 0, 0, 1),
             real(value, "width", 1, .000001, 1),
             real(value, "height", 1, .000001, 1)};
  if (result.x + result.width > 1.00000001 ||
      result.y + result.height > 1.00000001)
    invalid("Scene rectangles must remain inside their source/canvas");
  return result;
}
Json box_json(const Box &value) {
  return {{"x", value.x},
          {"y", value.y},
          {"width", value.width},
          {"height", value.height}};
}
int32_t even(double value) {
  return static_cast<int32_t>(std::llround(value / 2)) * 2;
}
} // namespace
Snapshot parse(const Json &value) {
  keys(value, {"version", "revision", "width", "height", "fpsNum", "fpsDen",
               "color", "background", "layout", "layers"});
  if (integer(value, "version", 0, 1) != 1)
    invalid("Scene version 1 required");
  Snapshot result;
  result.width = integer(value, "width", 0, 1920);
  result.height = integer(value, "height", 0, 1080);
  result.fps_num = integer(value, "fpsNum", 30, 30000);
  result.fps_den = integer(value, "fpsDen", 1, 1001);
  if (!result.width || !result.height || (result.width & 1) ||
      (result.height & 1) ||
      !recording_cadence_supported(result.fps_num, result.fps_den))
    invalid(
        "Scene requires a bounded even canvas and qualified rational cadence");
  if (value.value("color", std::string{"BT709Limited"}) != "BT709Limited")
    invalid("Only explicit SDR BT709 limited output is supported");
  result.layout = value.value("layout", std::string{"Custom"});
  if (result.layout != "Screen" && result.layout != "Camera" &&
      result.layout != "PictureInPicture" && result.layout != "SideBySide" &&
      result.layout != "Custom")
    invalid("Unknown scene layout");
  const auto background = value.value("background", Json::array({0, 0, 0}));
  if (!background.is_array() || background.size() != 3)
    invalid("Scene background requires opaque RGB bytes");
  for (size_t index = 0; index < 3; ++index) {
    if (!background[index].is_number_integer() ||
        background[index].get<int64_t>() < 0 ||
        background[index].get<int64_t>() > 255)
      invalid("Invalid scene background byte");
    result.background[index] = background[index].get<uint8_t>();
  }
  const auto &layers = value.at("layers");
  if (!layers.is_array() || layers.empty() || layers.size() > 2)
    invalid("Scene supports one or two ordered source layers");
  for (const auto &item : layers) {
    keys(item,
         {"sourceId", "crop", "destination", "visible", "opacity", "rotation",
          "mirror", "fit", "missing", "maxAgeMs", "required"});
    Layer layer;
    layer.source = integer(item, "sourceId", 0, 2);
    if (!layer.source)
      invalid("Scene source ID must identify screen or camera");
    if (std::any_of(
            result.layers.begin(), result.layers.end(),
            [&](const auto &other) { return other.source == layer.source; }))
      invalid("Duplicate scene source layer");
    layer.crop = box(item.value("crop", Json::object()));
    layer.destination = box(item.value("destination", Json::object()));
    layer.opacity = real(item, "opacity", 1, 0, 1);
    layer.visible = flag(item, "visible", true);
    layer.mirror = flag(item, "mirror", false);
    layer.required = flag(item, "required", true);
    layer.rotation = integer(item, "rotation", 0, 270);
    if (layer.rotation % 90)
      invalid("Scene rotation must be 0/90/180/270 degrees clockwise");
    const auto fit = item.value("fit", std::string{"Contain"});
    if (fit != "Contain" && fit != "Cover")
      invalid("Unsupported scene aspect policy");
    layer.cover = fit == "Cover";
    const auto missing = item.value("missing", std::string{"HoldLast"});
    if (missing == "HoldLast")
      layer.missing = Missing::HoldLast;
    else if (missing == "Background")
      layer.missing = Missing::Background;
    else if (missing == "Fail")
      layer.missing = Missing::Fail;
    else
      invalid("Unsupported scene late-source policy");
    layer.max_age_ms = integer(item, "maxAgeMs", 500, 10000);
    if (!layer.max_age_ms)
      invalid("Scene source age budget must be positive");
    result.layers.push_back(layer);
  }
  return result;
}
Selection select(const Layer &layer, const std::optional<capture::Frame> &frame,
                 int64_t now, bool required) {
  if (!frame && required)
    return Selection::Wait;
  if (frame && now - frame->capture_hns <= int64_t(layer.max_age_ms) * 10000)
    return Selection::Frame;
  if (layer.missing == Missing::Fail)
    return Selection::Fail;
  if (layer.missing == Missing::Background || !frame)
    return Selection::Background;
  return Selection::Frame;
}
Json json(const Snapshot &value) {
  auto layers = Json::array();
  for (const auto &layer : value.layers)
    layers.push_back(
        {{"sourceId", layer.source},
         {"crop", box_json(layer.crop)},
         {"destination", box_json(layer.destination)},
         {"visible", layer.visible},
         {"opacity", layer.opacity},
         {"rotation", layer.rotation},
         {"mirror", layer.mirror},
         {"fit", layer.cover ? "Cover" : "Contain"},
         {"missing", layer.missing == Missing::HoldLast     ? "HoldLast"
                     : layer.missing == Missing::Background ? "Background"
                                                            : "Fail"},
         {"maxAgeMs", layer.max_age_ms},
         {"required", layer.required}});
  return {{"version", 1},
          {"revision", value.revision},
          {"width", value.width},
          {"height", value.height},
          {"fpsNum", value.fps_num},
          {"fpsDen", value.fps_den},
          {"color", "BT709Limited"},
          {"background", value.background},
          {"layout", value.layout},
          {"layers", layers}};
}
Placement place(const Snapshot &scene, const Layer &layer, uint32_t width,
                uint32_t height, uint32_t orientation) {
  Placement result;
  result.crop = layer.crop;
  result.rotation = (layer.rotation + orientation) % 360;
  result.mirror = layer.mirror;
  auto &dest = result.destination;
  dest.x = even(layer.destination.x * scene.width);
  dest.y = even(layer.destination.y * scene.height);
  dest.width = (std::max)(2, even(layer.destination.width * scene.width));
  dest.height = (std::max)(2, even(layer.destination.height * scene.height));
  dest.width =
      (std::min)(dest.width, static_cast<int32_t>(scene.width) - dest.x);
  dest.height =
      (std::min)(dest.height, static_cast<int32_t>(scene.height) - dest.y);
  if (dest.width < 2 || dest.height < 2)
    invalid("Resolved scene destination is empty");
  double input_w = width * result.crop.width,
         input_h = height * result.crop.height;
  const bool rotated = result.rotation == 90 || result.rotation == 270;
  if (rotated)
    std::swap(input_w, input_h);
  if (!layer.cover) {
    const auto ratio = (std::min)(dest.width / input_w, dest.height / input_h);
    const auto w = (std::clamp)(even(input_w * ratio), 2, dest.width),
               h = (std::clamp)(even(input_h * ratio), 2, dest.height);
    dest.x += even((dest.width - w) / 2.0);
    dest.y += even((dest.height - h) / 2.0);
    dest.width = w;
    dest.height = h;
  } else {
    const auto aspect = double(dest.width) / dest.height;
    if (input_w / input_h > aspect) {
      const auto amount = aspect * input_h / input_w;
      if (rotated) {
        result.crop.y += result.crop.height * (1 - amount) / 2;
        result.crop.height *= amount;
      } else {
        result.crop.x += result.crop.width * (1 - amount) / 2;
        result.crop.width *= amount;
      }
    } else {
      const auto amount = input_w / aspect / input_h;
      if (rotated) {
        result.crop.x += result.crop.width * (1 - amount) / 2;
        result.crop.width *= amount;
      } else {
        result.crop.y += result.crop.height * (1 - amount) / 2;
        result.crop.height *= amount;
      }
    }
  }
  return result;
}
std::array<double, 2> source_uv(const Placement &place, double x, double y) {
  if (place.mirror)
    x = 1 - x;
  switch (place.rotation) {
  case 90: {
    const auto old = x;
    x = y;
    y = 1 - old;
    break;
  }
  case 180:
    x = 1 - x;
    y = 1 - y;
    break;
  case 270: {
    const auto old = x;
    x = 1 - y;
    y = old;
    break;
  }
  default:
    break;
  }
  return {place.crop.x + x * place.crop.width,
          place.crop.y + y * place.crop.height};
}
Store::Store(Snapshot value)
    : current_(std::make_shared<const Snapshot>(std::move(value))) {
  for (const auto &layer : current_.load()->layers) {
    allowed_[layer.source] = true;
    required_[layer.source] = layer.required;
  }
}
std::shared_ptr<const Snapshot> Store::get() const { return current_.load(); }
uint64_t Store::update(const Json &value) {
  auto next = parse(value);
  std::lock_guard lock(gate_);
  const auto previous = current_.load();
  if (next.width != previous->width || next.height != previous->height ||
      next.fps_num != previous->fps_num || next.fps_den != previous->fps_den)
    invalid("Active scene updates cannot change canvas/cadence/color");
  for (const auto &layer : next.layers) {
    if (!allowed_[layer.source])
      invalid("Active scene updates cannot add unopened sources");
    if (layer.required != required_[layer.source])
      invalid(
          "Active scene updates cannot change source ownership requirements");
  }
  next.revision = previous->revision + 1;
  current_.store(std::make_shared<const Snapshot>(std::move(next)));
  return previous->revision + 1;
}
} // namespace nss::scene
