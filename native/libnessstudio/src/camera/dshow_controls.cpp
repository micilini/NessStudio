#include "dshow_controls.hpp"
#include <array>
#include <limits>

namespace nss::camera::detail {
namespace {
struct Property {const char *name;bool camera;long id;};
constexpr std::array<Property,15> properties{{
  {"exposure",true,CameraControl_Exposure},{"focus",true,CameraControl_Focus},
  {"zoom",true,CameraControl_Zoom},{"pan",true,CameraControl_Pan},{"tilt",true,CameraControl_Tilt},
  {"roll",true,CameraControl_Roll},{"iris",true,CameraControl_Iris},
  {"brightness",false,VideoProcAmp_Brightness},{"contrast",false,VideoProcAmp_Contrast},
  {"hue",false,VideoProcAmp_Hue},{"saturation",false,VideoProcAmp_Saturation},
  {"sharpness",false,VideoProcAmp_Sharpness},{"gamma",false,VideoProcAmp_Gamma},
  {"whiteBalance",false,VideoProcAmp_WhiteBalance},{"backlightCompensation",false,VideoProcAmp_BacklightCompensation}
}};
const Property &property(const std::string &name) {
  const auto found=std::find_if(properties.begin(),properties.end(),[&](const auto &p) {return name==p.name;});
  if(found==properties.end()) throw capture::Error(NSS_E_ARGUMENT,NSS_STAGE_PLAN,0,"Unknown camera device control");
  return *found;
}
}
void validate_controls(const Json &request) {
  if(!request.is_object() || request.size()>properties.size())
    throw capture::Error(NSS_E_ARGUMENT,NSS_STAGE_PLAN,0,"Camera device controls must be a bounded object");
  for(const auto &item:request.items()) {
    (void)property(item.key());const auto &value=item.value();
    if(!value.is_object() || !value.contains("mode") || !value.at("mode").is_string())
      throw capture::Error(NSS_E_ARGUMENT,NSS_STAGE_PLAN,0,"Camera control requires explicit auto/manual mode");
    const auto mode=value.at("mode").get<std::string>();
    if(mode=="auto") {
      if(value.size()!=1) throw capture::Error(NSS_E_ARGUMENT,NSS_STAGE_PLAN,0,"Automatic camera control must omit manual value");
    } else if(mode=="manual") {
      if(value.size()!=2 || !value.contains("value") || !value.at("value").is_number_integer() ||
         (value.at("value").is_number_unsigned() && value.at("value").get<uint64_t>()>static_cast<uint64_t>(std::numeric_limits<LONG>::max())) ||
         value.at("value").get<int64_t>()<std::numeric_limits<LONG>::min() ||
         value.at("value").get<int64_t>()>std::numeric_limits<LONG>::max())
        throw capture::Error(NSS_E_ARGUMENT,NSS_STAGE_PLAN,0,"Manual camera control requires an integer device value");
    } else throw capture::Error(NSS_E_ARGUMENT,NSS_STAGE_PLAN,0,"Unknown camera control mode");
  }
}
Controls::Controls(IBaseFilter *source) {
  (void)source->QueryInterface(IID_PPV_ARGS(&camera_));(void)source->QueryInterface(IID_PPV_ARGS(&image_));undo_.reserve(properties.size());
}
Controls::~Controls() {(void)restore();}
HRESULT Controls::get(bool camera,long id,long *value,long *flags) const {
  return camera?(camera_?camera_->Get(id,value,flags):E_NOINTERFACE):(image_?image_->Get(id,value,flags):E_NOINTERFACE);
}
HRESULT Controls::range(bool camera,long id,long *minimum,long *maximum,long *step,long *standard,long *caps) const {
  return camera?(camera_?camera_->GetRange(id,minimum,maximum,step,standard,caps):E_NOINTERFACE):
    (image_?image_->GetRange(id,minimum,maximum,step,standard,caps):E_NOINTERFACE);
}
HRESULT Controls::set(const Setting &s) {
  return s.camera?(camera_?camera_->Set(s.property,s.value,s.flags):E_NOINTERFACE):
    (image_?image_->Set(s.property,s.value,s.flags):E_NOINTERFACE);
}
Json Controls::inventory() const {
  Json result=Json::object();
  for(const auto &p:properties) {
    long minimum{},maximum{},step{},standard{},caps{},value{},flags{};
    if(FAILED(range(p.camera,p.id,&minimum,&maximum,&step,&standard,&caps))) continue;
    const auto status=get(p.camera,p.id,&value,&flags);
    result[p.name]={{"minimum",minimum},{"maximum",maximum},{"step",step},{"default",standard},
      {"supportsAuto",bool(caps&CameraControl_Flags_Auto)},{"supportsManual",bool(caps&CameraControl_Flags_Manual)},
      {"currentValue",value},{"currentMode",flags&CameraControl_Flags_Auto?"auto":"manual"},{"readHresult",status}};
  }
  return result;
}
void Controls::apply(const Json &request) {
  validate_controls(request);
  for(const auto &item:request.items()) {
    const auto &p=property(item.key());long minimum{},maximum{},step{},standard{},caps{},before{},before_flags{};
    checked(range(p.camera,p.id,&minimum,&maximum,&step,&standard,&caps),"Requested camera control is unavailable");
    checked(get(p.camera,p.id,&before,&before_flags),"Cannot preserve the previous camera control");
    const bool automatic=item.value().at("mode")=="auto";
    const long flags=automatic?CameraControl_Flags_Auto:CameraControl_Flags_Manual;
    const long value=automatic?before:item.value().at("value").get<long>();
    if(!(caps&flags) || (!automatic && (value<minimum || value>maximum || step<=0 || (int64_t(value)-minimum)%step)))
      throw capture::Error(NSS_E_UNAVAILABLE,NSS_STAGE_CAPTURE,E_INVALIDARG,"Requested camera control value/mode is not supported by this device");
    undo_.push_back({p.camera,p.id,before,before_flags});
    checked(set({p.camera,p.id,value,flags}),"Camera driver rejected the requested control");
    long actual{},actual_flags{};checked(get(p.camera,p.id,&actual,&actual_flags),"Camera control readback failed");
    if(!(actual_flags&flags) || (!automatic && actual!=value))
      throw capture::Error(NSS_E_IO,NSS_STAGE_CAPTURE,E_FAIL,"Camera driver silently changed requested control value/mode");
    applied_.push_back({{"name",p.name},{"requested",item.value()},{"actualValue",actual},{"actualFlags",actual_flags},
      {"previousValue",before},{"previousFlags",before_flags}});
    requested_.push_back({p.camera,p.id,value,flags});
  }
}
void Controls::apply_streaming() {
  // Some USB drivers acknowledge controls before Run but reinitialize their
  // exposure when the stream starts. Reassert only the caller's explicit
  // request after the first real sample; keep the original rollback values.
  for(size_t index=0;index<requested_.size();++index) {
    const auto &setting=requested_[index];
    checked(set(setting),"Camera streaming control could not be applied");
    long actual{},flags{};
    checked(get(setting.camera,setting.property,&actual,&flags),"Camera streaming control could not be verified");
    if((flags&3)!=setting.flags || (!(flags&CameraControl_Flags_Auto) && actual!=setting.value))
      throw capture::Error(NSS_E_IO,NSS_STAGE_CAPTURE,E_FAIL,"Camera streaming control differs from the explicit request");
    applied_[index]["streamingActualValue"]=actual;
    applied_[index]["streamingActualFlags"]=flags;
    applied_[index]["reassertedAfterFirstSample"]=true;
  }
}
HRESULT Controls::restore() noexcept {
  for(auto it=undo_.rbegin();it!=undo_.rend();++it) {
    const auto result=set(*it);if(FAILED(result) && SUCCEEDED(restore_error_)) restore_error_=result;
  }
  undo_.clear();return restore_error_;
}
Json Controls::metrics() const {return {{"applied",applied_},{"restoreOnClose",true},{"restoreHresult",restore_error_}};}
} // namespace nss::camera::detail
