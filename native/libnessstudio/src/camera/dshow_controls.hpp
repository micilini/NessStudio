#pragma once
#include "dshow_types.hpp"

namespace nss::camera::detail {
void validate_controls(const Json &);
class Controls final {
  struct Setting {bool camera{};long property{},value{},flags{};};
  ComPtr<IAMCameraControl> camera_;
  ComPtr<IAMVideoProcAmp> image_;
  std::vector<Setting> undo_, requested_;
  Json applied_=Json::array();
  HRESULT restore_error_{S_OK};
  HRESULT get(bool,long,long *,long *) const;
  HRESULT range(bool,long,long *,long *,long *,long *,long *) const;
  HRESULT set(const Setting &);
public:
  explicit Controls(IBaseFilter *);
  ~Controls();
  Json inventory() const;
  void apply(const Json &);
  void apply_streaming();
  HRESULT restore() noexcept;
  Json metrics() const;
};
} // namespace nss::camera::detail
